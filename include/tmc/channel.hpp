// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

// Provides tmc::channel, an async MPMC queue of unbounded size.
// Producers enqueue values with push().
// Consumers retrieve values in FIFO order with co_await pull().
// If no values are available, the consumer will suspend until a value is ready.

// The hazard pointer scheme is loosely based on
// 'A wait-free queue as fast as fetch-and-add' by Yang & Mellor-Crummey
// https://dl.acm.org/doi/10.1145/2851141.2851168

#include "tmc/aw_yield.hpp"
#include "tmc/detail/compat.hpp"
#include "tmc/detail/concepts.hpp"
#include "tmc/detail/thread_locals.hpp"
#include "tmc/detail/tiny_lock.hpp"
#include "tmc/task.hpp"

#include <array>
#include <atomic>
#include <coroutine>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>
#include <variant>

// TODO mask with highest bit set to 0 when comparing indexes

namespace tmc {

enum class channel_error { OK = 0, CLOSED = 1, EMPTY = 2 };

struct channel_default_config {
  static inline constexpr size_t BlockSize = 4096;
  static inline constexpr size_t ReuseBlocks = true;
  static inline constexpr size_t ConsumerSpins = 0;
  static inline constexpr size_t PackingLevel = 0;
};

/// channel_token allows access to a channel.
/// Access to the channel (from multiple tokens) is thread-safe,
/// but access to a single token from multiple threads is not.
/// To access the channel from multiple threads or tasks concurrently,
/// make a copy of the token for each.
template <typename T, typename Config = tmc::channel_default_config>
class channel_token;
template <typename T, typename Config> struct aw_push;

/// Creates a new channel and returns an access token to it.
/// Tokens share ownership of a channel by reference counting.
/// Access to the channel (from multiple tokens) is thread-safe,
/// but access to a single token from multiple threads is not.
/// To access the channel from multiple threads or tasks concurrently,
/// make a copy of the token for each.
template <typename T, typename Config = tmc::channel_default_config>
static inline channel_token<T, Config> make_channel();

template <typename T, typename Config = tmc::channel_default_config>
class channel {
  static inline constexpr size_t BlockSize = Config::BlockSize;
  static_assert(
    BlockSize && ((BlockSize & (BlockSize - 1)) == 0),
    "BlockSize must be a power of 2"
  );

  static constexpr size_t BlockSizeMask = BlockSize - 1;

  friend channel_token<T, Config>;
  friend aw_push<T, Config>;
  template <typename Tc, typename Cc>
  friend channel_token<Tc, Cc> make_channel();

public:
  class aw_pull;

private:
  struct element {
    std::atomic<size_t> flags;
    aw_pull* consumer;
    T data;
    static constexpr size_t UNPADLEN =
      sizeof(size_t) + sizeof(void*) + sizeof(T);
    static constexpr size_t PADLEN = UNPADLEN < 64 ? (64 - UNPADLEN) : 0;
    char pad[PADLEN];
  };

  struct data_block {
    size_t offset;
    std::atomic<data_block*> next;
    std::array<element, BlockSize> values;

    void reset_values() {
      for (size_t i = 0; i < BlockSize; ++i) {
        values[i].flags.store(0, std::memory_order_relaxed);
      }
    }

    data_block(size_t Offset) {
      offset = Offset;
      reset_values();
      next.store(nullptr, std::memory_order_release);
    }
  };

  // Pointer tagging the next ptr allows for efficient search
  // of owned or unowned hazptrs in the list.
public:
  static inline constexpr size_t IS_OWNED_BIT = TMC_ONE_BIT << 60;
  struct alignas(64) hazard_ptr {
    std::atomic<uintptr_t> next;
    std::atomic<size_t> active_offset;
    std::atomic<data_block*> write_block;
    std::atomic<data_block*> read_block;

    hazard_ptr() : active_offset{TMC_ALL_ONES} {}

    bool try_take_ownership() {
      return (next.fetch_or(IS_OWNED_BIT) & IS_OWNED_BIT) == 0;
    }
    void release_ownership() {
      [[maybe_unused]] bool ok =
        (next.fetch_and(~IS_OWNED_BIT) & IS_OWNED_BIT) != 0;
      assert(ok);
    }
  };

  static_assert(std::atomic<size_t>::is_always_lock_free);
  static_assert(std::atomic<data_block*>::is_always_lock_free);

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
  char pad2[64 - 1 * (sizeof(void*))];
  // Access to this are currently all relaxed due to being inside blocks_lock.
  // If hazptr list access becomes lock-free then those atomic operations will
  // need to be strengthened.
  std::atomic<hazard_ptr*> hazard_ptr_list;

  channel()
      : closed{0}, write_closed_at{TMC_ALL_ONES}, read_closed_at{TMC_ALL_ONES} {
    auto block = new data_block(0);
    head_block = block;
    tail_block = block;
    read_offset = 0;
    write_offset = 0;
    hazard_ptr* hazptr = new hazard_ptr;
    hazptr->next = 0;
    hazard_ptr_list = hazptr;
  }

  // Gets a hazard pointer from the list, and takes ownership of it.
  hazard_ptr* get_hazard_ptr() {
    // Mutex with block reclamation, which reads from the hazard pointer list
    tmc::tiny_lock_guard lg{blocks_lock};

    hazard_ptr* ptr = hazard_ptr_list.load(std::memory_order_relaxed);
    uintptr_t next_raw = ptr->next.load(std::memory_order_acquire);
    uintptr_t is_owned = next_raw & IS_OWNED_BIT;
    while (true) {
      if ((is_owned == 0) && ptr->try_take_ownership()) {
        break;
      }
      hazard_ptr* next =
        reinterpret_cast<hazard_ptr*>(next_raw & ~IS_OWNED_BIT);
      next_raw = IS_OWNED_BIT;
      if (next == nullptr) {
        hazard_ptr* newptr = new hazard_ptr;
        newptr->next = IS_OWNED_BIT;
        uintptr_t store = reinterpret_cast<uintptr_t>(newptr) | IS_OWNED_BIT;
        if (ptr->next.compare_exchange_strong(
              next_raw, store, std::memory_order_acq_rel,
              std::memory_order_acquire
            )) {
          // New hazard_ptrs are created in an owned state
          ptr = newptr;
          break;
        } else {
          delete newptr;
          is_owned = next_raw & IS_OWNED_BIT;
          if (is_owned == 0) {
            // This hazard_ptr was released, try again to take it
            continue;
          }
          next = reinterpret_cast<hazard_ptr*>(next_raw & ~IS_OWNED_BIT);
        }
      }
      ptr = next;
      next_raw = ptr->next.load(std::memory_order_acquire);
      is_owned = next_raw & IS_OWNED_BIT;
    }
    ptr->read_block = head_block.load(std::memory_order_relaxed);
    ptr->write_block = head_block.load(std::memory_order_relaxed);
    return ptr;
  }

  // Load src and move it into dst if src < dst.
  static inline void keep_min(size_t& Dst, std::atomic<size_t> const& Src) {
    size_t val = Src.load(std::memory_order_acquire);
    if (val < Dst) {
      Dst = val;
    }
  }

  // Move src into dst if src < dst.
  static inline void keep_min(size_t& Dst, size_t Src) {
    if (Src < Dst) {
      Dst = Src;
    }
  }

  // Advances DstBlock to be equal to NewHead. Possibly reduces MinProtect if
  // DstBlock was already updated by its owning thread.
  void try_advance_hazptr_block(
    std::atomic<data_block*>& DstBlock, size_t& MinProtected,
    data_block* NewHead, std::atomic<size_t> const& HazardOffset
  ) {
    data_block* block = DstBlock.load(std::memory_order_acquire);
    if (block->offset < NewHead->offset) {
      if (!DstBlock.compare_exchange_strong(
            block, NewHead, std::memory_order_seq_cst
          )) {
        // If this hazptr updated its own block, but the updated block is
        // still earlier than the new head, then we cannot free that block.
        keep_min(MinProtected, block->offset);
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
  data_block* try_advance_head(data_block* OldHead, size_t ProtectIdx) {
    // In the current implementation, this is called only from consumers.
    // Therefore, this token's hazptr will be active, and protecting read_block.
    // However, if producers are lagging behind, and no producer is currently
    // active, write_block would not be protected. Therefore, write_offset
    // should be passed to ProtectIdx to cover this scenario.
    ProtectIdx = ProtectIdx & ~BlockSizeMask; // round down to block index

    // Find the lowest offset that is protected by ProtectIdx or any hazptr.
    hazard_ptr* curr = hazard_ptr_list.load(std::memory_order_relaxed);
    while (ProtectIdx > OldHead->offset && curr != nullptr) {
      uintptr_t next_raw = curr->next.load(std::memory_order_acquire);
      hazard_ptr* next =
        reinterpret_cast<hazard_ptr*>(next_raw & ~IS_OWNED_BIT);
      uintptr_t is_owned = next_raw & IS_OWNED_BIT;
      if (is_owned) {
        keep_min(ProtectIdx, curr->active_offset);
      }
      curr = next;
    }

    // If head block is protected, nothing can be reclaimed.
    if (ProtectIdx == OldHead->offset) {
      return OldHead;
    }

    // Find the block associated with this offset.
    data_block* newHead = OldHead;
    while (newHead->offset < ProtectIdx) {
      newHead = newHead->next;
    }

    // Then update all hazptrs to be at this block or later.
    curr = hazard_ptr_list.load(std::memory_order_relaxed);
    while (ProtectIdx > OldHead->offset && curr != nullptr) {
      uintptr_t next_raw = curr->next.load(std::memory_order_acquire);
      hazard_ptr* next =
        reinterpret_cast<hazard_ptr*>(next_raw & ~IS_OWNED_BIT);
      uintptr_t is_owned = next_raw & IS_OWNED_BIT;
      if (is_owned) {
        try_advance_hazptr_block(
          curr->write_block, ProtectIdx, newHead, curr->active_offset
        );
        try_advance_hazptr_block(
          curr->read_block, ProtectIdx, newHead, curr->active_offset
        );
      }
      curr = next;
    }

    // minProtected may have been reduced by the double-check in
    // try_advance_block. If so, reduce newHead as well.
    if (ProtectIdx < newHead->offset) {
      newHead = OldHead;
      while (newHead->offset < ProtectIdx) {
        newHead = newHead->next;
      }
    }

#ifndef NDEBUG
    assert(newHead->offset <= read_offset.load(std::memory_order_acquire));
    assert(newHead->offset <= write_offset.load(std::memory_order_acquire));
#endif
    return newHead;
  }

  void reclaim_blocks(data_block* OldHead, data_block* NewHead) {
    if constexpr (!Config::ReuseBlocks) {
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
          size_t boff = tailBlock->offset + BlockSize;
          for (; i < unlinkedCount - 1; ++i) {
            data_block* b = unlinked[i];
            b->offset = boff;
            b->next.store(unlinked[i + 1], std::memory_order_release);
            boff += BlockSize;
          }

          unlinked[i]->offset = boff;
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
  void try_reclaim_blocks(size_t ProtectIdx) {
    data_block* oldHead = head_block.load(std::memory_order_acquire);
    data_block* newHead = try_advance_head(oldHead, ProtectIdx);
    if (newHead == oldHead) {
      return;
    }
    head_block.store(newHead, std::memory_order_release);
    reclaim_blocks(oldHead, newHead);
  }

  // Given idx and a starting block, advance it until the block containing idx
  // is found.
  data_block* find_block(data_block* Block, size_t Idx) {
    size_t offset = Block->offset;
    // Find or allocate the associated block
    while (offset + BlockSize <= Idx) {
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
      assert(Block->offset == offset);
    }
    size_t boff = Block->offset;
    assert(Idx >= boff && Idx < boff + BlockSize);
    return Block;
  }

  element* get_write_ticket(hazard_ptr* Haz) {
    data_block* block = Haz->write_block.load(std::memory_order_acquire);
    Haz->active_offset.store(block->offset, std::memory_order_release);
    // seq_cst is needed here to create a StoreLoad barrier between setting
    // hazptr and reloading the block
    size_t idx = write_offset.fetch_add(1, std::memory_order_seq_cst);
    // Reload block in case it was modified after setting hazptr offset
    block = Haz->write_block.load(std::memory_order_acquire);
    assert(idx >= block->offset);
    // close() will set `closed` before incrementing offset.
    // Thus we are guaranteed to see it if we acquire offset first.
    if (closed.load(std::memory_order_acquire)) {
      return nullptr;
    }
    block = find_block(block, idx);
    // Update last known block.
    // Note that if hazptr was to an older block, that block will still be
    // protected. This prevents a channel consisting of a single block from
    // trying to unlink/link that block to itself.
    Haz->write_block.store(block, std::memory_order_release);
    element* elem = &block->values[idx & BlockSizeMask];
    return elem;
  }

  element* get_read_ticket(hazard_ptr* Haz) {
    data_block* block = Haz->read_block.load(std::memory_order_acquire);
    Haz->active_offset.store(block->offset, std::memory_order_release);
    // seq_cst is needed here to create a StoreLoad barrier between setting
    // hazptr and reloading the block
    size_t idx = read_offset.fetch_add(1, std::memory_order_seq_cst);
    // Reload block in case it was modified after setting hazptr offset
    block = Haz->read_block.load(std::memory_order_acquire);
    assert(idx >= block->offset);
    // close() will set `closed` before incrementing offset.
    // Thus we are guaranteed to see it if we acquire offset first.
    if (closed.load(std::memory_order_acquire)) {
      // If closed, continue draining until the channel is empty
      // As indicated by the write_closed_at index
      if (idx >= write_closed_at.load(std::memory_order_relaxed)) {
        // After channel is empty, we still need to mark each element as
        // finished. This is a side effect of using fetch_add - we are still
        // consuming indexes even if they aren't used.
        block = find_block(block, idx);
        element* elem = &block->values[idx & BlockSizeMask];
        elem->flags.store(3, std::memory_order_release);
        return nullptr;
      }
    }
    block = find_block(block, idx);
    // Update last known block.
    // Note that if hazptr was to an older block, that block will still be
    // protected. This prevents a channel consisting of a single block from
    // trying to unlink/link that block to itself.
    Haz->read_block.store(block, std::memory_order_release);
    // Try to reclaim old blocks. Checking for index 1 ensures that at least
    // this token's hazptr will already be advanced to the new block.
    // Only consumers participate in reclamation and only 1 consumer at a time.
    if ((idx & BlockSizeMask) == 1 && blocks_lock.try_lock()) {
      // TODO just do a seq_cst load instead?
      size_t protectIdx = write_offset.load(std::memory_order_relaxed);
      write_offset.compare_exchange_strong(
        protectIdx, protectIdx, std::memory_order_seq_cst
      );
      try_reclaim_blocks(protectIdx);
      blocks_lock.unlock();
    }
    element* elem = &block->values[idx & BlockSizeMask];
    return elem;
  }

  template <typename U>
  channel_error write_element(element* Elem, U&& Val, bool& consWaiting) {
    if (Elem == nullptr) {
      return tmc::channel_error::CLOSED;
    }
    size_t flags = Elem->flags.load(std::memory_order_acquire);
    assert((flags & 1) == 0);

    // Check if consumer is waiting
    if (flags & 2) {
      consWaiting = true;
      // There was a consumer waiting for this data
      auto cons = Elem->consumer;
      // Still need to store so block can be freed
      Elem->flags.store(3, std::memory_order_release);
      cons->t = (1 << 31) | Val; // TODO remove this - it's for debugging
      tmc::detail::post_checked(
        cons->continuation_executor, std::move(cons->continuation), cons->prio,
        cons->thread_hint
      );
      return tmc::channel_error::OK;
    }

    // No consumer waiting, store the data
    Elem->data = std::forward<U>(Val);

    // Finalize transaction
    size_t expected = 0;
    if (!Elem->flags.compare_exchange_strong(
          expected, 1, std::memory_order_acq_rel, std::memory_order_acquire
        )) {
      // Consumer started waiting for this data during our RMW cycle
      assert(expected == 2);
      auto cons = Elem->consumer;
      cons->t = (1 << 31) | Elem->data;
      tmc::detail::post_checked(
        cons->continuation_executor, std::move(cons->continuation), cons->prio,
        cons->thread_hint
      );
      Elem->flags.store(3, std::memory_order_release);
    }
    return tmc::channel_error::OK;
  }

  template <typename U>
  channel_error push(U&& Val, hazard_ptr* Haz, bool& consWaiting) {
    // Get write ticket and associated block, protected by hazptr.
    element* elem = get_write_ticket(Haz);

    // Store the data / wake any waiting consumers
    channel_error err = write_element(elem, std::forward<U>(Val), consWaiting);

    // Then release the hazard pointer
    Haz->active_offset.store(TMC_ALL_ONES, std::memory_order_release);

    return err;
  }

public:
  class aw_pull : private tmc::detail::AwaitTagNoGroupAsIs {
    friend channel_token<T, Config>;
    friend aw_push<T, Config>;
    T t; // TODO handle non-default-constructible types
    channel& chan;
    channel_error err;
    tmc::detail::type_erased_executor* continuation_executor;
    std::coroutine_handle<> continuation;
    size_t prio;
    size_t thread_hint;
    hazard_ptr* haz_ptr;
    element* elem;

    aw_pull(channel& Chan, hazard_ptr* Haz)
        : chan(Chan), err{tmc::channel_error::OK},
          continuation_executor{tmc::detail::this_thread::executor},
          continuation{nullptr}, prio(tmc::detail::this_thread::this_task.prio),
          thread_hint(tmc::detail::this_thread::thread_index), haz_ptr{Haz} {}

    friend channel;

  public:
    bool await_ready() {
      // Get read ticket and associated block, protected by hazptr.
      elem = chan.get_read_ticket(haz_ptr);
      if (elem == nullptr) {
        err = tmc::channel_error::CLOSED;
        haz_ptr->active_offset.store(TMC_ALL_ONES, std::memory_order_release);
        return true;
      }

      // Check if data is ready
      size_t flags = elem->flags.load(std::memory_order_acquire);
      assert((flags & 2) == 0);

      if (flags & 1) {
        // Data is already ready here.
        t = std::move(elem->data);
        // Still need to store so block can be freed
        elem->flags.store(3, std::memory_order_release);
        haz_ptr->active_offset.store(TMC_ALL_ONES, std::memory_order_release);
        return true;
      }
      for (size_t i = 0; i < Config::ConsumerSpins; ++i) {
        TMC_CPU_PAUSE();
        size_t flags = elem->flags.load(std::memory_order_acquire);
        assert((flags & 2) == 0);

        if (flags & 1) {
          // Data is already ready here.
          t = std::move(elem->data);
          // Still need to store so block can be freed
          elem->flags.store(3, std::memory_order_release);
          haz_ptr->active_offset.store(TMC_ALL_ONES, std::memory_order_release);
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

      // Finalize transaction
      size_t expected = 0;
      if (!elem->flags.compare_exchange_strong(
            expected, 2, std::memory_order_acq_rel, std::memory_order_acquire
          )) {
        // data became ready during our RMW cycle
        assert(expected == 1);
        t = std::move(elem->data);
        elem->flags.store(3, std::memory_order_release);
        haz_ptr->active_offset.store(TMC_ALL_ONES, std::memory_order_release);
        return false;
      }
      return true;
    }

    // May return a value or CLOSED
    std::variant<T, channel_error> await_resume() {
      haz_ptr->active_offset.store(TMC_ALL_ONES, std::memory_order_release);
      if (err == tmc::channel_error::OK) {
        return std::move(t);
      } else {
        return err;
      }
    }
  };

private:
  // May return a value, or EMPTY or CLOSED
  std::variant<T, channel_error> try_pull();

  // May return a value or CLOSED
  aw_pull pull(hazard_ptr* Haz) { return aw_pull(*this, Haz); }

  // TODO separate drain() (async) and drain_sync()
  // Store the drain async continuation at read_closed_at.
  // Which consumer is the last consumer to resume the continuation?
  // Based on releasing hazptr ownership? - would be unreliable
  // Channel shared_ptr use_count - assumes consumers actually release the queue
  // when they stop consuming

  // All currently waiting producers will return CLOSED.
  // Consumers will continue to read data until the channel is drained,
  // at which point all consumers will return CLOSED.
  void close() {
    size_t expected = 0;
    if (!closed.compare_exchange_strong(
          expected, 1, std::memory_order_seq_cst, std::memory_order_seq_cst
        )) {
      return;
    }
    write_closed_at.store(
      write_offset.fetch_add(1, std::memory_order_seq_cst),
      std::memory_order_seq_cst
    );
  }

  // If the channel is not already closed, it will be closed.
  // Then, waits for the channel to drain.
  // After all data has been consumed from the channel,
  // all consumers will return CLOSED.
  tmc::task<void> drain() {
    close(); // close() is idempotent and a precondition to call this.
    blocks_lock.spin_lock();
    size_t woff = write_closed_at.load(std::memory_order_seq_cst);
    size_t roff = read_offset.load(std::memory_order_seq_cst);

    // Fast-path reclaim blocks up to the earlier of read or write index
    size_t protectIdx = woff;
    keep_min(protectIdx, roff);
    try_reclaim_blocks(protectIdx);

    data_block* block = head_block.load(std::memory_order_seq_cst);
    size_t i = block->offset;

    // Slow-path wait for the channel to drain.
    // Check each element prior to write_closed_at write index.
    size_t consumerWaitSpins = 0;
    while (true) {
      while (i < roff && i < woff) {
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
      if (roff < woff) {
        // Wait for readers to catch up.
        TMC_CPU_PAUSE();
        size_t newRoff = read_offset.load(std::memory_order_seq_cst);
        if (roff == newRoff) {
          ++consumerWaitSpins;
          if (consumerWaitSpins == 10) {
            // If we spun 10 times without seeing roff change, we may be
            // deadlocked with a consumer running on this thread.
            consumerWaitSpins = 0;
            co_await yield();
          }
        }
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
      read_closed_at.store(read_offset.fetch_add(1, std::memory_order_seq_cst));
      closed.store(3, std::memory_order_seq_cst);
    }
    roff = read_closed_at.load(std::memory_order_seq_cst);

    // No data will be written to these elements. They are past the
    // write_closed_at write index. `roff` is now read_closed_at.
    // Consumers may be waiting at indexes prior to `roff`.
    while (i < roff) {
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
        cons->err = tmc::channel_error::CLOSED;
        tmc::detail::post_checked(
          cons->continuation_executor, std::move(cons->continuation),
          cons->prio, cons->thread_hint
        );
      }

      ++i;
      if (i >= roff) {
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
    assert(blocks_lock.try_lock());
    data_block* block = head_block.load(std::memory_order_acquire);
    size_t idx = block->offset;
    while (block != nullptr) {
      data_block* next = block->next.load(std::memory_order_acquire);
      delete block;
      block = next;
      idx = idx + BlockSize;
    }
    hazard_ptr* hazptr = hazard_ptr_list.load(std::memory_order_relaxed);
    while (hazptr != nullptr) {
      uintptr_t next_raw = hazptr->next;
      assert((next_raw & IS_OWNED_BIT) == 0);
      hazard_ptr* next =
        reinterpret_cast<hazard_ptr*>(next_raw & ~IS_OWNED_BIT);
      delete hazptr;
      hazptr = next;
    }
  }

  channel(const channel&) = delete;
  channel& operator=(const channel&) = delete;
  channel(channel&&) = delete;
  channel& operator=(channel&&) = delete;
};

template <typename T, typename Config>
struct aw_push : private tmc::detail::AwaitTagNoGroupAsIs {
  channel_token<T, Config>* tok;
  tmc::channel_error result;
  T u;
  aw_push(channel_token<T, Config>* Tok, T U) : tok{Tok}, u{U} {}

  using chan_t = channel<T, Config>;
  using hazard_ptr = chan_t::hazard_ptr;
  bool await_ready() {
    hazard_ptr* hazptr = tok->get_hazard_ptr();
    bool consWaiting = false;
    result = tok->chan->push(u, hazptr, consWaiting);
    if (consWaiting) {
      if (++tok->prodLagCount == 1000) {
        tok->prodLagCount = 0;
        return false;
      }
    }
    return true;
  }

  void await_suspend(std::coroutine_handle<> Outer) {
    tmc::detail::post_checked(
      tmc::detail::this_thread::executor, std::move(Outer),
      tmc::detail::this_thread::this_task.prio, 0
    );
  }

  tmc::channel_error await_resume() { return result; }
};

/// Tokens share ownership of a channel by reference counting.
/// Access to the channel (from multiple tokens) is thread-safe,
/// but access to a single token from multiple threads is not.
/// To access the channel from multiple threads or tasks concurrently,
/// make a copy of the token for each.
template <typename T, typename Config> class channel_token {
  using chan_t = channel<T, Config>;
  using hazard_ptr = chan_t::hazard_ptr;
  std::shared_ptr<chan_t> chan;
  hazard_ptr* haz_ptr;
  size_t prodLagCount = 0;
  friend aw_push<T, Config>;
  NO_CONCURRENT_ACCESS_LOCK;

  friend channel_token make_channel<T, Config>();

  channel_token(std::shared_ptr<chan_t>&& QIn)
      : chan{std::move(QIn)}, haz_ptr{nullptr} {}

public:
  channel_token(const channel_token& Other)
      : chan(Other.chan), haz_ptr{nullptr} {}
  channel_token& operator=(const channel_token& Other) {
    chan = Other.chan;
    // TODO allow assigning to a non-empty token
    // Perhaps if user is switching between queues it would be useful to use a
    // token as a variable. This requires calling release_ownership().
    // Is that safe to do concurrently with try_reclaim_blocks?
    assert(haz_ptr == nullptr);
    // ... then should move construct/assign also be allowed?
  }

  ~channel_token() {
    if (haz_ptr != nullptr) {
      haz_ptr->release_ownership();
    }
  }

  hazard_ptr* get_hazard_ptr() {
    if (haz_ptr == nullptr) {
      haz_ptr = chan->get_hazard_ptr();
    }
    return haz_ptr;
  }

  // May return OK or CLOSED
  template <typename U> [[nodiscard]] aw_push<T, Config> push(U&& u) {
    ASSERT_NO_CONCURRENT_ACCESS();
    return aw_push<T, Config>(this, std::forward<U>(u));
  }

  // May return a value or CLOSED
  [[nodiscard]] chan_t::aw_pull pull() {
    ASSERT_NO_CONCURRENT_ACCESS();
    hazard_ptr* hazptr = get_hazard_ptr();
    return chan->pull(hazptr);
  }

  void close() {
    ASSERT_NO_CONCURRENT_ACCESS();
    chan->close();
  }

  tmc::task<void> drain() {
    ASSERT_NO_CONCURRENT_ACCESS();
    return chan->drain();
  }
};

template <typename T, typename Config>
static inline channel_token<T, Config> make_channel() {
  auto chan = new channel<T, Config>();
  return channel_token{std::shared_ptr<channel<T, Config>>(chan)};
}

} // namespace tmc
