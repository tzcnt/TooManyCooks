// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

// Modified from tmc::channel, making the consumer side non-atomic,
// and removing consumer suspension. Removed close() logic, as it's expected
// for all tasks to be consumed from the executor before shutdown.

#include "tmc/detail/compat.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <type_traits>
#include <utility>

namespace tmc {
namespace detail {
// Allocates elements without constructing them, to be constructed later using
// placement new. T need not be default, copy, or move constructible.
// The caller must track whether the element exists, and manually invoke the
// destructor if necessary.
template <typename T> struct qu_mpsc_storage {
  union alignas(alignof(T)) {
    T value;
  };
#ifndef NDEBUG
  bool exists = false;
#endif

  qu_mpsc_storage() noexcept {}

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
  qu_mpsc_storage(qu_mpsc_storage&& Other) noexcept {
    emplace(static_cast<T&&>(Other.value));
    Other.destroy();
  }
  qu_mpsc_storage& operator=(qu_mpsc_storage&& Other) noexcept {
    emplace(static_cast<T&&>(Other.value));
    Other.destroy();
    return *this;
  }

  // If data was present, the caller is responsible for destroying it.
#ifndef NDEBUG
  ~qu_mpsc_storage() { assert(!exists); }
#else
  ~qu_mpsc_storage()
    requires(std::is_trivially_destructible_v<T>)
  = default;
  ~qu_mpsc_storage()
    requires(!std::is_trivially_destructible_v<T>)
  {}
#endif

  qu_mpsc_storage(const qu_mpsc_storage&) = delete;
  qu_mpsc_storage& operator=(const qu_mpsc_storage&) = delete;
};

struct qu_mpsc_default_config {
  /// The number of elements that can be stored in each block in the qu_mpsc
  /// linked list.
  static inline constexpr size_t BlockSize = 4096;

  /// At level 0, queue elements will be padded up to the next increment of 64
  /// bytes. This reduces false sharing between neighboring elements.
  /// At level 1, no padding will be applied.
  static inline constexpr size_t PackingLevel = 0;

  /// If true, the first storage block will be a member of the qu_mpsc object
  /// (instead of dynamically allocated). Subsequent storage blocks are always
  /// dynamically allocated. Incompatible with set_reuse_blocks(false).
  static inline constexpr bool EmbedFirstBlock = false;
};

template <typename T, typename Config = tmc::detail::qu_mpsc_default_config>
class qu_mpsc {
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
  class element_t {
    static inline constexpr size_t DATA_BIT = TMC_ONE_BIT;
    std::atomic<size_t> flags;

  public:
    tmc::detail::qu_mpsc_storage<T> data;

    static constexpr size_t UNPADLEN =
      sizeof(size_t) + sizeof(void*) + sizeof(tmc::detail::qu_mpsc_storage<T>);
    static constexpr size_t WANTLEN = (UNPADLEN + TMC_CACHE_LINE_SIZE - 1) &
                                      static_cast<size_t>(
                                        -TMC_CACHE_LINE_SIZE
                                      ); // round up to TMC_CACHE_LINE_SIZE
    static constexpr size_t PADLEN =
      UNPADLEN < WANTLEN ? (WANTLEN - UNPADLEN) : 999;

    struct empty {};
    using Padding = std::conditional_t<
      Config::PackingLevel == 0 && PADLEN != 999, char[PADLEN], empty>;
    TMC_NO_UNIQUE_ADDRESS Padding pad;

    void set_data_ready() noexcept {
      flags.store(DATA_BIT, std::memory_order_release);
    }

    bool is_data_waiting() noexcept {
      return DATA_BIT == flags.load(std::memory_order_acquire);
    }

    void reset() noexcept { flags.store(0, std::memory_order_relaxed); }
  };

  using element = element_t;
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

  static_assert(std::atomic<size_t>::is_always_lock_free);
  static_assert(std::atomic<data_block*>::is_always_lock_free);

  char pad0[TMC_CACHE_LINE_SIZE - sizeof(size_t)];
  std::atomic<size_t> write_offset;
  char pad1[TMC_CACHE_LINE_SIZE - sizeof(size_t)];
  size_t read_offset;
  char pad2[TMC_CACHE_LINE_SIZE - sizeof(size_t)];

  std::atomic<data_block*> write_block;
  data_block* head_block;
  data_block* tail_block;

  data_block* pending_reclaim_old_head;
  data_block* pending_reclaim_new_head;
  size_t pending_reclaim_cutoff;

  struct empty {};
  using EmbeddedBlock =
    std::conditional_t<Config::EmbedFirstBlock, data_block, empty>;
  TMC_NO_UNIQUE_ADDRESS EmbeddedBlock embedded_block;

public:
  qu_mpsc() noexcept {
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
    pending_reclaim_old_head = nullptr;
    pending_reclaim_new_head = nullptr;
    pending_reclaim_cutoff = 0;
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

  void try_start_reclaim() noexcept {
    if (pending_reclaim_old_head != nullptr) {
      return;
    }

    data_block* oldHead = head_block;
    size_t protectIdx = read_offset & ~BlockSizeMask;
    size_t oldOff = oldHead->offset.load(std::memory_order_relaxed);
    if (!circular_less_than(oldOff, protectIdx)) {
      return;
    }

    data_block* newHead = find_block(oldHead, protectIdx);

    // This seq_cst store and the following seq_cst write_offset load form the
    // cutoff protocol with producers, which do a seq_cst fetch_add before a
    // seq_cst load of write_block. A producer that observes the old write_block
    // must have a reservation included in the cutoff.
    write_block.store(newHead, std::memory_order_seq_cst);
    pending_reclaim_cutoff = write_offset.load(std::memory_order_seq_cst);
    pending_reclaim_old_head = oldHead;
    pending_reclaim_new_head = newHead;
  }

  void try_reclaim_blocks() noexcept {
    try_finish_pending_reclaim();
    try_start_reclaim();
    try_finish_pending_reclaim();
  }

  // Idx will be initialized by this function
  element* get_write_ticket(size_t& Idx) noexcept {
    // seq_cst is needed here so the reader can order its write_block update and
    // subsequent write_offset load against the producer's reservation and
    // write_block load.
    Idx = write_offset.fetch_add(1, std::memory_order_seq_cst);
    data_block* block = write_block.load(std::memory_order_seq_cst);

    size_t boff = block->offset.load(std::memory_order_relaxed);
    assert(circular_less_than(boff, 1 + Idx));

    block = find_block(block, Idx);
    element* elem = &block->values[Idx & BlockSizeMask];
    return elem;
  }

  // StartIdx and EndIdx will be initialized by this function
  data_block* get_write_ticket_bulk(
    size_t Count, size_t& StartIdx, size_t& EndIdx
  ) noexcept {
    // seq_cst is needed here so the reader can order its write_block update and
    // subsequent write_offset load against the producer's reservation and
    // write_block load.
    StartIdx = write_offset.fetch_add(Count, std::memory_order_seq_cst);
    EndIdx = StartIdx + Count;
    data_block* block = write_block.load(std::memory_order_seq_cst);

    if (Count == 0) [[unlikely]] {
      return block;
    }

    [[maybe_unused]] size_t boff =
      block->offset.load(std::memory_order_relaxed);
    assert(circular_less_than(boff, 1 + StartIdx));

    // Ensure all blocks for the operation are allocated and available.
    data_block* startBlock = find_block(block, StartIdx);
    find_block(startBlock, EndIdx - 1);
    return startBlock;
  }

public:
  template <typename U> void post(U&& Val) noexcept {
    // Get write ticket and associated block.
    size_t idx;
    element* elem = get_write_ticket(idx);

    elem->data.emplace(static_cast<U&&>(Val));
    elem->set_data_ready();
  }

  template <typename It>
  void post_bulk(It&& Items, size_t Count) noexcept {
    // Get write ticket and associated block.
    size_t startIdx, endIdx;
    data_block* block = get_write_ticket_bulk(Count, startIdx, endIdx);

    size_t idx = startIdx;
    while (idx < endIdx) {
      element* elem = &block->values[idx & BlockSizeMask];

      TMC_DISABLE_WARNING_PESSIMIZING_MOVE_BEGIN
      elem->data.emplace(std::move(*Items));
      TMC_DISABLE_WARNING_PESSIMIZING_MOVE_END
      elem->set_data_ready();

      ++Items;
      ++idx;
      if ((idx & BlockSizeMask) == 0) {
        block = block->next.load(std::memory_order_acquire);
        // all blocks should have been preallocated for [startIdx, endIdx)
        assert(block != nullptr || idx >= endIdx);
      }
    }
  }

  // No hazard pointer needed if this is only called from the single consumer
  bool empty() {
    // need a StoreLoad barrier to ensure we see the queue is empty (?)
    tmc::detail::memory_barrier();

    size_t Idx = read_offset;
    data_block* block = find_block(head_block, Idx);
    element* elem = &block->values[Idx & BlockSizeMask];

    bool isEmpty = !elem->is_data_waiting();
    return isEmpty;
  }

  bool try_pull(T& output) {
    size_t Idx = read_offset;
    data_block* block = head_block;

    [[maybe_unused]] size_t boff =
      block->offset.load(std::memory_order_relaxed);
    assert(circular_less_than(boff, 1 + Idx));

    block = find_block(block, Idx);
    element* elem = &block->values[Idx & BlockSizeMask];

    if (elem->is_data_waiting()) {
      // Data is already ready here.
      output = std::move(elem->data.value);
      elem->data.destroy();
      read_offset = Idx + 1;
      try_reclaim_blocks();
      return true;
    }
    return false;
  }

  ~qu_mpsc() {
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

  qu_mpsc(const qu_mpsc&) = delete;
  qu_mpsc& operator=(const qu_mpsc&) = delete;
  qu_mpsc(qu_mpsc&&) = delete;
  qu_mpsc& operator=(qu_mpsc&&) = delete;
};

} // namespace detail
} // namespace tmc
