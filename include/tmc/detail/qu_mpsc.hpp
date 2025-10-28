// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

// Modified from tmc::qu_mpsc, making the consumer side non-atomic,
// and removing consumer suspension. Removed close() logic, as it's expected
// for all tasks to be consumed from the executor before shutdown.

#include "tmc/current.hpp"
#include "tmc/detail/compat.hpp"
#include "tmc/detail/tiny_lock.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <memory>
#include <mutex>
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
} // namespace detail

struct qu_mpsc_default_config {
  /// The number of elements that can be stored in each block in the qu_mpsc
  /// linked list.
  static inline constexpr size_t BlockSize = 4096;

  /// At level 0, queue elements will be padded up to the next increment of 64
  /// bytes. This reduces false sharing between neighboring elements.
  /// At level 1, no padding will be applied.
  /// At level 2, no padding will be applied, and the flags value will be
  /// combined with the consumer pointer.
  static inline constexpr size_t PackingLevel = 0;

  /// If true, the first storage block will be a member of the qu_mpsc object
  /// (instead of dynamically allocated). Subsequent storage blocks are always
  /// dynamically allocated. Incompatible with set_reuse_blocks(false).
  static inline constexpr bool EmbedFirstBlock = false;
};

/// Tokens share ownership of a qu_mpsc by reference counting.
/// Access to the qu_mpsc (from multiple tokens) is thread-safe,
/// but access to a single token from multiple threads is not.
/// To access the qu_mpsc from multiple threads or tasks concurrently,
/// make a copy of the token for each (by using the copy constructor).
template <typename T, typename Config = tmc::qu_mpsc_default_config>
class qu_mpsc_tok;

/// Creates a new qu_mpsc and returns an access token to it.
template <typename T, typename Config = tmc::qu_mpsc_default_config>
inline qu_mpsc_tok<T, Config> make_qu_mpsc() noexcept;

template <typename T, typename Config = tmc::qu_mpsc_default_config>
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

  // Defaults to 2M items per second; this is 1 item every 500ns.
  static inline constexpr size_t DefaultHeavyLoadThreshold = 2000000;

  friend qu_mpsc_tok<T, Config>;
  template <typename Tc, typename Cc>
  friend qu_mpsc_tok<Tc, Cc> make_qu_mpsc() noexcept;

private:
  // The API of this class is a bit unusual, in order to match packed_element_t
  // (which can efficiently access both flags and consumer at the same time).
  class element_t {
    static inline constexpr size_t DATA_BIT = TMC_ONE_BIT;
    std::atomic<size_t> flags;

  public:
    tmc::detail::qu_mpsc_storage<T> data;

    static constexpr size_t UNPADLEN =
      sizeof(size_t) + sizeof(void*) + sizeof(tmc::detail::qu_mpsc_storage<T>);
    static constexpr size_t WANTLEN = (UNPADLEN + 63) & -64; // round up to 64
    static constexpr size_t PADLEN =
      UNPADLEN < WANTLEN ? (WANTLEN - UNPADLEN) : 0;

    struct empty {};
    using Padding =
      std::conditional_t<Config::PackingLevel == 0, char[PADLEN], empty>;
    TMC_NO_UNIQUE_ADDRESS Padding pad;

    // Sets the data ready flag,
    // or returns a consumer pointer if that consumer was already waiting.
    void set_data_ready() noexcept {
      uintptr_t expected = 0;
      [[maybe_unused]] bool ok = flags.compare_exchange_strong(
        expected, DATA_BIT, std::memory_order_acq_rel, std::memory_order_acquire
      );
      assert(ok);
    }

    bool is_data_waiting() noexcept {
      return DATA_BIT == flags.load(std::memory_order_acquire);
    }

    void reset() noexcept { flags.store(0, std::memory_order_relaxed); }
  };

  // Same API as element_t
  struct packed_element_t {
    static inline constexpr uintptr_t DATA_BIT = TMC_ONE_BIT;
    std::atomic<void*> flags;

  public:
    tmc::detail::qu_mpsc_storage<T> data;

    // Sets the data ready flag,
    // or returns a consumer pointer if that consumer was already waiting.
    void set_data_ready() noexcept {
      void* expected = nullptr;
      [[maybe_unused]] bool ok = flags.compare_exchange_strong(
        expected, reinterpret_cast<void*>(DATA_BIT), std::memory_order_acq_rel,
        std::memory_order_acquire
      );
      assert(ok);
    }

    bool is_data_waiting() noexcept {
      void* f = flags.load(std::memory_order_acquire);
      return DATA_BIT == reinterpret_cast<uintptr_t>(f);
    }

    void reset() noexcept {
      // Clear the consumer pointer
      flags.store(nullptr, std::memory_order_relaxed);
    }
  };

  using element =
    std::conditional_t < Config::PackingLevel<2, element_t, packed_element_t>;
  static_assert(
    Config::PackingLevel < 2 || TMC_PLATFORM_BITS == 64,
    "Packing level 2 requires 64-bit mode due to the use of pointer tagging."
  );

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
    std::atomic<bool> owned;
    std::atomic<hazard_ptr*> next;
    std::atomic<size_t> active_offset;
    std::atomic<data_block*> write_block;
    std::atomic<data_block*> read_block;
    std::atomic<int> thread_index;
    std::atomic<int> requested_thread_index;
    std::atomic<size_t> next_protect_write;
    std::atomic<size_t> next_protect_read;
    size_t lastTimestamp;

    friend class qu_mpsc;

    void release_blocks() noexcept {
      // These elements may be read (by try_reclaim_block()) after
      // take_ownership() has been called, but before init() has been called.
      // These defaults ensure sane behavior.
      write_block.store(nullptr, std::memory_order_relaxed);
      read_block.store(nullptr, std::memory_order_relaxed);
    }

    hazard_ptr() noexcept {
      thread_index.store(
        static_cast<int>(tmc::current_thread_index()), std::memory_order_relaxed
      );
      active_offset.store(InactiveHazptrOffset, std::memory_order_relaxed);
      release_blocks();
    }

    void init(data_block* head) noexcept {
      thread_index.store(
        static_cast<int>(tmc::current_thread_index()), std::memory_order_relaxed
      );
      requested_thread_index.store(-1, std::memory_order_relaxed);
      size_t headOff = head->offset.load(std::memory_order_relaxed);
      next_protect_write.store(headOff);
      next_protect_read.store(headOff);
      active_offset.store(
        headOff + InactiveHazptrOffset, std::memory_order_relaxed
      );
      read_block.store(head, std::memory_order_relaxed);
      write_block.store(head, std::memory_order_relaxed);

      lastTimestamp = TMC_CPU_TIMESTAMP();
    }

    bool try_take_ownership() noexcept {
      bool expected = false;
      return owned.compare_exchange_strong(expected, true);
    }

    template <typename Pred, typename Func>
    void for_each_owned_hazptr(Pred pred, Func func) noexcept {
      hazard_ptr* curr = this;
      while (pred()) {
        hazard_ptr* n = curr->next.load(std::memory_order_acquire);
        bool is_owned = curr->owned.load(std::memory_order_relaxed);
        if (is_owned) {
          func(curr);
        }
        if (n == this) {
          break;
        }
        curr = n;
      }
    }

  public:
    /// Returns the hazard pointer back to the hazard pointer freelist, so that
    /// it can be reused by another thread or task.
    void release_ownership() noexcept {
      release_blocks();
      owned.store(false);
    }
  };

  static_assert(std::atomic<size_t>::is_always_lock_free);
  static_assert(std::atomic<data_block*>::is_always_lock_free);

  static inline constexpr size_t WRITE_CLOSING_BIT = TMC_ONE_BIT;
  static inline constexpr size_t WRITE_CLOSED_BIT = TMC_ONE_BIT << 1;
  static inline constexpr size_t READ_CLOSED_BIT = TMC_ONE_BIT << 2;
  static inline constexpr size_t ALL_CLOSED_BITS = (TMC_ONE_BIT << 3) - 1;

  // Written by get_hazard_ptr()
  std::atomic<size_t> haz_ptr_counter;
  std::atomic<hazard_ptr*> hazard_ptr_list;

  // Written by set_*() configuration functions
  std::atomic<size_t> ReuseBlocks;
  std::atomic<size_t> ConsumerSpins;
  char pad0[64];
  std::atomic<size_t> write_offset;
  char pad1[64];
  size_t read_offset;
  char pad2[64];

  // Blocks try_reclaim_blocks(), close(), and drain().
  // Rarely blocks get_hazard_ptr() - if racing with try_reclaim_blocks().
  alignas(64) std::mutex blocks_lock;
  std::atomic<size_t> reclaim_counter;
  std::atomic<data_block*> head_block;
  std::atomic<data_block*> tail_block;

  struct empty {};
  using EmbeddedBlock =
    std::conditional_t<Config::EmbedFirstBlock, data_block, empty>;
  TMC_NO_UNIQUE_ADDRESS EmbeddedBlock embedded_block;

  qu_mpsc() noexcept {
    data_block* block;
    if constexpr (Config::EmbedFirstBlock) {
      block = &embedded_block;
    } else {
      block = new data_block(0);
    }
    head_block.store(block, std::memory_order_relaxed);
    tail_block.store(block, std::memory_order_relaxed);
    read_offset = 0;
    write_offset.store(0, std::memory_order_relaxed);

    ReuseBlocks.store(true, std::memory_order_relaxed);
    ConsumerSpins.store(0, std::memory_order_relaxed);

    haz_ptr_counter.store(0, std::memory_order_relaxed);
    reclaim_counter.store(0, std::memory_order_relaxed);
    hazard_ptr* haz = new hazard_ptr;
    haz->next.store(haz, std::memory_order_relaxed);
    haz->owned.store(false, std::memory_order_relaxed);
    hazard_ptr_list.store(haz, std::memory_order_relaxed);
    tmc::detail::memory_barrier();
  }

  hazard_ptr* get_hazard_ptr_impl() noexcept {
    hazard_ptr* start = hazard_ptr_list.load(std::memory_order_relaxed);
    hazard_ptr* ptr = start;
    while (true) {
      hazard_ptr* next = ptr->next.load(std::memory_order_acquire);
      bool is_owned = ptr->owned.load(std::memory_order_relaxed);
      if ((is_owned == false) && ptr->try_take_ownership()) {
        break;
      }
      if (next == start) {
        hazard_ptr* newptr = new hazard_ptr;
        newptr->owned.store(true, std::memory_order_relaxed);
        do {
          newptr->next.store(next, std::memory_order_release);
        } while (!ptr->next.compare_exchange_strong(
          next, newptr, std::memory_order_acq_rel, std::memory_order_acquire
        ));
        ptr = newptr;
        break;
      }
      ptr = next;
    }
    return ptr;
  }

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
  data_block* try_advance_head(
    hazard_ptr* Haz, data_block* OldHead, size_t ProtectIdx
  ) noexcept {
    // In the current implementation, this is called only from consumers.
    // Therefore, this token's hazptr will be active, and protecting read_block.
    // However, if producers are lagging behind, and no producer is currently
    // active, write_block would not be protected. Therefore, write_offset
    // should be passed to ProtectIdx to cover this scenario.
    ProtectIdx = ProtectIdx & ~BlockSizeMask; // round down to block index

    // Find the lowest offset that is protected by ProtectIdx or any hazptr.
    size_t oldOff = OldHead->offset.load(std::memory_order_relaxed);
    Haz->for_each_owned_hazptr(
      [&]() { return circular_less_than(oldOff, ProtectIdx); },
      [&](hazard_ptr* curr) { keep_min(ProtectIdx, curr->active_offset); }
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
    Haz->for_each_owned_hazptr(
      [&]() { return circular_less_than(oldOff, ProtectIdx); },
      [&](hazard_ptr* curr) {
        try_advance_hazptr_block(
          curr->write_block, ProtectIdx, newHead, curr->active_offset
        );
        // TODO - there is only 1 reader (this thread), and its value could be
        // non-atomic. Split this out of the loop
        // Also, can we skip updating the release_idx for reader?
        try_advance_hazptr_block(
          curr->read_block, ProtectIdx, newHead, curr->active_offset
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
    if (!ReuseBlocks.load(std::memory_order_relaxed)) {
      while (OldHead != NewHead) {
        data_block* next = OldHead->next.load(std::memory_order_relaxed);
        delete OldHead;
        OldHead = next;
      }
    } else {
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

        data_block* tailBlock = tail_block.load(std::memory_order_acquire);
        data_block* next = tailBlock->next.load(std::memory_order_acquire);
        do {
          while (next != nullptr) {
            tailBlock = next;
            next = tailBlock->next.load(std::memory_order_acquire);
          }
          size_t i = 0;
          size_t boff =
            tailBlock->offset.load(std::memory_order_relaxed) + BlockSize;
          for (; i < unlinkedCount - 1; ++i) {
            data_block* b = unlinked[i];
            b->offset.store(boff, std::memory_order_relaxed);
            b->next.store(unlinked[i + 1], std::memory_order_release);
            boff += BlockSize;
          }

          unlinked[i]->offset.store(boff, std::memory_order_relaxed);
          unlinked[i]->next.store(nullptr, std::memory_order_release);

        } while (!tailBlock->next.compare_exchange_strong(
          next, unlinked[0], std::memory_order_acq_rel,
          std::memory_order_acquire
        ));

        tail_block.store(unlinked[unlinkedCount - 1]);
      }
    }
  }

  // Access to this function must be externally synchronized (via blocks_lock).
  // Blocks that are not protected by a hazard pointer will be reclaimed, and
  // head_block will be advanced to the first protected block.
  void try_reclaim_blocks(hazard_ptr* Haz, size_t ProtectIdx) noexcept {
    data_block* oldHead = head_block.load(std::memory_order_acquire);
    // reclaim_counter and haz_ptr_counter behave as a split lock shared with
    // get_hazard_ptr(). If both operations run at the same time, this will
    // abandon its operation before the final stage.
    size_t hazptrCount = haz_ptr_counter.load(std::memory_order_acquire);

    // Perform the private stage of the operation.
    data_block* newHead = try_advance_head(Haz, oldHead, ProtectIdx);
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

  template <typename U> void write_element(element* Elem, U&& Val) noexcept {}

public:
  // Gets a hazard pointer from the list, and takes ownership of it.
  hazard_ptr* get_hazard_ptr() noexcept {
    // reclaim_counter and haz_ptr_counter behave as a split lock shared with
    // try_reclaim_blocks(). If both operations run at the same time, we may see
    // an outdated value of head, and will need to reload head.
    size_t reclaimCount = reclaim_counter.load(std::memory_order_acquire);

    // Perform the private stage of the operation.
    hazard_ptr* ptr = get_hazard_ptr_impl();

    // Reload head_block until try_reclaim_blocks was not running.
    size_t reclaimCheck;
    do {
      reclaimCheck = reclaimCount;
      ptr->init(head_block.load(std::memory_order_acquire));
      // Signal to try_reclaim_blocks() that we read the value of head_block.
      haz_ptr_counter.fetch_add(1, std::memory_order_seq_cst);
      // Check if try_reclaim_blocks() was running (again)
      reclaimCount = reclaim_counter.load(std::memory_order_seq_cst);
    } while (reclaimCount != reclaimCheck);
    return ptr;
  }

  template <typename U> bool post(hazard_ptr* Haz, U&& Val) noexcept {
    // Get write ticket and associated block, protected by hazptr.
    size_t idx;
    element* elem = get_write_ticket(Haz, idx);
    if (elem == nullptr) [[unlikely]] {
      return false;
    }

    elem->data.emplace(static_cast<U&&>(Val));
    elem->set_data_ready();

    // Then release the hazard pointer
    Haz->active_offset.store(
      idx + InactiveHazptrOffset, std::memory_order_release
    );

    return true;
  }

  template <typename It>
  bool post_bulk(hazard_ptr* Haz, It&& Items, size_t Count) noexcept {
    // Get write ticket and associated block, protected by hazptr.
    size_t startIdx, endIdx;
    data_block* block = get_write_ticket_bulk(Haz, Count, startIdx, endIdx);
    if (block == nullptr) [[unlikely]] {
      return false;
    }

    size_t idx = startIdx;
    while (idx < endIdx) {
      element* elem = &block->values[idx & BlockSizeMask];

      elem->data.emplace(std::move(*Items));
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

    return true;
  }

  bool empty(hazard_ptr* Haz) {
    size_t actOff = Haz->next_protect_read.load(std::memory_order_relaxed);
    Haz->active_offset.store(actOff, std::memory_order_relaxed);

    // need a StoreLoad barrier between setting hazptr and loading the block
    tmc::detail::memory_barrier();

    size_t Idx = read_offset;
    size_t release_idx = Idx + InactiveHazptrOffset;
    data_block* block = Haz->read_block.load(std::memory_order_seq_cst);

    size_t boff = block->offset.load(std::memory_order_relaxed);
    assert(circular_less_than(actOff, 1 + Idx));
    assert(circular_less_than(boff, 1 + Idx));

    block = find_block(block, Idx);
    element* elem = &block->values[Idx & BlockSizeMask];

    bool isEmpty = !elem->is_data_waiting();
    Haz->active_offset.store(release_idx, std::memory_order_release);
    return isEmpty;
  }

  bool try_pull(hazard_ptr* Haz, T& output) {
    size_t actOff = Haz->next_protect_read.load(std::memory_order_relaxed);
    Haz->active_offset.store(actOff, std::memory_order_relaxed);

    // need a StoreLoad barrier between setting hazptr and loading the block
    tmc::detail::memory_barrier();

    size_t Idx = read_offset;
    size_t release_idx = Idx + InactiveHazptrOffset;
    data_block* block = Haz->read_block.load(std::memory_order_seq_cst);

    size_t boff = block->offset.load(std::memory_order_relaxed);
    assert(circular_less_than(actOff, 1 + Idx));
    assert(circular_less_than(boff, 1 + Idx));

    block = find_block(block, Idx);
    // Update last known block.
    // Note that if hazptr was to an older block, that block will still be
    // protected (by active_offset). This prevents a qu_mpsc consisting of a
    // single block from trying to unlink/link that block to itself.
    Haz->read_block.store(block, std::memory_order_release);
    Haz->next_protect_read.store(boff, std::memory_order_relaxed);
    // Try to reclaim old blocks. Checking for index 1 ensures that at least
    // this token's hazptr will already be advanced to the new block.
    // Only consumers participate in reclamation and only 1 consumer at a time.
    if ((Idx & BlockSizeMask) == 1 && blocks_lock.try_lock()) {
      size_t protectIdx = write_offset.load(std::memory_order_acquire);
      try_reclaim_blocks(Haz, protectIdx);
      blocks_lock.unlock();
    }
    element* elem = &block->values[Idx & BlockSizeMask];

    if (elem->is_data_waiting()) {
      // Data is already ready here.
      output = std::move(elem->data.value);
      elem->data.destroy();
      read_offset = Idx + 1;
      Haz->active_offset.store(release_idx, std::memory_order_release);
      return true;
    }
    Haz->active_offset.store(release_idx, std::memory_order_release);
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
    {
      hazard_ptr* start = hazard_ptr_list.load(std::memory_order_relaxed);
      hazard_ptr* curr = start;
      while (true) {
        assert(!curr->owned);
        hazard_ptr* next = curr->next.load(std::memory_order_relaxed);
        delete curr;
        if (next == start) {
          break;
        }
        curr = next;
      }
    }
  }

  qu_mpsc(const qu_mpsc&) = delete;
  qu_mpsc& operator=(const qu_mpsc&) = delete;
  qu_mpsc(qu_mpsc&&) = delete;
  qu_mpsc& operator=(qu_mpsc&&) = delete;
};

template <typename T, typename Config> class qu_mpsc_tok {
  using qu_mpsc_t = qu_mpsc<T, Config>;
  using hazard_ptr = qu_mpsc_t::hazard_ptr;
  std::shared_ptr<qu_mpsc_t> chan;
  hazard_ptr* haz_ptr;
  NO_CONCURRENT_ACCESS_LOCK;

  friend qu_mpsc_tok make_qu_mpsc<T, Config>() noexcept;

  qu_mpsc_tok(std::shared_ptr<qu_mpsc_t>&& Chan) noexcept
      : chan{std::move(Chan)}, haz_ptr{nullptr} {}

  hazard_ptr* get_hazard_ptr() noexcept {
    if (haz_ptr == nullptr) [[unlikely]] {
      haz_ptr = chan->get_hazard_ptr();
    }
    return haz_ptr;
  }

  void free_hazard_ptr() noexcept {
    if (haz_ptr != nullptr) [[likely]] {
      haz_ptr->release_ownership();
      haz_ptr = nullptr;
    }
  }

public:
  /// If the qu_mpsc is open, this will always return true, indicating that Val
  /// was enqueued.
  ///
  /// If the qu_mpsc is closed, this will return false, and Val will not be
  /// enqueued.
  ///
  /// Will not suspend or block.
  template <typename U> bool post(U&& Val) noexcept {
    ASSERT_NO_CONCURRENT_ACCESS();
    hazard_ptr* haz = get_hazard_ptr();
    return chan->post(haz, std::forward<U>(Val));
  }

  bool try_pull(T& item) noexcept {
    ASSERT_NO_CONCURRENT_ACCESS();
    hazard_ptr* haz = get_hazard_ptr();
    return chan->try_pull(haz, item);
  }

  bool empty() noexcept {
    ASSERT_NO_CONCURRENT_ACCESS();
    hazard_ptr* haz = get_hazard_ptr();
    return chan->empty(haz);
  }

  /// If the qu_mpsc is open, this will always return true, indicating that
  /// Count elements, starting from the Begin iterator, were enqueued.
  ///
  /// If the qu_mpsc is closed, this will return false, and no items
  /// will be enqueued.
  ///
  /// Each item is moved (not copied) from the iterator into the qu_mpsc.
  ///
  /// The closed check is performed first, then space is pre-allocated, then all
  /// Count items are moved into the qu_mpsc. Thus, there cannot be a partial
  /// success - either all or none of the items will be moved.
  ///
  /// Will not suspend or block.
  template <typename TIter> bool post_bulk(TIter&& Begin, size_t Count) {
    hazard_ptr* haz = get_hazard_ptr();
    return chan->post_bulk(haz, static_cast<TIter&&>(Begin), Count);
  }

  /// Calculates the number of elements via `size_t Count = End - Begin;`
  ///
  /// If the qu_mpsc is open, this will always return true, indicating that
  /// Count elements, starting from the Begin iterator, were enqueued.
  ///
  /// If the qu_mpsc is closed, this will return false, and no items
  /// will be enqueued.
  ///
  /// Each item is moved (not copied) from the iterator into the qu_mpsc.
  ///
  /// The closed check is performed first, then space is pre-allocated, then all
  /// Count items are moved into the qu_mpsc. Thus, there cannot be a partial
  /// success - either all or none of the items will be moved.
  ///
  /// Will not suspend or block.
  template <typename TIter> bool post_bulk(TIter&& Begin, TIter&& End) {
    hazard_ptr* haz = get_hazard_ptr();
    return chan->post_bulk(
      haz, static_cast<TIter&&>(Begin), static_cast<size_t>(End - Begin)
    );
  }

  /// Calculates the number of elements via
  /// `size_t Count = Range.end() - Range.begin();`
  ///
  /// If the qu_mpsc is open, this will always return true, indicating that
  /// Count elements from the beginning of the range were enqueued.
  ///
  /// If the qu_mpsc is closed, this will return false, and no items
  /// will be enqueued.
  ///
  /// Each item is moved (not copied) from the iterator into the qu_mpsc.
  ///
  /// The closed check is performed first, then space is pre-allocated, then all
  /// Count items are moved into the qu_mpsc. Thus, there cannot be a partial
  /// success - either all or none of the items will be moved.
  ///
  /// Will not suspend or block.
  template <typename TRange> bool post_bulk(TRange&& Range) {
    hazard_ptr* haz = get_hazard_ptr();
    auto begin = static_cast<TRange&&>(Range).begin();
    auto end = static_cast<TRange&&>(Range).end();
    return chan->post_bulk(haz, begin, static_cast<size_t>(end - begin));
  }

  /// If true, spent blocks will be cleared and moved to the tail of the queue.
  /// If false, spent blocks will be deleted.
  /// Default: true
  ///
  /// If Config::EmbedFirstBlock == true, this will be forced to true.
  qu_mpsc_tok& set_reuse_blocks(bool Reuse) noexcept {
    if constexpr (!Config::EmbedFirstBlock) {
      chan->ReuseBlocks.store(Reuse, std::memory_order_relaxed);
      return *this;
    }
  }

  /// If a consumer sees no data is ready at a ticket, it will spin wait this
  /// many times. Each spin wait is an asm("pause") and reload.
  /// Default: 0
  qu_mpsc_tok& set_consumer_spins(size_t SpinCount) noexcept {
    chan->ConsumerSpins.store(SpinCount, std::memory_order_relaxed);
    return *this;
  }

  /// Copy Constructor: The new qu_mpsc_tok will have its own hazard pointer so
  /// that it can be used concurrently with the other token.
  ///
  /// If the other token is from a different qu_mpsc, this token will now point
  /// to that qu_mpsc.
  qu_mpsc_tok(const qu_mpsc_tok& Other) noexcept
      : chan(Other.chan), haz_ptr{nullptr} {}

  /// Copy Assignment: If the other token is from a different qu_mpsc, this
  /// token will now point to that qu_mpsc.
  qu_mpsc_tok& operator=(const qu_mpsc_tok& Other) noexcept {
    if (chan != Other.chan) {
      free_hazard_ptr();
      chan = Other.chan;
    }
  }

  /// Identical to the token copy constructor, but makes
  /// the intent more explicit - that a new token is being created which will
  /// independently own a reference count and hazard pointer to the underlying
  /// qu_mpsc.
  qu_mpsc_tok new_token() noexcept { return qu_mpsc_tok(*this); }

  /// Move Constructor: The moved-from token will become empty; it will release
  /// its qu_mpsc pointer, and its hazard pointer.
  qu_mpsc_tok(qu_mpsc_tok&& Other) noexcept
      : chan(std::move(Other.chan)), haz_ptr{Other.haz_ptr} {
    Other.haz_ptr = nullptr;
  }

  /// Move Assignment: The moved-from token will become empty; it will release
  /// its qu_mpsc pointer, and its hazard pointer.
  ///
  /// If the other token is from a different qu_mpsc, this token will now point
  /// to that qu_mpsc.
  qu_mpsc_tok& operator=(qu_mpsc_tok&& Other) noexcept {
    if (chan != Other.chan) {
      free_hazard_ptr();
      haz_ptr = Other.haz_ptr;
      Other.haz_ptr = nullptr;
    } else {
      if (haz_ptr != nullptr) {
        // It's more efficient to keep our own hazptr
        Other.free_hazard_ptr();
      } else {
        haz_ptr = Other.haz_ptr;
        Other.haz_ptr = nullptr;
      }
    }
    chan = std::move(Other.chan);
    return *this;
  }

  /// Releases the token's hazard pointer and decrements the qu_mpsc's shared
  /// reference count. When the last token for a qu_mpsc is destroyed, the
  /// qu_mpsc will also be destroyed. If the qu_mpsc was not drained and any
  /// data remains in the qu_mpsc, the destructor will also be called for each
  /// remaining data element.
  ~qu_mpsc_tok() { free_hazard_ptr(); }
};

template <typename T, typename Config>
inline qu_mpsc_tok<T, Config> make_qu_mpsc() noexcept {
  auto chan = new qu_mpsc<T, Config>();
  return qu_mpsc_tok<T, Config>{std::shared_ptr<qu_mpsc<T, Config>>(chan)};
}

} // namespace tmc
