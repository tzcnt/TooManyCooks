// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

// Modified from tmc::channel, making the consumer side non-atomic,
// and removing consumer suspension. Removed close() logic, as it's expected
// for all tasks to be consumed from the executor before shutdown.

#include "tmc/detail/bitmap_object_pool.hpp"
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

  // An offset far enough forward that it won't protect anything for a very long
  // time, but close enough that it isn't considered "circular less than" 0.
  // On 32 bit this is only 1Gi elements. The worst case is that a
  // thread suspends for a very long time, and the queue processes 1Gi
  // elements and then cannot free any blocks until that thread wakes. This is
  // extremely unlikely, and not an error - it will just prevent block
  // reclamation. On 64 bit in practice this will never happen.
  static inline constexpr size_t InactiveHazptrOffset =
    TMC_ONE_BIT << (TMC_PLATFORM_BITS - 2);

private:
  class element_t {
    static inline constexpr size_t DATA_BIT = TMC_ONE_BIT;
    std::atomic<size_t> flags;

  public:
    tmc::detail::qu_mpsc_storage<T> data;

    static constexpr size_t UNPADLEN =
      sizeof(size_t) + sizeof(void*) + sizeof(tmc::detail::qu_mpsc_storage<T>);
    static constexpr size_t WANTLEN =
      (UNPADLEN + 63) & static_cast<size_t>(-64); // round up to 64
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

  class alignas(64) hazard_ptr {
    std::atomic<size_t> active_offset;
    std::atomic<data_block*> write_block;
    std::atomic<size_t> next_protect_write;

    friend class qu_mpsc;
    friend class tmc::detail::BitmapObjectPool<hazard_ptr>;

    void release_blocks() noexcept {
      // These elements may be read (by try_reclaim_block()) after
      // acquire_scoped_wfpg() has been called, but before init() has been
      // called. These defaults ensure sane behavior.
      write_block.store(nullptr, std::memory_order_relaxed);
    }

    hazard_ptr() noexcept {
      active_offset.store(InactiveHazptrOffset, std::memory_order_relaxed);
      release_blocks();
    }

    void init(data_block* head) noexcept {
      size_t headOff = head->offset.load(std::memory_order_relaxed);
      next_protect_write.store(headOff, std::memory_order_relaxed);
      active_offset.store(
        headOff + InactiveHazptrOffset, std::memory_order_relaxed
      );
      write_block.store(head, std::memory_order_relaxed);
    }
    TMC_DISABLE_WARNING_PADDED_BEGIN
  };
  TMC_DISABLE_WARNING_PADDED_END

  static_assert(std::atomic<size_t>::is_always_lock_free);
  static_assert(std::atomic<data_block*>::is_always_lock_free);

  // Written by get_hazard_ptr()
  std::atomic<size_t> haz_ptr_counter;
  tmc::detail::BitmapObjectPool<hazard_ptr> hazard_ptr_pool;

  char pad0[64];
  std::atomic<size_t> write_offset;
  char pad1[64];
  size_t read_offset;
  char pad2[64];

  std::atomic<size_t> reclaim_counter;
  std::atomic<data_block*> head_block;
  data_block* tail_block;

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
    head_block.store(block, std::memory_order_relaxed);
    tail_block = block;
    write_offset.store(0, std::memory_order_relaxed);
    read_offset = 0;

    haz_ptr_counter.store(0, std::memory_order_relaxed);
    reclaim_counter.store(0, std::memory_order_relaxed);
    tmc::detail::memory_barrier();
  }

private:
  static inline bool circular_less_than(size_t a, size_t b) noexcept {
    return a - b > (TMC_ONE_BIT << (TMC_PLATFORM_BITS - 1));
  }

  // Load src and move it into dst if src < dst.
  static inline void
  keep_min(size_t& Dst, std::atomic<size_t> const& Src) noexcept {
    size_t val = Src.load(std::memory_order_acquire);
    if (circular_less_than(val, Dst)) {
      Dst = val;
    }
  }

  // Move src into dst if src < dst.
  static inline void keep_min(size_t& Dst, size_t Src) noexcept {
    if (circular_less_than(Src, Dst)) {
      Dst = Src;
    }
  }

  // Advances DstBlock to be equal to NewHead. Possibly reduces MinProtect if
  // DstBlock was already updated by its owning thread.
  static inline void try_advance_hazptr_block(
    std::atomic<data_block*>& DstBlock, size_t& MinProtected,
    data_block* NewHead, std::atomic<size_t> const& HazardOffset
  ) noexcept {
    data_block* block = DstBlock.load(std::memory_order_acquire);
    if (block == nullptr) {
      // A newly owned hazptr. It will reload the value of head after this
      // reclaim operation completes, or cause the entire reclaim operation to
      // be abandoned. In either case, we don't need to update it here.
      // May also be a newly released hazptr, in which case we don't want to
      // overwrite the value of block either.
      return;
    }
    if (circular_less_than(
          block->offset.load(std::memory_order_relaxed),
          NewHead->offset.load(std::memory_order_relaxed)
        )) {
      if (!DstBlock.compare_exchange_strong(
            block, NewHead, std::memory_order_seq_cst
          )) {
        if (block == nullptr) {
          // A newly released hazptr.
          return;
        }
        // If this hazptr updated its own block, but the updated block is
        // still earlier than the new head, then we cannot free that block.
        keep_min(MinProtected, block->offset.load(std::memory_order_relaxed));
      }
      // Reload hazptr after trying to modify block to ensure that if it was
      // written, its value is seen.
      keep_min(MinProtected, HazardOffset);
    }
  }

  // Starting from OldHead, advance forward through the block list, stopping at
  // the first block that is protected by a hazard pointer. This block is
  // returned to become the NewHead. If OldHead is protected, then it will be
  // returned unchanged, and no blocks can be reclaimed.
  data_block*
  try_advance_head(data_block* OldHead, size_t ProtectIdx) noexcept {
    // In the current implementation, this is called only from consumers.
    // Therefore, this token's hazptr will be active, and protecting read_block.
    // However, if producers are lagging behind, and no producer is currently
    // active, write_block would not be protected. Therefore, write_offset
    // should be passed to ProtectIdx to cover this scenario.
    ProtectIdx = ProtectIdx & ~BlockSizeMask; // round down to block index

    // Find the lowest offset that is protected by ProtectIdx or any hazptr.
    size_t oldOff = OldHead->offset.load(std::memory_order_relaxed);
    hazard_ptr_pool.for_each_in_use(
      [&]() { return circular_less_than(oldOff, ProtectIdx); },
      [&](hazard_ptr& curr) { keep_min(ProtectIdx, curr.active_offset); }
    );

    // If head block is protected, nothing can be reclaimed.
    if (circular_less_than(ProtectIdx, 1 + oldOff)) {
      return OldHead;
    }

    // Find the block associated with this offset.
    data_block* newHead = OldHead;
    while (circular_less_than(
      newHead->offset.load(std::memory_order_relaxed), ProtectIdx
    )) {
      newHead = newHead->next.load(std::memory_order_acquire);
    }

    // Then update all hazptrs to be at this block or later.
    hazard_ptr_pool.for_each_in_use(
      [&]() { return circular_less_than(oldOff, ProtectIdx); },
      [&](hazard_ptr& curr) {
        try_advance_hazptr_block(
          curr.write_block, ProtectIdx, newHead, curr.active_offset
        );
      }
    );

    // ProtectIdx may have been reduced by the double-check in
    // try_advance_block. If so, reduce newHead as well.
    if (circular_less_than(
          ProtectIdx, newHead->offset.load(std::memory_order_relaxed)
        )) {
      newHead = OldHead;
      while (circular_less_than(
        newHead->offset.load(std::memory_order_relaxed), ProtectIdx
      )) {
        newHead = newHead->next;
      }
    }

#ifndef NDEBUG
    assert(circular_less_than(
      newHead->offset.load(std::memory_order_relaxed), 1 + read_offset
    ));
    assert(circular_less_than(
      newHead->offset.load(std::memory_order_relaxed),
      1 + write_offset.load(std::memory_order_acquire)
    ));
#endif
    return newHead;
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

  // Access to this function must be externally synchronized (via blocks_lock).
  // Blocks that are not protected by a hazard pointer will be reclaimed, and
  // head_block will be advanced to the first protected block.
  void try_reclaim_blocks(size_t ProtectIdx) noexcept {
    data_block* oldHead = head_block.load(std::memory_order_acquire);
    // reclaim_counter and haz_ptr_counter behave as a split lock shared with
    // get_hazard_ptr(). If both operations run at the same time, this will
    // abandon its operation before the final stage.
    size_t hazptrCount = haz_ptr_counter.load(std::memory_order_acquire);

    // Perform the private stage of the operation.
    data_block* newHead = try_advance_head(oldHead, ProtectIdx);
    if (newHead == oldHead) {
      return;
    }
    head_block.store(newHead, std::memory_order_release);

    // Signal to get_hazard_ptr() that we updated head_block.
    reclaim_counter.fetch_add(1, std::memory_order_seq_cst);

    // Check if get_hazard_ptr() was running.
    size_t hazptrCheck = haz_ptr_counter.load(std::memory_order_seq_cst);
    if (hazptrCount != hazptrCheck) {
      // A hazard pointer was acquired during try_advance_head().
      // It may have an outdated value of head. Our options are to run
      // try_advance_head() again, or just abandon (rollback) the operation. For
      // now, I've chosen to abandon the operation. This will run again when the
      // next block is allocated.
      head_block.store(oldHead, std::memory_order_release);
      return;
    }
    reclaim_blocks(oldHead, newHead);
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

  // Idx will be initialized by this function
  element* get_write_ticket(hazard_ptr* Haz, size_t& Idx) noexcept {
    size_t actOff = Haz->next_protect_write.load(std::memory_order_relaxed);
    Haz->active_offset.store(actOff, std::memory_order_relaxed);

    // seq_cst is needed here to create a StoreLoad barrier between setting
    // hazptr and loading the block
    Idx = write_offset.fetch_add(1, std::memory_order_seq_cst);
    data_block* block = Haz->write_block.load(std::memory_order_seq_cst);

    size_t boff = block->offset.load(std::memory_order_relaxed);
    assert(circular_less_than(actOff, 1 + Idx));
    assert(circular_less_than(boff, 1 + Idx));

    block = find_block(block, Idx);
    // Update last known block.
    Haz->write_block.store(block, std::memory_order_release);
    Haz->next_protect_write.store(boff, std::memory_order_relaxed);
    element* elem = &block->values[Idx & BlockSizeMask];
    return elem;
  }

  // StartIdx and EndIdx will be initialized by this function
  data_block* get_write_ticket_bulk(
    hazard_ptr* Haz, size_t Count, size_t& StartIdx, size_t& EndIdx
  ) noexcept {
    size_t actOff = Haz->next_protect_write.load(std::memory_order_relaxed);
    Haz->active_offset.store(actOff, std::memory_order_relaxed);

    // seq_cst is needed here to create a StoreLoad barrier between setting
    // hazptr and loading the block
    StartIdx = write_offset.fetch_add(Count, std::memory_order_seq_cst);
    EndIdx = StartIdx + Count;
    data_block* block = Haz->write_block.load(std::memory_order_seq_cst);

    [[maybe_unused]] size_t boff =
      block->offset.load(std::memory_order_relaxed);
    assert(circular_less_than(actOff, 1 + StartIdx));
    assert(circular_less_than(boff, 1 + StartIdx));

    // Ensure all blocks for the operation are allocated and available.
    data_block* startBlock = find_block(block, StartIdx);

    data_block* protectBlock;
    if (StartIdx != EndIdx) [[likely]] {
      data_block* endBlock = find_block(startBlock, EndIdx - 1);
      protectBlock = endBlock;
    } else {
      // User passed an empty range, or Count == 0
      protectBlock = startBlock;
    }
    // Update last known block.
    Haz->write_block.store(protectBlock, std::memory_order_release);
    Haz->next_protect_write.store(
      protectBlock->offset.load(std::memory_order_relaxed),
      std::memory_order_relaxed
    );
    return startBlock;
  }

  using handle_t = tmc::detail::BitmapObjectPool<hazard_ptr>::ScopedPoolObject;

public:
  // Gets a hazard pointer from the list, and takes ownership of it.
  handle_t get_hazard_ptr() noexcept {
    // reclaim_counter and haz_ptr_counter behave as a split lock shared with
    // try_reclaim_blocks(). If both operations run at the same time, we may see
    // an outdated value of head, and will need to reload head.
    size_t reclaimCount = reclaim_counter.load(std::memory_order_acquire);

    // Perform the private stage of the operation.
    auto obj = hazard_ptr_pool.template acquire_scoped_wfpg<1>();
    auto& ptr = obj.value;

    // Reload head_block until try_reclaim_blocks was not running.
    size_t reclaimCheck;
    do {
      reclaimCheck = reclaimCount;
      ptr.init(head_block.load(std::memory_order_acquire));
      // Signal to try_reclaim_blocks() that we read the value of head_block.
      haz_ptr_counter.fetch_add(1, std::memory_order_seq_cst);
      // Check if try_reclaim_blocks() was running (again)
      reclaimCount = reclaim_counter.load(std::memory_order_seq_cst);
    } while (reclaimCount != reclaimCheck);
    return obj;
  }

  template <typename U> void post(hazard_ptr* Haz, U&& Val) noexcept {
    // Get write ticket and associated block, protected by hazptr.
    size_t idx;
    element* elem = get_write_ticket(Haz, idx);

    elem->data.emplace(static_cast<U&&>(Val));
    elem->set_data_ready();

    // Then release the hazard pointer
    Haz->active_offset.store(
      idx + InactiveHazptrOffset, std::memory_order_release
    );
  }

  template <typename It>
  void post_bulk(hazard_ptr* Haz, It&& Items, size_t Count) noexcept {
    // Get write ticket and associated block, protected by hazptr.
    size_t startIdx, endIdx;
    data_block* block = get_write_ticket_bulk(Haz, Count, startIdx, endIdx);

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

    // Then release the hazard pointer
    Haz->active_offset.store(
      endIdx + InactiveHazptrOffset, std::memory_order_release
    );
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
    data_block* block = head_block.load(std::memory_order_relaxed);

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

      // Try to reclaim old blocks. Checking for index 1 ensures that at least
      // this consumer will already be advanced to the next block.
      if ((Idx & BlockSizeMask) == 1) {
        size_t protectIdx = write_offset.load(std::memory_order_acquire);
        if (circular_less_than(Idx, protectIdx)) {
          protectIdx = Idx;
        }
        try_reclaim_blocks(protectIdx);
      }
      return true;
    }
    return false;
  }

  ~qu_mpsc() {
    {
      size_t woff = write_offset.load(std::memory_order_relaxed);
      size_t idx = read_offset;
      data_block* block = head_block.load(std::memory_order_acquire);
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
      data_block* block = head_block.load(std::memory_order_acquire);
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
