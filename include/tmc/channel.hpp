// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

// Provides tmc::channel, an async MPMC queue of unbounded size.
// Producers enqueue values with co_await push() or post().
// Consumers retrieve values in FIFO order with co_await pull().
// If no values are available, the consumer will suspend until a value is ready.

// The hazard pointer scheme is loosely based on
// 'A wait-free queue as fast as fetch-and-add' by Yang & Mellor-Crummey
// https://dl.acm.org/doi/10.1145/2851141.2851168

#include "tmc/detail/compat.hpp"
#include "tmc/detail/concepts.hpp"
#include "tmc/detail/thread_locals.hpp"
#include "tmc/detail/tiny_lock.hpp"

#include <array>
#include <atomic>
#include <coroutine>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>
#include <variant>
#include <vector>

namespace tmc {
namespace detail {
// Allocates elements without constructing them, to be constructed later using
// placement new. T need not be default, copy, or move constructible.
// The caller must track whether the element exists, and manually invoke the
// destructor if necessary.
template <typename T> struct channel_storage {
  union alignas(alignof(T)) {
    T value;
  };
#ifndef NDEBUG
  bool exists = false;
#endif

  channel_storage() {}

  template <typename... ConstructArgs> void emplace(ConstructArgs&&... Args) {
#ifndef NDEBUG
    assert(!exists);
    exists = true;
#endif
    ::new (static_cast<void*>(&value)) T(static_cast<ConstructArgs&&>(Args)...);
  }

  void destroy() {
#ifndef NDEBUG
    assert(exists);
    exists = false;
#endif
    value.~T();
  }

  // Precondition: Other.value must exist
  channel_storage(channel_storage&& Other) {
    emplace(static_cast<T&&>(Other.value));
    Other.destroy();
  }
  channel_storage& operator=(channel_storage&& Other) {
    emplace(static_cast<T&&>(Other.value));
    Other.destroy();
    return *this;
  }

  // If data was present, the caller is responsible for destroying it.
#ifndef NDEBUG
  ~channel_storage() { assert(!exists); }
#else
  ~channel_storage() = default;
#endif

  channel_storage(const channel_storage&) = delete;
  channel_storage& operator=(const channel_storage&) = delete;
};
} // namespace detail

enum class chan_err { OK = 0, CLOSED = 1, EMPTY = 2 };

struct chan_default_config {
  static inline constexpr size_t BlockSize = 4096;
  /// At level 0, queue elements will be padded to 64 bytes.
  /// At level 1, queue elements will not be padded, and the flags will be
  /// packed into the upper bits of the consumer pointer.
  static inline constexpr size_t PackingLevel = 0;
};

/// Tokens share ownership of a channel by reference counting.
/// Access to the channel (from multiple tokens) is thread-safe,
/// but access to a single token from multiple threads is not.
/// To access the channel from multiple threads or tasks concurrently,
/// make a copy of the token for each (by using the copy constructor).
template <typename T, typename Config = tmc::chan_default_config>
class chan_tok;

/// Creates a new channel and returns an access token to it.
/// Tokens share ownership of a channel by reference counting.
/// Access to the channel (from multiple tokens) is thread-safe,
/// but access to a single token from multiple threads is not.
/// To access the channel from multiple threads or tasks concurrently,
/// make a copy of the token for each.
template <typename T, typename Config = tmc::chan_default_config>
static inline chan_tok<T, Config> make_channel();

template <typename T, typename Config = tmc::chan_default_config>
class channel {
  static inline constexpr size_t BlockSize = Config::BlockSize;
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

  // An offset far enough forward that it won't protect anything for a very long
  // time, but close enough that it isn't considered "circular less than" 0.
  // On 32 bit this is only 1Gi elements. The worst case is that a
  // thread suspends for a very long time, and the queue processes 1Gi
  // elements and then cannot free any blocks until that thread wakes. This is
  // extremely unlikely, and not an error - it will just prevent block
  // reclamation. On 64 bit in practice this will never happen.
  static inline constexpr size_t InactiveHazptrOffset =
    TMC_ONE_BIT << (TMC_PLATFORM_BITS - 2);

  static constexpr size_t BlockSizeMask = BlockSize - 1;

  friend chan_tok<T, Config>;
  template <typename Tc, typename Cc> friend chan_tok<Tc, Cc> make_channel();

public:
  class aw_pull;
  class aw_push;

private:
  struct element {
    std::atomic<size_t> flags;
    aw_pull::aw_pull_impl* consumer;
    tmc::detail::channel_storage<T> data;
    static constexpr size_t UNPADLEN =
      sizeof(size_t) + sizeof(void*) + sizeof(T);
    static constexpr size_t PADLEN = UNPADLEN < 64 ? (64 - UNPADLEN) : 0;
    char pad[PADLEN];
  };

  struct data_block {
    std::atomic<size_t> offset;
    std::atomic<data_block*> next;
    std::array<element, BlockSize> values;

    void reset_values() {
      for (size_t i = 0; i < BlockSize; ++i) {
        values[i].flags.store(0, std::memory_order_relaxed);
      }
    }

    data_block(size_t Offset) {
      offset.store(Offset, std::memory_order_relaxed);
      reset_values();
      next.store(nullptr, std::memory_order_release);
    }
  };

  // Pointer tagging the next ptr allows for efficient search
  // of owned or unowned hazptrs in the list.
  static inline constexpr size_t IS_OWNED_BIT = TMC_ONE_BIT << 60;
  struct alignas(64) hazard_ptr {
    std::atomic<uintptr_t> next;
    std::atomic<size_t> active_offset;
    std::atomic<data_block*> write_block;
    std::atomic<data_block*> read_block;
    std::atomic<size_t> read_count;
    std::atomic<size_t> write_count;
    std::atomic<int> thread_index;
    std::atomic<int> requested_thread_index;
    size_t lastTimestamp;
    size_t minCycles;

    void release() {
      // These elements may be read (by try_reclaim_block()) after
      // take_ownership() has been called, but before init() has been called.
      // These defaults ensure sane behavior.
      write_block.store(nullptr, std::memory_order_relaxed);
      read_block.store(nullptr, std::memory_order_relaxed);
      active_offset.store(InactiveHazptrOffset, std::memory_order_relaxed);
    }

    hazard_ptr() {
      thread_index.store(
        tmc::detail::this_thread::thread_index, std::memory_order_relaxed
      );
      release();
    }

    void init(data_block* head, size_t MinCycles) {
      thread_index.store(
        static_cast<int>(tmc::detail::this_thread::thread_index),
        std::memory_order_relaxed
      );
      requested_thread_index.store(-1, std::memory_order_relaxed);
      read_count.store(0, std::memory_order_relaxed);
      write_count.store(0, std::memory_order_relaxed);
      active_offset.store(
        head->offset.load(std::memory_order_relaxed) + InactiveHazptrOffset,
        std::memory_order_relaxed
      );
      read_block.store(head, std::memory_order_relaxed);
      write_block.store(head, std::memory_order_relaxed);

      lastTimestamp = TMC_CPU_TIMESTAMP();
      minCycles = MinCycles;
    }

    bool should_suspend() { return write_count + read_count >= 10000; }

    size_t elapsed() {
      size_t currTimestamp = TMC_CPU_TIMESTAMP();
      size_t elapsed = currTimestamp - lastTimestamp;
      lastTimestamp = currTimestamp;
      return elapsed;
    }

    bool try_take_ownership() {
      if ((next.fetch_or(IS_OWNED_BIT) & IS_OWNED_BIT) == 0) {
        return true;
      }
      return false;
    }

    void release_ownership() {
      release();
      [[maybe_unused]] bool ok =
        (next.fetch_and(~IS_OWNED_BIT) & IS_OWNED_BIT) != 0;
      assert(ok);
    }

    void inc_read_count() {
      auto count = read_count.load(std::memory_order_relaxed);
      read_count.store(count + 1, std::memory_order_relaxed);
      thread_index.store(
        static_cast<int>(tmc::detail::this_thread::thread_index),
        std::memory_order_relaxed
      );
    }

    void inc_write_count() {
      auto count = write_count.load(std::memory_order_relaxed);
      write_count.store(count + 1, std::memory_order_relaxed);
      thread_index.store(
        static_cast<int>(tmc::detail::this_thread::thread_index),
        std::memory_order_relaxed
      );
    }

    template <typename Pred, typename Func>
    void for_each_owned_hazptr(Pred pred, Func func) {
      hazard_ptr* curr = this;
      while (pred()) {
        uintptr_t next_raw = curr->next.load(std::memory_order_acquire);
        hazard_ptr* next =
          reinterpret_cast<hazard_ptr*>(next_raw & ~IS_OWNED_BIT);
        uintptr_t is_owned = next_raw & IS_OWNED_BIT;
        if (is_owned) {
          func(curr);
        }
        if (next == this) {
          break;
        }
        curr = next;
      }
    }
  };

  static_assert(std::atomic<size_t>::is_always_lock_free);
  static_assert(std::atomic<data_block*>::is_always_lock_free);

  // Signals between try_reclaim_blocks(), close(), drain(), and reopen().
  std::atomic<size_t> closed; // bit 0 = write closed. bit 1 = read closed
  std::atomic<size_t> write_closed_at;
  std::atomic<size_t> read_closed_at;
  std::atomic<data_block*> head_block;
  std::atomic<data_block*> tail_block;
  tmc::tiny_lock blocks_lock;
  char pad0[64 - 1 * (sizeof(void*))];
  std::atomic<size_t> write_offset;
  char pad1[64 - 1 * (sizeof(void*))];
  std::atomic<size_t> read_offset;
  char pad2[64 - 2 * (sizeof(void*))];
  // Signals between get_hazard_ptr() and try_reclaim_blocks().
  // Configs in between, as these signals are infrequently used.
  std::atomic<hazard_ptr*> hazard_ptr_list;
  std::atomic<size_t> haz_ptr_counter;
  std::atomic<size_t> ReuseBlocks;
  std::atomic<size_t> MinClusterCycles;
  std::atomic<size_t> ConsumerSpins;
  tmc::tiny_lock cluster_lock;
  std::atomic<size_t> reclaim_counter;

  channel() {
    closed.store(0, std::memory_order_relaxed);
    write_closed_at.store(0, std::memory_order_relaxed);
    read_closed_at.store(0, std::memory_order_relaxed);

    auto block = new data_block(0);
    head_block.store(block, std::memory_order_relaxed);
    tail_block.store(block, std::memory_order_relaxed);
    read_offset.store(0, std::memory_order_relaxed);
    write_offset.store(0, std::memory_order_relaxed);

    ReuseBlocks.store(true, std::memory_order_relaxed);
    MinClusterCycles.store(TMC_CPU_FREQ / 200, std::memory_order_relaxed);
    ConsumerSpins.store(0, std::memory_order_relaxed);

    haz_ptr_counter.store(0, std::memory_order_relaxed);
    reclaim_counter.store(0, std::memory_order_relaxed);
    hazard_ptr* hazptr = new hazard_ptr;
    hazptr->next.store(
      reinterpret_cast<uintptr_t>(hazptr), std::memory_order_relaxed
    );
    hazard_ptr_list.store(hazptr, std::memory_order_seq_cst);
  }

  struct cluster_data {
    size_t destination;
    hazard_ptr* id;
  };

  // Uses an extremely simple algorithm to determine the best thread to assign
  // workers to.
  static inline void cluster(std::vector<cluster_data>& clusterOn) {
    if (clusterOn.size() == 0) {
      return;
    }
    // Using the average is a hack - it would be better to determine
    // which group already has the most active tasks in it.
    size_t avg = 0;
    for (size_t i = 0; i < clusterOn.size(); ++i) {
      avg += clusterOn[i].destination;
    }
    avg /= clusterOn.size(); // integer division, yuck

    // Find the tid that is the closest to the average.
    // This becomes the clustering point.
    size_t minDiff = TMC_ALL_ONES;
    size_t closest;
    for (size_t i = 0; i < clusterOn.size(); ++i) {
      size_t tid = clusterOn[i].destination;
      size_t diff;
      if (tid >= avg) {
        diff = tid - avg;
      } else {
        diff = avg - tid;
      }
      if (diff < minDiff) {
        diff = minDiff;
        closest = tid;
      }
    }

    for (size_t i = 0; i < clusterOn.size(); ++i) {
      clusterOn[i].id->requested_thread_index.store(
        closest, std::memory_order_relaxed
      );
    }
  }

  // Tries to move producers and closers near each other.
  // Returns true if this thread ran the clustering algorithm, or if another
  // thread already ran the clustering algorithm and the result is ready.
  // Returns false if another thread is currently running the clustering
  // algorithm.
  bool try_cluster(hazard_ptr* hazptr) {
    if (!cluster_lock.try_lock()) {
      return false;
    }
    size_t rti = hazptr->requested_thread_index.load(std::memory_order_relaxed);
    if (rti != -1) {
      // Another thread already calculated rti for us
      cluster_lock.unlock();
      return true;
    }
    std::vector<cluster_data> reader;
    std::vector<cluster_data> writer;
    std::vector<cluster_data> both;
    reader.reserve(64);
    writer.reserve(64);
    hazptr->for_each_owned_hazptr(
      []() { return true; },
      [&](hazard_ptr* curr) {
        auto reads = curr->read_count.load(std::memory_order_relaxed);
        auto writes = curr->write_count.load(std::memory_order_relaxed);
        auto tid = curr->thread_index.load(std::memory_order_relaxed);
        if (writes == 0) {
          if (reads != 0) {
            reader.emplace_back(tid, curr);
          }
        } else {
          if (reads == 0) {
            writer.emplace_back(tid, curr);
          } else {
            both.emplace_back(tid, curr);
          }
        }
      }
    );

    if (writer.size() + reader.size() + both.size() <= 4) {
      // Cluster small numbers of workers together
      for (size_t i = 0; i < reader.size(); ++i) {
        writer.push_back(reader[i]);
      }
      for (size_t i = 0; i < both.size(); ++i) {
        writer.push_back(both[i]);
      }
      cluster(writer);
    } else {
      // Separate clusters for each kind of worker
      cluster(writer);
      cluster(reader);
      cluster(both);
    }

    cluster_lock.unlock();
    return true;
  }

  hazard_ptr* get_hazard_ptr_impl() {
    hazard_ptr* start = hazard_ptr_list.load(std::memory_order_relaxed);
    hazard_ptr* ptr = start;
    while (true) {
      uintptr_t next_raw = ptr->next.load(std::memory_order_acquire);
      uintptr_t is_owned = next_raw & IS_OWNED_BIT;
      if ((is_owned == 0) && ptr->try_take_ownership()) {
        break;
      }
      hazard_ptr* next =
        reinterpret_cast<hazard_ptr*>(next_raw & ~IS_OWNED_BIT);
      if (next == start) {
        hazard_ptr* newptr = new hazard_ptr;
        do {
          newptr->next.store(next_raw, std::memory_order_release);
        } while (!ptr->next.compare_exchange_strong(
          next_raw, IS_OWNED_BIT | reinterpret_cast<uintptr_t>(newptr),
          std::memory_order_acq_rel, std::memory_order_acquire
        ));
        ptr = newptr;
        break;
      }
      ptr = next;
    }
    return ptr;
  }

  // Gets a hazard pointer from the list, and takes ownership of it.
  // Lock-free on the common fast path.
  hazard_ptr* get_hazard_ptr() {
    size_t reclaimCheck = reclaim_counter.load(std::memory_order_seq_cst);
    hazard_ptr* ptr = get_hazard_ptr_impl();
    haz_ptr_counter.fetch_add(1, std::memory_order_seq_cst);
    size_t cycles = MinClusterCycles.load(std::memory_order_relaxed);
    if (reclaimCheck == reclaim_counter.load(std::memory_order_relaxed)) {
      ptr->init(head_block.load(std::memory_order_relaxed), cycles);
    } else {
      // A reclaim operation is in progress. Wait for it to complete before
      // getting the head pointer.
      tmc::tiny_lock_guard lg(blocks_lock);
      ptr->init(head_block.load(std::memory_order_relaxed), cycles);
    }
    return ptr;
  }

  static inline bool circular_less_than(size_t a, size_t b) {
    return a - b > (TMC_ONE_BIT << (TMC_PLATFORM_BITS - 1));
  }

  // Load src and move it into dst if src < dst.
  static inline void keep_min(size_t& Dst, std::atomic<size_t> const& Src) {
    size_t val = Src.load(std::memory_order_acquire);
    if (circular_less_than(val, Dst)) {
      Dst = val;
    }
  }

  // Move src into dst if src < dst.
  static inline void keep_min(size_t& Dst, size_t Src) {
    if (circular_less_than(Src, Dst)) {
      Dst = Src;
    }
  }

  // Advances DstBlock to be equal to NewHead. Possibly reduces MinProtect if
  // DstBlock was already updated by its owning thread.
  static inline void try_advance_hazptr_block(
    std::atomic<data_block*>& DstBlock, size_t& MinProtected,
    data_block* NewHead, std::atomic<size_t> const& HazardOffset
  ) {
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
  try_advance_head(hazard_ptr* Haz, data_block* OldHead, size_t ProtectIdx) {
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
      newHead = newHead->next;
    }

    // Then update all hazptrs to be at this block or later.
    Haz->for_each_owned_hazptr(
      [&]() { return circular_less_than(oldOff, ProtectIdx); },
      [&](hazard_ptr* curr) {
        try_advance_hazptr_block(
          curr->write_block, ProtectIdx, newHead, curr->active_offset
        );
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
      newHead->offset.load(std::memory_order_relaxed),
      1 + read_offset.load(std::memory_order_acquire)
    ));
    assert(circular_less_than(
      newHead->offset.load(std::memory_order_relaxed),
      1 + write_offset.load(std::memory_order_acquire)
    ));
#endif
    return newHead;
  }

  void reclaim_blocks(data_block* OldHead, data_block* NewHead) {
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
        data_block* updatedTail = unlinked[0];
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
  void try_reclaim_blocks(hazard_ptr* Haz, size_t ProtectIdx) {
    data_block* oldHead = head_block.load(std::memory_order_acquire);
    size_t hazptrCheck = haz_ptr_counter.load(std::memory_order_seq_cst);
    data_block* newHead = try_advance_head(Haz, oldHead, ProtectIdx);
    if (newHead == oldHead) {
      return;
    }
    reclaim_counter.fetch_add(1, std::memory_order_seq_cst);
    if (hazptrCheck != haz_ptr_counter.load(std::memory_order_relaxed)) {
      // A hazard pointer was created during the reclaim operation.
      // It may have an outdated value of head.
      return;
    }
    head_block.store(newHead, std::memory_order_release);
    reclaim_blocks(oldHead, newHead);
  }

  // Given idx and a starting block, advance it until the block containing idx
  // is found.
  static inline data_block* find_block(data_block* Block, size_t Idx) {
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
  element* get_write_ticket(hazard_ptr* Haz, size_t& Idx) {
    data_block* block = Haz->write_block.load(std::memory_order_relaxed);
    Haz->active_offset.store(
      block->offset.load(std::memory_order_relaxed), std::memory_order_relaxed
    );
    // seq_cst is needed here to create a StoreLoad barrier between setting
    // hazptr and reloading the block
    Idx = write_offset.fetch_add(1, std::memory_order_seq_cst);
    // Reload block in case it was modified after setting hazptr offset
    block = Haz->write_block.load(std::memory_order_relaxed);
    assert(
      circular_less_than(block->offset.load(std::memory_order_relaxed), 1 + Idx)
    );
    // close() will set `closed` before incrementing offset.
    // Thus we are guaranteed to see it if we acquire offset first.
    if (closed.load(std::memory_order_relaxed)) {
      return nullptr;
    }
    block = find_block(block, Idx);
    // Update last known block.
    // Note that if hazptr was to an older block, that block will still be
    // protected. This prevents a channel consisting of a single block from
    // trying to unlink/link that block to itself.
    Haz->write_block.store(block, std::memory_order_relaxed);
    element* elem = &block->values[Idx & BlockSizeMask];
    return elem;
  }

  // Idx will be initialized by this function
  element* get_read_ticket(hazard_ptr* Haz, size_t& Idx) {
    data_block* block = Haz->read_block.load(std::memory_order_relaxed);
    Haz->active_offset.store(
      block->offset.load(std::memory_order_relaxed), std::memory_order_relaxed
    );
    // seq_cst is needed here to create a StoreLoad barrier between setting
    // hazptr and reloading the block
    Idx = read_offset.fetch_add(1, std::memory_order_seq_cst);
    // Reload block in case it was modified after setting hazptr offset
    block = Haz->read_block.load(std::memory_order_relaxed);
    assert(
      circular_less_than(block->offset.load(std::memory_order_relaxed), 1 + Idx)
    );
    // close() will set `closed` before incrementing offset.
    // Thus we are guaranteed to see it if we acquire offset first.
    if (closed.load(std::memory_order_acquire)) {
      // If closed, continue draining until the channel is empty.
      // Producers *may* produce elements up to write_closed_at, or they may
      // stop producing slightly sooner, in which case this consumer will be
      // woken by drain().
      if (circular_less_than(
            write_closed_at.load(std::memory_order_relaxed), 1 + Idx
          )) {
        // After channel is empty, we still need to mark each element as
        // finished. This is a side effect of using fetch_add - we are still
        // consuming indexes even if they aren't used.
        block = find_block(block, Idx);
        element* elem = &block->values[Idx & BlockSizeMask];
        elem->flags.store(3, std::memory_order_release);
        return nullptr;
      }
    }
    block = find_block(block, Idx);
    // Update last known block.
    // Note that if hazptr was to an older block, that block will still be
    // protected. This prevents a channel consisting of a single block from
    // trying to unlink/link that block to itself.
    Haz->read_block.store(block, std::memory_order_relaxed);
    // Try to reclaim old blocks. Checking for index 1 ensures that at least
    // this token's hazptr will already be advanced to the new block.
    // Only consumers participate in reclamation and only 1 consumer at a time.
    if ((Idx & BlockSizeMask) == 1 && blocks_lock.try_lock()) {
      // seq_cst to ensure we see any writer-protected blocks
      size_t protectIdx = write_offset.load(std::memory_order_seq_cst);
      try_reclaim_blocks(Haz, protectIdx);
      blocks_lock.unlock();
    }
    element* elem = &block->values[Idx & BlockSizeMask];
    return elem;
  }

  template <typename U> chan_err write_element(element* Elem, U&& Val) {
    if (Elem == nullptr) {
      return tmc::chan_err::CLOSED;
    }
    size_t flags = Elem->flags.load(std::memory_order_acquire);
    assert((flags & 1) == 0);

    // Check if consumer is waiting
    if (flags & 2) {
      // There was a consumer waiting for this data
      auto cons = Elem->consumer;
      // Still need to store so block can be freed
      Elem->flags.store(3, std::memory_order_release);
      cons->t.emplace(std::forward<U>(Val));
      tmc::detail::post_checked(
        cons->continuation_executor, std::move(cons->continuation), cons->prio
      );
      return tmc::chan_err::OK;
    }

    // No consumer waiting, store the data
    Elem->data.emplace(std::forward<U>(Val));

    // Finalize transaction
    size_t expected = 0;
    if (!Elem->flags.compare_exchange_strong(
          expected, 1, std::memory_order_acq_rel, std::memory_order_acquire
        )) {
      // Consumer started waiting for this data during our RMW cycle
      assert(expected == 2);
      auto cons = Elem->consumer;
      cons->t = std::move(Elem->data);
      tmc::detail::post_checked(
        cons->continuation_executor, std::move(cons->continuation), cons->prio
      );
      Elem->flags.store(3, std::memory_order_release);
    }
    return tmc::chan_err::OK;
  }

  template <typename U> chan_err post(hazard_ptr* Haz, U&& Val) {
    Haz->inc_write_count();
    // Get write ticket and associated block, protected by hazptr.
    size_t idx;
    element* elem = get_write_ticket(Haz, idx);

    // Store the data / wake any waiting consumers
    chan_err err = write_element(elem, std::forward<U>(Val));

    // Then release the hazard pointer
    Haz->active_offset.store(
      idx + InactiveHazptrOffset, std::memory_order_release
    );

    return err;
  }

public:
  class aw_pull : private tmc::detail::AwaitTagNoGroupCoAwait {
    channel& chan;
    hazard_ptr* haz_ptr;

    friend channel;
    friend chan_tok<T, Config>;

    aw_pull(channel& Chan, hazard_ptr* Haz) : chan(Chan), haz_ptr{Haz} {}

    struct aw_pull_impl {
      aw_pull& parent;
      tmc::detail::type_erased_executor* continuation_executor;
      std::coroutine_handle<> continuation;
      size_t prio;
      size_t thread_hint;
      element* elem;
      size_t release_idx;
      tmc::detail::channel_storage<T> t;
      chan_err err;

      aw_pull_impl(aw_pull& Parent)
          : parent{Parent}, err{tmc::chan_err::OK},
            continuation_executor{tmc::detail::this_thread::executor},
            continuation{nullptr},
            prio(tmc::detail::this_thread::this_task.prio),
            thread_hint(tmc::detail::this_thread::thread_index) {}
      bool await_ready() {
        parent.haz_ptr->inc_read_count();
        // Get read ticket and associated block, protected by hazptr.
        size_t idx;
        elem = parent.chan.get_read_ticket(parent.haz_ptr, idx);
        release_idx = idx + InactiveHazptrOffset;
        if (elem == nullptr) {
          err = tmc::chan_err::CLOSED;
          parent.haz_ptr->active_offset.store(
            release_idx, std::memory_order_release
          );
          return true;
        }

        if (parent.haz_ptr->should_suspend()) {
          if (parent.haz_ptr->write_count + parent.haz_ptr->read_count ==
              10000) {
            size_t elapsed = parent.haz_ptr->elapsed();
            size_t readerCount = 0;
            parent.haz_ptr->for_each_owned_hazptr(
              [&]() { return true; },
              [&](hazard_ptr* curr) {
                auto reads = curr->read_count.load(std::memory_order_relaxed);
                if (reads != 0) {
                  ++readerCount;
                }
              }
            );

            if (elapsed >= parent.haz_ptr->minCycles * readerCount) {
              // Just suspend without rebalancing (to allow other producers to
              // run)
              parent.haz_ptr->write_count.store(0, std::memory_order_relaxed);
              parent.haz_ptr->read_count.store(0, std::memory_order_relaxed);
              return false;
            }
          }
          // Try to get rti. Suspend if we can get it.
          // If we don't get it on this call to push(), don't suspend and try
          // again to get it on the next call.
          size_t rti = parent.haz_ptr->requested_thread_index.load(
            std::memory_order_relaxed
          );
          if (rti == -1) {
            if (parent.chan.try_cluster(parent.haz_ptr)) {
              rti = parent.haz_ptr->requested_thread_index.load(
                std::memory_order_relaxed
              );
            }
          }
          if (rti != -1) {
            parent.haz_ptr->write_count.store(0, std::memory_order_relaxed);
            parent.haz_ptr->read_count.store(0, std::memory_order_relaxed);
            return false;
          }
        }

        // Check if data is ready
        size_t flags = elem->flags.load(std::memory_order_acquire);
        assert((flags & 2) == 0);

        if (flags & 1) {
          // Data is already ready here.
          t = std::move(elem->data);
          // Still need to store so block can be freed
          elem->flags.store(3, std::memory_order_release);
          parent.haz_ptr->active_offset.store(
            release_idx, std::memory_order_release
          );
          return true;
        }
        size_t spins =
          parent.chan.ConsumerSpins.load(std::memory_order_relaxed);
        for (size_t i = 0; i < spins; ++i) {
          TMC_CPU_PAUSE();
          size_t flags = elem->flags.load(std::memory_order_acquire);
          assert((flags & 2) == 0);

          if (flags & 1) {
            // Data is already ready here.
            t = std::move(elem->data);
            // Still need to store so block can be freed
            elem->flags.store(3, std::memory_order_release);
            parent.haz_ptr->active_offset.store(
              release_idx, std::memory_order_release
            );
            return true;
          }
        }

        // If we suspend, hold on to the hazard pointer to keep the block alive
        return false;
      }
      bool await_suspend(std::coroutine_handle<> Outer) {
        // Data wasn't ready, prepare to suspend
        continuation = Outer;
        elem->consumer = this;

        size_t rti =
          parent.haz_ptr->requested_thread_index.load(std::memory_order_relaxed
          );
        if (rti != -1) {
          thread_hint = rti;
          parent.haz_ptr->requested_thread_index.store(
            -1, std::memory_order_relaxed
          );
        }

        // Finalize transaction
        size_t expected = 0;
        if (!elem->flags.compare_exchange_strong(
              expected, 2, std::memory_order_acq_rel, std::memory_order_acquire
            )) {
          // data became ready during our RMW cycle
          assert(expected == 1);
          t = std::move(elem->data);
          elem->flags.store(3, std::memory_order_release);
          parent.haz_ptr->active_offset.store(
            release_idx, std::memory_order_release
          );
          if (thread_hint != -1) {
            // Periodically suspend consumers to avoid starvation if producer is
            // running in same node
            tmc::detail::post_checked(
              continuation_executor, std::move(continuation), prio, thread_hint
            );
            return true;
          }
          return false;
        }
        return true;
      }

      // May return a value or CLOSED
      std::variant<T, chan_err> await_resume() {
        parent.haz_ptr->active_offset.store(
          release_idx, std::memory_order_release
        );
        if (err == tmc::chan_err::OK) {
          std::variant<T, chan_err> result(std::move(t.value));
          t.destroy();
          return result;
        } else {
          return err;
        }
      }
    };

  public:
    aw_pull_impl operator co_await() && { return aw_pull_impl(*this); }
  };

  class aw_push : private tmc::detail::AwaitTagNoGroupCoAwait {
    channel& chan;
    hazard_ptr* hazptr;
    T t;

    friend chan_tok<T, Config>;

    template <typename U>
    aw_push(channel& Chan, hazard_ptr* Haz, U u)
        : chan{Chan}, hazptr{Haz}, t{std::forward<U>(u)} {}

    struct aw_push_impl {
      aw_push& parent;
      tmc::chan_err result;

      aw_push_impl(aw_push& Parent) : parent{Parent} {}

      bool await_ready() {
        result = parent.chan.post(parent.hazptr, std::move(parent.t));
        if (parent.hazptr->should_suspend()) {
          if (parent.hazptr->write_count + parent.hazptr->read_count == 10000) {
            size_t elapsed = parent.hazptr->elapsed();
            size_t writerCount = 0;
            parent.hazptr->for_each_owned_hazptr(
              [&]() { return true; },
              [&](hazard_ptr* curr) {
                auto writes = curr->write_count.load(std::memory_order_relaxed);
                if (writes != 0) {
                  ++writerCount;
                }
              }
            );

            if (elapsed >= parent.hazptr->minCycles * writerCount) {
              // Just suspend without clustering (to allow other producers to
              // run)
              parent.hazptr->write_count.store(0, std::memory_order_relaxed);
              parent.hazptr->read_count.store(0, std::memory_order_relaxed);
              return false;
            }
          }

          // Try to get rti. Suspend if we can get it.
          // If we don't get it on this call to push(), don't suspend and try
          // again to get it on the next call.
          size_t rti =
            parent.hazptr->requested_thread_index.load(std::memory_order_relaxed
            );
          if (rti == -1) {
            if (parent.chan.try_cluster(parent.hazptr)) {
              rti = parent.hazptr->requested_thread_index.load(
                std::memory_order_relaxed
              );
            }
          }
          if (rti != -1) {
            parent.hazptr->write_count.store(0, std::memory_order_relaxed);
            parent.hazptr->read_count.store(0, std::memory_order_relaxed);
            return false;
          }
        }

        return true;
      }

      void await_suspend(std::coroutine_handle<> Outer) {
        size_t target =
          static_cast<size_t>(parent.hazptr->requested_thread_index);
        parent.hazptr->requested_thread_index.store(
          -1, std::memory_order_relaxed
        );
        tmc::detail::post_checked(
          tmc::detail::this_thread::executor, std::move(Outer),
          tmc::detail::this_thread::this_task.prio, target
        );
      }

      tmc::chan_err await_resume() { return result; }
    };

  public:
    aw_push_impl operator co_await() && { return aw_push_impl(*this); }
  };

private:
  void close() {
    tmc::tiny_lock_guard lg(blocks_lock);
    if (0 != closed.load(std::memory_order_relaxed)) {
      return;
    }
    size_t expected = 0;
    size_t woff = write_offset.load(std::memory_order_seq_cst);
    // Setting this to a distant-but-greater value before setting closed
    // prevents consumers from exiting too early.
    write_closed_at.store(
      woff + InactiveHazptrOffset, std::memory_order_seq_cst
    );

    closed.store(1, std::memory_order_seq_cst);

    // Now mark the real closed_at index. Past this index, producers are
    // guaranteed to not produce.
    write_closed_at.store(
      write_offset.fetch_add(1, std::memory_order_seq_cst),
      std::memory_order_seq_cst
    );
  }

  void reopen() {
    tmc::tiny_lock_guard lg(blocks_lock);
    if (0 == closed.load(std::memory_order_relaxed)) {
      return;
    }
    closed.store(0, std::memory_order_seq_cst);

    size_t woff = write_offset.load(std::memory_order_seq_cst);
    write_closed_at.store(
      woff + InactiveHazptrOffset, std::memory_order_seq_cst
    );
  }

  void drain_wait() {
    close(); // close() is idempotent and a precondition to call this.
    blocks_lock.spin_lock();
    size_t woff = write_closed_at.load(std::memory_order_seq_cst);
    size_t roff = read_offset.load(std::memory_order_seq_cst);

    // Fast-path reclaim blocks up to the earlier of read or write index
    size_t protectIdx = woff;
    keep_min(protectIdx, roff);
    hazard_ptr* hazptr = hazard_ptr_list.load(std::memory_order_relaxed);
    try_reclaim_blocks(hazptr, protectIdx);

    data_block* block = head_block.load(std::memory_order_seq_cst);
    size_t i = block->offset.load(std::memory_order_relaxed);

    // Slow-path wait for the channel to drain.
    // Check each element prior to write_closed_at write index.
    while (true) {
      while (circular_less_than(i, roff) && circular_less_than(i, woff)) {
        size_t idx = i & BlockSizeMask;
        auto v = &block->values[idx];
        // Data is present at these elements; wait for consumer
        while (v->flags.load(std::memory_order_acquire) != 3) {
          TMC_CPU_PAUSE();
        }

        ++i;
        if ((i & BlockSizeMask) == 0) {
          data_block* next = block->next.load(std::memory_order_acquire);
          while (next == nullptr) {
            // A block is being constructed; wait for it
            TMC_CPU_PAUSE();
            next = block->next.load(std::memory_order_acquire);
          }
          block = next;
        }
      }
      if (circular_less_than(roff, woff)) {
        // Wait for readers to catch up.
        TMC_CPU_PAUSE();
        size_t newRoff = read_offset.load(std::memory_order_seq_cst);
        roff = newRoff;
      } else {
        break;
      }
    }

    // i >= woff now and all data has been drained.
    // Now handle waking up waiting consumers.

    // `closed` is accessed by relaxed load in consumer.
    // In order to ensure that it is seen in a timely fashion, this
    // creates a release sequence with the acquire load in consumer.

    if (closed.load() != 3) {
      read_closed_at.store(read_offset.fetch_add(1, std::memory_order_relaxed));
      closed.store(3, std::memory_order_seq_cst);
    }
    roff = read_closed_at.load(std::memory_order_relaxed);

    // No data will be written to these elements. They are past the
    // write_closed_at write index. `roff` is now read_closed_at.
    // Consumers may be waiting at indexes prior to `roff`.
    while (circular_less_than(i, roff)) {
      size_t idx = i & BlockSizeMask;
      auto v = &block->values[idx];

      // Wait for consumer to appear
      size_t flags = v->flags.load(std::memory_order_acquire);
      while ((flags & 2) == 0) {
        TMC_CPU_PAUSE();
        flags = v->flags.load(std::memory_order_acquire);
      }

      // If flags & 1 is set, it indicates the consumer saw the closed flag
      // and did not wait. Otherwise, wakeup the waiting consumer.
      if ((flags & 1) == 0) {
        auto cons = v->consumer;
        cons->err = tmc::chan_err::CLOSED;
        tmc::detail::post_checked(
          cons->continuation_executor, std::move(cons->continuation), cons->prio
        );
      }

      ++i;
      if (circular_less_than(roff, 1 + i)) {
        break;
      }
      if ((i & BlockSizeMask) == 0) {
        data_block* next = block->next.load(std::memory_order_acquire);
        while (next == nullptr) {
          // A block is being constructed; wait for it
          TMC_CPU_PAUSE();
          next = block->next.load(std::memory_order_acquire);
        }
        block = next;
      }
    }
    blocks_lock.unlock();
  }

public:
  ~channel() {
    {
      // Since tokens share ownership of channel, at this point there can be no
      // active tokens. However it is possible that data was pushed to the
      // channel without being pulled. Run destructors for this data.
      close(); // ensure write_closed_at exists
      size_t woff = write_closed_at.load(std::memory_order_relaxed);
      size_t idx = read_offset.load(std::memory_order_relaxed);
      data_block* block = head_block.load(std::memory_order_acquire);
      while (circular_less_than(idx, woff)) {
        block = find_block(block, idx);
        element* elem = &block->values[idx & BlockSizeMask];
        if (1 == elem->flags.load(std::memory_order_relaxed)) {
          elem->data.destroy();
        }
        ++idx;
      }
    }
    {
      data_block* block = head_block.load(std::memory_order_acquire);
      while (block != nullptr) {
        data_block* next = block->next.load(std::memory_order_acquire);
        delete block;
        block = next;
      }
    }
    {
      hazard_ptr* start = hazard_ptr_list.load(std::memory_order_relaxed);
      hazard_ptr* curr = start;
      while (true) {
        uintptr_t next_raw = curr->next;
        assert((next_raw & IS_OWNED_BIT) == 0);
        hazard_ptr* next =
          reinterpret_cast<hazard_ptr*>(next_raw & ~IS_OWNED_BIT);
        delete curr;
        if (next == start) {
          break;
        }
        curr = next;
      }
    }
  }

  channel(const channel&) = delete;
  channel& operator=(const channel&) = delete;
  channel(channel&&) = delete;
  channel& operator=(channel&&) = delete;
};

template <typename T, typename Config> class chan_tok {
  using chan_t = channel<T, Config>;
  using hazard_ptr = chan_t::hazard_ptr;
  std::shared_ptr<chan_t> chan;
  hazard_ptr* haz_ptr;
  NO_CONCURRENT_ACCESS_LOCK;

  friend chan_tok make_channel<T, Config>();

  chan_tok(std::shared_ptr<chan_t>&& Chan)
      : chan{std::move(Chan)}, haz_ptr{nullptr} {}

  hazard_ptr* get_hazard_ptr() {
    if (haz_ptr == nullptr) {
      haz_ptr = chan->get_hazard_ptr();
    }
    return haz_ptr;
  }

  void free_hazard_ptr() {
    if (haz_ptr != nullptr) {
      haz_ptr->release_ownership();
      haz_ptr = nullptr;
    }
  }

public:
  /// The new chan_tok will have its own hazard pointer so that it can be
  /// used concurrently with this token.
  chan_tok(const chan_tok& Other) : chan(Other.chan), haz_ptr{nullptr} {}

  /// This token can "become" another token, even if that token is to a
  /// different channel (as long as the channels have the same template
  /// parameters).
  chan_tok& operator=(const chan_tok& Other) {
    if (chan != Other.chan) {
      free_hazard_ptr();
      chan = Other.chan;
    }
  }

  /// The moved-from token will become empty; it will release its channel
  /// pointer, and its hazard pointer.
  chan_tok(chan_tok&& Other)
      : chan(std::move(Other.chan)), haz_ptr{Other.haz_ptr} {
    Other.haz_ptr = nullptr;
  }

  /// This token can "become" another token, even if that token is to a
  /// different channel (as long as the channels have the same template
  /// parameters).
  ///
  /// The moved-from token will become empty; it will release its channel
  /// pointer, and its hazard pointer.
  chan_tok& operator=(chan_tok&& Other) {
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

  ~chan_tok() { free_hazard_ptr(); }

  /// May return OK or CLOSED. Will not suspend or block.
  template <typename U> chan_err post(U&& u) {
    ASSERT_NO_CONCURRENT_ACCESS();
    hazard_ptr* hazptr = get_hazard_ptr();
    return chan->post(hazptr, std::forward<U>(u));
  }

  /// May return OK or CLOSED. May suspend to do producer clustering under high
  /// load.
  template <typename U>
  [[nodiscard("You must co_await push().")]] chan_t::aw_push push(U&& u) {
    ASSERT_NO_CONCURRENT_ACCESS();
    hazard_ptr* hazptr = get_hazard_ptr();
    return typename chan_t::aw_push(*chan, hazptr, std::forward<U>(u));
  }

  /// May return a value (in variant index 0) or CLOSED (in variant index 1).
  /// If no value is ready, will suspend until a value becomes ready, or the
  /// queue is drained.
  [[nodiscard("You must co_await pull().")]] chan_t::aw_pull pull() {
    ASSERT_NO_CONCURRENT_ACCESS();
    hazard_ptr* hazptr = get_hazard_ptr();
    return typename chan_t::aw_pull(*chan, hazptr);
  }

  /// All future producers will return CLOSED.
  /// Consumers will continue to read data until the channel is drained,
  /// at which point all consumers will return CLOSED.
  ///
  /// This function is idempotent and thread-safe. It is not lock-free. It may
  /// contend the lock against `close()`, `reopen()`, and `drain()`.
  void close() { chan->close(); }

  /// If the channel is not already closed, it will be closed.
  /// Then, waits for consumers to drain all remaining data from the channel.
  /// After all data has been consumed from the channel, any waiting consumers
  /// will be awakened, and all current and future consumers will immediately
  /// return CLOSED.
  ///
  /// This function is idempotent and thread-safe. It is not lock-free. It may
  /// contend the lock against `close()`, `reopen()`, and `drain()`.
  void drain_wait() { chan->drain_wait(); }

  /// If the queue was closed, it will be reopened. Producers may submit work
  /// again and consumers may consume or await work. If the queue was not
  /// previously closed, this will have no effect.
  ///
  /// Note that if you simply call `close()` + `drain()` followed by `reopen()`,
  /// consumers that do not attempt to access the channel during this time will
  /// not see the CLOSED signal; it may appear to them as if the queue
  /// remained open the entire time. If you want to ensure that all consumers
  /// see the CLOSED signal before reopening the queue, you will need to use
  /// external synchronization.
  ///
  /// This function is idempotent and thread-safe. It is not lock-free. It may
  /// contend the lock against `close()`, `reopen()`, and `drain()`.
  void reopen() { chan->reopen(); }

  /// If true, spent blocks will be cleared and moved to the tail of the queue.
  /// If false, spent blocks will be deleted.
  /// Default: true
  chan_tok& set_reuse_blocks(bool Reuse) {
    chan->ReuseBlocks.store(Reuse, std::memory_order_relaxed);
    return *this;
  }

  /// If a consumer sees no data is ready at a ticket, it will spin wait this
  /// many times. Each spin wait is an asm("pause") and reload.
  /// Default: 0
  chan_tok& set_consumer_spins(size_t SpinCount) {
    chan->ConsumerSpins.store(SpinCount, std::memory_order_relaxed);
    return *this;
  }

  /// If the total number of elements pushed per second to the queue is greater
  /// than this threshold, then the queue will attempt to move producers and
  /// consumers near each other to optimize sharing efficiency. The default
  /// value of 2,000,000 represents an item being pushed every 500ns. This
  /// behavior can be disabled entirely by setting this to 0.
  chan_tok& set_heavy_load_threshold(size_t Threshold) {
    // The magic number 10000 corresponds to the number of elements that must
    // be produced or consumed before the heuristic is checked.
    size_t cycles = Threshold == 0 ? 0 : TMC_CPU_FREQ * 10000 / Threshold;
    chan->MinClusterCycles.store(cycles, std::memory_order_relaxed);
    return *this;
  }
};

template <typename T, typename Config>
static inline chan_tok<T, Config> make_channel() {
  auto chan = new channel<T, Config>();
  return chan_tok{std::shared_ptr<channel<T, Config>>(chan)};
}

} // namespace tmc
