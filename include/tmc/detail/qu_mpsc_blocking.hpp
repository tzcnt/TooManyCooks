// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

// Unbounded MPSC queue using linked list of blocks. Uses a similar fetch-add
// slot acquisition scheme to tmc::channel, but with various changes:
// - consumers are single-threaded, so read offset does not need to be atomic
// - queue cannot be closed
// - single consumer's offset is non-atomic

// Instead of hazard pointers, uses a quiescent-state based reclamation scheme:
// 1. Producers reserve tickets with write_offset, then load write_block.
// 2. The consumer enters a new block and publishes it as the new write_block.
// 3. The consumer snapshots write_offset as the reclaim cutoff.
// 4. Once read_offset reaches that cutoff, producers that may have observed the
//    old write_block are done, so old blocks can be recycled.
// This scheme works only with single-consumer queues since we can be sure that
// after the consumer reached the cutoff, there are guaranteed to be no other
// users of the old blocks.

// This version is modified to allow callers to block. It is co-designed with
// ex_cpu_st for an efficient blocking / reference counting scheme.

#include "tmc/detail/compat.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <type_traits>
#include <utility>

#ifdef __linux__
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace tmc {
namespace detail {
// Allocates elements without constructing them, to be constructed later using
// placement new. T need not be default, copy, or move constructible.
// The caller must track whether the element exists, and manually invoke the
// destructor if necessary.
template <typename T> struct qu_mpsc_blocking_storage {
  union alignas(alignof(T)) {
    T value;
  };
#ifndef NDEBUG
  bool exists = false;
#endif

  qu_mpsc_blocking_storage() noexcept {}

  template <typename... ConstructArgs>
  void emplace(ConstructArgs&&... Args) noexcept {
#ifndef NDEBUG
    assert(!exists);
    exists = true;
#endif
    ::new (static_cast<void*>(&value)) T(static_cast<ConstructArgs&&>(Args)...);
  }

  void destroy() noexcept {
#ifndef NDEBUG
    assert(exists);
    exists = false;
#endif
    value.~T();
  }

  // Precondition: Other.value must exist
  qu_mpsc_blocking_storage(qu_mpsc_blocking_storage&& Other) noexcept {
    emplace(static_cast<T&&>(Other.value));
    Other.destroy();
  }
  qu_mpsc_blocking_storage&
  operator=(qu_mpsc_blocking_storage&& Other) noexcept {
    emplace(static_cast<T&&>(Other.value));
    Other.destroy();
    return *this;
  }

  // If data was present, the caller is responsible for destroying it.
#ifndef NDEBUG
  ~qu_mpsc_blocking_storage() { assert(!exists); }
#else
  ~qu_mpsc_blocking_storage()
    requires(std::is_trivially_destructible_v<T>)
  = default;
  ~qu_mpsc_blocking_storage()
    requires(!std::is_trivially_destructible_v<T>)
  {}
#endif

  qu_mpsc_blocking_storage(const qu_mpsc_blocking_storage&) = delete;
  qu_mpsc_blocking_storage& operator=(const qu_mpsc_blocking_storage&) = delete;
};

struct qu_mpsc_blocking_default_config {
  /// The number of elements that can be stored in each block in the
  /// qu_mpsc_blocking linked list.
  static inline constexpr size_t BlockSize = 4096;

  /// At level 0, queue elements will be padded up to the next increment of 64
  /// bytes. This reduces false sharing between neighboring elements.
  /// At level 1, no padding will be applied.
  static inline constexpr size_t PackingLevel = 0;

  /// If true, the first storage block will be a member of the qu_mpsc_blocking
  /// object (instead of dynamically allocated). Subsequent storage blocks are
  /// always dynamically allocated.
  static inline constexpr bool EmbedFirstBlock = false;
};

template <
  typename T, typename Config = tmc::detail::qu_mpsc_blocking_default_config>
class qu_mpsc_blocking {
  static inline constexpr size_t BlockSize = Config::BlockSize;
  static inline constexpr size_t BlockSizeMask = BlockSize - 1;
  static_assert(
    BlockSize && ((BlockSize & (BlockSize - 1)) == 0),
    "BlockSize must be a power of 2"
  );

  // Ensure that the subtraction of unsigned offsets always results in a value
  // that can be represented as a signed integer.
  static_assert(
    BlockSize <= (TMC_ONE_BIT << (TMC_PLATFORM_BITS - 1)),
    "BlockSize must not be larger than half the max value that can be "
    "represented by a platform word"
  );

  // Implementing handling for throwing construction is not possible with the
  // current design.
  static_assert(std::is_nothrow_move_constructible_v<T>);

private:
  struct element {
    static inline constexpr tmc::detail::atomic_wait_t EMPTY = 0;
    static inline constexpr tmc::detail::atomic_wait_t WAITING = 1;
    static inline constexpr tmc::detail::atomic_wait_t DATA = 2;
    std::atomic<tmc::detail::atomic_wait_t> flags;

    tmc::detail::qu_mpsc_blocking_storage<T> data;

    static constexpr size_t UNPADLEN =
      sizeof(std::atomic<tmc::detail::atomic_wait_t>) +
      sizeof(tmc::detail::qu_mpsc_blocking_storage<T>);
    static constexpr size_t WANTLEN = (UNPADLEN + TMC_CACHE_LINE_SIZE - 1) &
                                      static_cast<size_t>(
                                        0 - TMC_CACHE_LINE_SIZE
                                      ); // round up to TMC_CACHE_LINE_SIZE
    static constexpr size_t PADLEN =
      UNPADLEN < WANTLEN ? (WANTLEN - UNPADLEN) : 999;

    struct empty {};
    using Padding = std::conditional_t<
      Config::PackingLevel == 0 && PADLEN != 999, char[PADLEN], empty>;
    TMC_NO_UNIQUE_ADDRESS Padding pad;

    static constexpr tmc::detail::atomic_wait_t WAIT_VALUE = WAITING;

    tmc::detail::atomic_wait_t* set_waiting_or_get_wait_address() noexcept {
      tmc::detail::atomic_wait_t prev =
        flags.exchange(WAITING, std::memory_order_seq_cst);
      if (prev != DATA) {
        return reinterpret_cast<tmc::detail::atomic_wait_t*>(&flags);
      }
      return nullptr;
    }

    bool is_data_waiting() noexcept {
      return DATA == flags.load(std::memory_order_acquire);
    }

    void reset() noexcept { flags.store(EMPTY, std::memory_order_relaxed); }
  };

  static_assert(Config::PackingLevel < 2);

  struct data_block {
    std::atomic<size_t> offset;
    std::atomic<data_block*> next;
    std::array<element, BlockSize> values;

    void reset_values() noexcept {
      for (size_t i = 0; i < BlockSize; ++i) {
        values[i].reset();
      }
    }

    data_block(size_t Offset) noexcept {
      offset.store(Offset, std::memory_order_relaxed);
      next.store(nullptr, std::memory_order_relaxed);
      reset_values();
    }

    data_block() noexcept : data_block(0) {}
  };

  char pad0[TMC_CACHE_LINE_SIZE - sizeof(size_t)];
  std::atomic<size_t> write_offset;
  char pad1[TMC_CACHE_LINE_SIZE - sizeof(size_t)];
  size_t read_offset;
  data_block* read_block;
  char pad2[TMC_CACHE_LINE_SIZE - sizeof(size_t) - sizeof(data_block*)];

  std::atomic<data_block*> write_block;
  data_block* head_block;
  data_block* tail_block;

  data_block* pending_reclaim_old_head;
  data_block* pending_reclaim_new_head;
  size_t pending_reclaim_cutoff;

#ifndef __linux__
  std::atomic<tmc::detail::atomic_wait_t>* wake_wait;
#endif

  struct empty {};
  using EmbeddedBlock =
    std::conditional_t<Config::EmbedFirstBlock, data_block, empty>;
  TMC_NO_UNIQUE_ADDRESS EmbeddedBlock embedded_block;

public:
  class aw_pull;

  // Used as a reference count to prevent racing between producer syscall and
  // consumer teardown. On Linux, counts only "failed wakes" - producer wake
  // operations that observed a waiting consumer but woke no kernel waiter. The
  // single consumer accounts for these before running the corresponding item;
  // owners may wait for both sides to match before destroying the queue
  // storage. On other OSes, counts all wakes, since the OS APIs don't provide
  // the necessary information, AND the OS APIs don't support user-space
  // multi-wait, instead requiring the use of kernel objects. So we just use
  // C++20 standard std::atomic::wait on a single value shared across multiple
  // queues.
  std::atomic<size_t> wake_ref_count;

  qu_mpsc_blocking() noexcept {
    data_block* block;
    if constexpr (Config::EmbedFirstBlock) {
      block = &embedded_block;
    } else {
      block = new data_block(0);
    }
    head_block = block;
    write_block.store(block, std::memory_order_relaxed);
    tail_block = block;
    write_offset.store(0, std::memory_order_relaxed);
    read_offset = 0;
    read_block = block;
    pending_reclaim_old_head = nullptr;
    pending_reclaim_new_head = nullptr;
    pending_reclaim_cutoff = 0;
#ifndef __linux__
    wake_wait = nullptr;
#endif
    wake_ref_count.store(0, std::memory_order_relaxed);
    tmc::detail::memory_barrier();
  }

private:
  static inline bool circular_less_than(size_t a, size_t b) noexcept {
    return a - b > (TMC_ONE_BIT << (TMC_PLATFORM_BITS - 1));
  }

  void reclaim_blocks(data_block* OldHead, data_block* NewHead) noexcept {
    // Reset blocks and move them to the tail of the list in groups of 4.
    while (true) {
      std::array<data_block*, 4> unlinked;
      size_t unlinkedCount = 0;
      for (; unlinkedCount < unlinked.size(); ++unlinkedCount) {
        if (OldHead == NewHead) {
          break;
        }
        unlinked[unlinkedCount] = OldHead;
        OldHead = OldHead->next.load(std::memory_order_acquire);
      }
      if (unlinkedCount == 0) {
        break;
      }

      for (size_t i = 0; i < unlinkedCount; ++i) {
        unlinked[i]->reset_values();
      }

      data_block* tailBlock = tail_block;
      data_block* next = tailBlock->next.load(std::memory_order_acquire);

      // Iterate forward in case tailBlock is part of unlinked.
      while (next != nullptr) {
        tailBlock = next;
        next = tailBlock->next.load(std::memory_order_acquire);
      }
      // Actually unlink the blocks from the head of the queue.
      // They stay linked to each other.
      unlinked[unlinkedCount - 1]->next.store(
        nullptr, std::memory_order_release
      );

      while (true) {
        // Update their offsets to the end of the queue.
        size_t boff =
          tailBlock->offset.load(std::memory_order_relaxed) + BlockSize;
        for (size_t i = 0; i < unlinkedCount; ++i) {
          unlinked[i]->offset.store(boff, std::memory_order_relaxed);
          boff += BlockSize;
        }

        // Re-link the tail of the queue to the head of the unlinked blocks.
        if (tailBlock->next.compare_exchange_strong(
              next, unlinked[0], std::memory_order_acq_rel,
              std::memory_order_acquire
            )) {
          break;
        }

        // Tail was out of date, find the new tail.
        while (next != nullptr) {
          tailBlock = next;
          next = tailBlock->next.load(std::memory_order_acquire);
        }
      }

      tail_block = unlinked[unlinkedCount - 1];
    }
  }

  // Given idx and a starting block, advance it until the block containing idx
  // is found.
  static inline data_block* find_block(data_block* Block, size_t Idx) noexcept {
    size_t offset = Block->offset.load(std::memory_order_relaxed);
    size_t targetOffset = Idx & ~BlockSizeMask;
    // Find or allocate the associated block
    while (offset != targetOffset) {
      data_block* next = Block->next.load(std::memory_order_acquire);
      if (next == nullptr) {
        data_block* newBlock = new data_block(offset + BlockSize);
        if (Block->next.compare_exchange_strong(
              next, newBlock, std::memory_order_acq_rel,
              std::memory_order_acquire
            )) {
          next = newBlock;
        } else {
          delete newBlock;
        }
      }
      Block = next;
      offset += BlockSize;
      assert(Block->offset.load(std::memory_order_relaxed) == offset);
    }

    assert(
      Idx >= Block->offset.load(std::memory_order_relaxed) &&
      Idx <= Block->offset.load(std::memory_order_relaxed) + BlockSize - 1
    );
    return Block;
  }

  bool try_finish_pending_reclaim() noexcept {
    if (pending_reclaim_old_head == nullptr) {
      return false;
    }

    // The pending blocks were removed from the producer-visible write head
    // before pending_reclaim_cutoff was read from write_offset. Any producer
    // that can still be walking from the old write head must therefore have a
    // reservation before this cutoff. Once the single consumer reaches the
    // cutoff, those producers have published their elements and no longer touch
    // the old blocks.
    if (circular_less_than(read_offset, pending_reclaim_cutoff)) {
      return false;
    }

    data_block* oldHead = pending_reclaim_old_head;
    data_block* newHead = pending_reclaim_new_head;
    head_block = newHead;
    pending_reclaim_old_head = nullptr;
    pending_reclaim_new_head = nullptr;
    reclaim_blocks(oldHead, newHead);
    return true;
  }

  void try_start_reclaim(data_block* NewHead) noexcept {
    if (pending_reclaim_old_head != nullptr) {
      return;
    }

    data_block* oldHead = head_block;
    size_t newHeadOffset = read_offset & ~BlockSizeMask;
    assert(NewHead->offset.load(std::memory_order_relaxed) == newHeadOffset);
    size_t oldOff = oldHead->offset.load(std::memory_order_relaxed);
    if (!circular_less_than(oldOff, newHeadOffset)) {
      return;
    }

    // This seq_cst store and the following seq_cst write_offset load form the
    // cutoff protocol with producers, which do a seq_cst fetch_add before a
    // seq_cst load of write_block. A producer that observes the old write_block
    // must have a reservation included in the cutoff.
    write_block.store(NewHead, std::memory_order_seq_cst);
    pending_reclaim_cutoff = write_offset.load(std::memory_order_seq_cst);
    pending_reclaim_old_head = oldHead;
    pending_reclaim_new_head = NewHead;
  }

  void try_reclaim_blocks(data_block* NewHead) noexcept {
    try_finish_pending_reclaim();
    try_start_reclaim(NewHead);
    try_finish_pending_reclaim();
  }

  // Idx will be initialized by this function
  element* get_write_ticket(size_t& Idx) noexcept {
    // seq_cst is needed here so the reader can order its write_block update and
    // subsequent write_offset load against the producer's reservation and
    // write_block load.
    Idx = write_offset.fetch_add(1, std::memory_order_seq_cst);
    data_block* block = write_block.load(std::memory_order_seq_cst);

    assert(
      circular_less_than(block->offset.load(std::memory_order_relaxed), 1 + Idx)
    );

    block = find_block(block, Idx);
    element* elem = &block->values[Idx & BlockSizeMask];
    return elem;
  }

  element* get_read_ticket(size_t& Idx, data_block*& Block) noexcept {
    Idx = read_offset;
    Block = read_block;

    assert(
      circular_less_than(Block->offset.load(std::memory_order_relaxed), 1 + Idx)
    );

    Block = find_block(Block, Idx);
    return &Block->values[Idx & BlockSizeMask];
  }

  void finish_read(element* Elem, data_block* Block, size_t Idx) noexcept {
    Elem->data.destroy();
    read_offset = Idx + 1;
    // Only try to advance the producer-visible write head once the consumer
    // has entered a new block. Pending reclaim may also complete here; if its
    // cutoff was reached earlier, this delays recycling by at most one block
    // while keeping the per-element hot path small.
    if ((Idx & BlockSizeMask) == 0) {
      read_block = Block;
      try_reclaim_blocks(Block);
    }
  }

  // Returns true if a waiter was found.
  template <typename... Args>
  bool write_element(element* Elem, Args&&... ConstructArgs) noexcept {
    Elem->data.emplace(std::forward<Args>(ConstructArgs)...);

    // On non-Linux, producer does store data -> load waiters on non-Linux. A
    // StoreLoad barrier is required in between to prevent lost wakeups, hence
    // the seq_cst ordering.
    // On Linux, the futex provides the necessary barrier, but in practice
    // seq_cst and acq_rel exchanges are identical on modern x86/ARM, so we
    // use a single ordering for consistency.
    tmc::detail::atomic_wait_t prev =
      Elem->flags.exchange(element::DATA, std::memory_order_seq_cst);
    if (prev != element::WAITING) {
      return false;
    }

#ifdef __linux__
    long wokenCount = syscall(
      SYS_futex, reinterpret_cast<tmc::detail::atomic_wait_t*>(&Elem->flags),
      FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0
    );
    assert(wokenCount >= 0);
    bool didWake = wokenCount != 0;
    if (!didWake) {
      wake_ref_count.fetch_add(1, std::memory_order_release);
    }
    return didWake;
#else
    wake_wait->fetch_add(1, std::memory_order_release);
    wake_wait->notify_one();
    wake_ref_count.fetch_add(1, std::memory_order_release);
    return true;
#endif
  }

  // StartIdx and EndIdx will be initialized by this function.
  // Count must be non-zero (enforced by the caller).
  data_block* get_write_ticket_bulk(
    size_t Count, size_t& StartIdx, size_t& EndIdx
  ) noexcept {
    // seq_cst is needed here so the reader can order its write_block update and
    // subsequent write_offset load against the producer's reservation and
    // write_block load.
    StartIdx = write_offset.fetch_add(Count, std::memory_order_seq_cst);
    EndIdx = StartIdx + Count;
    data_block* block = write_block.load(std::memory_order_seq_cst);

    assert(circular_less_than(
      block->offset.load(std::memory_order_relaxed), 1 + StartIdx
    ));

    // Ensure all blocks for the operation are allocated and available.
    data_block* startBlock = find_block(block, StartIdx);
    find_block(startBlock, EndIdx - 1);
    return startBlock;
  }

public:
  using wait_ptr = tmc::detail::atomic_wait_t*;
  static constexpr tmc::detail::atomic_wait_t WAIT_VALUE = element::WAIT_VALUE;

#ifndef __linux__
  void
  set_wake_wait(std::atomic<tmc::detail::atomic_wait_t>& WakeWait) noexcept {
    wake_wait = &WakeWait;
  }
#endif

  // Returns true if a waiter was found.
  template <typename U> bool post(U&& Val) noexcept {
    // Get write ticket and associated block.
    size_t idx;
    element* elem = get_write_ticket(idx);

    return write_element(elem, static_cast<U&&>(Val));
  }

  // Returns true if a waiter was found.
  template <typename It> bool post_bulk(It&& Items, size_t Count) noexcept {
    if (Count == 0) [[unlikely]] {
      return false;
    }

    // Get write ticket and associated block.
    size_t startIdx, endIdx;
    data_block* block = get_write_ticket_bulk(Count, startIdx, endIdx);

    size_t idx = startIdx;
    bool didWake = false;
    while (idx < endIdx) {
      element* elem = &block->values[idx & BlockSizeMask];

      TMC_DISABLE_WARNING_PESSIMIZING_MOVE_BEGIN
      didWake |= write_element(elem, std::move(*Items));
      TMC_DISABLE_WARNING_PESSIMIZING_MOVE_END

      ++Items;
      ++idx;
      if ((idx & BlockSizeMask) == 0) {
        block = block->next.load(std::memory_order_acquire);
        // all blocks should have been preallocated for [startIdx, endIdx)
        assert(block != nullptr || idx >= endIdx);
      }
    }
    return didWake;
  }

  // Only safe to call from the single consumer.
  bool empty() {
    size_t Idx = read_offset;
    data_block* block = find_block(read_block, Idx);
    element* elem = &block->values[Idx & BlockSizeMask];

    bool isEmpty = !elem->is_data_waiting();
    return isEmpty;
  }

  /// Attempts to pull a T into Output.
  ///
  /// Returns nullptr if a value was pulled into Output. Otherwise, returns the
  /// address of the current element's wait word, which the caller may block on
  /// until it differs from WAIT_VALUE. qu_mpsc_blocking has no close operation,
  /// so callers that need to terminate a consumer loop should post a sentinel
  /// value.
  [[nodiscard]] wait_ptr pull(T& Output) noexcept {
    static_assert(std::is_nothrow_move_constructible_v<T>);

    size_t Idx;
    data_block* block;
    element* elem = get_read_ticket(Idx, block);

    if (!elem->is_data_waiting()) {
      wait_ptr waitAddress = elem->set_waiting_or_get_wait_address();
      if (waitAddress != nullptr) {
        return waitAddress;
      }
    }

    Output = std::move(elem->data.value);
    finish_read(elem, block, Idx);
    return nullptr;
  }

  bool try_pull(T& output) {
    size_t Idx;
    data_block* block;
    element* elem = get_read_ticket(Idx, block);

    if (elem->is_data_waiting()) {
      // Data is already ready here.
      output = std::move(elem->data.value);
      finish_read(elem, block, Idx);
      return true;
    }
    return false;
  }

  ~qu_mpsc_blocking() {
    {
      size_t woff = write_offset.load(std::memory_order_relaxed);
      size_t idx = read_offset;
      data_block* block = head_block;
      while (circular_less_than(idx, woff)) {
        block = find_block(block, idx);
        element* elem = &block->values[idx & BlockSizeMask];
        if (elem->is_data_waiting()) {
          elem->data.destroy();
        }
        ++idx;
      }
    }
    {
      data_block* block = head_block;
      while (block != nullptr) {
        data_block* next = block->next.load(std::memory_order_acquire);
        if constexpr (Config::EmbedFirstBlock) {
          if (block != &embedded_block) {
            delete block;
          }
        } else {
          delete block;
        }
        block = next;
      }
    }
  }

  qu_mpsc_blocking(const qu_mpsc_blocking&) = delete;
  qu_mpsc_blocking& operator=(const qu_mpsc_blocking&) = delete;
  qu_mpsc_blocking(qu_mpsc_blocking&&) = delete;
  qu_mpsc_blocking& operator=(qu_mpsc_blocking&&) = delete;
};

} // namespace detail
} // namespace tmc
