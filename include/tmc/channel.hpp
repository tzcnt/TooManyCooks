// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

// Provides tmc::channel, an async MPMC queue of unbounded size.

// Producers may suspend when calling co_await push() if the queue is full.
// Consumers retrieve values in FIFO order with pull().
// Consumers retrieve values in LIFO order with pop();

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
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

// TODO mask with highest bit set to 0 when comparing indexes

namespace this_thread {
inline thread_local size_t thread_slot = TMC_ALL_ONES;
} // namespace this_thread

namespace tmc {

enum queue_error { OK = 0, CLOSED = 1, EMPTY = 2, FULL = 3, SUSPENDED = 4 };
template <typename T, size_t BlockSize> class channel_token;

/// Creates a new channel and returns an access token to it.
/// Tokens share ownership of a channel by reference counting.
/// Access to the channel (from multiple tokens) is thread-safe,
/// but access to a single token from multiple threads is not.
/// To access the channel from multiple threads or tasks concurrently,
/// make a copy of the token for each.
template <typename T, size_t BlockSize = 4096>
static inline channel_token<T, BlockSize> make_channel();

template <typename T, size_t BlockSize> class channel {
  static_assert(BlockSize > 1);
  static_assert(BlockSize % 2 == 0, "BlockSize must be a power of 2");

  static constexpr size_t BlockSizeMask = BlockSize - 1;

  friend channel_token<T, BlockSize>;
  template <typename Tc, size_t Bc> friend channel_token<Tc, Bc> make_channel();

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

  static inline constexpr size_t IS_OWNED_BIT = TMC_ONE_BIT << 60;
  class alignas(64) hazard_ptr {
  public:
    std::atomic<uintptr_t> next;
    std::atomic<size_t> active_offset;
    std::atomic<data_block*> write_block;
    std::atomic<data_block*> read_block;

    hazard_ptr() : active_offset{TMC_ALL_ONES} {}

    bool take_ownership() {
      return (next.fetch_or(IS_OWNED_BIT) & IS_OWNED_BIT) == 0;
    }
    void release_ownership() {
      [[maybe_unused]] bool ok =
        (next.fetch_and(~IS_OWNED_BIT) & IS_OWNED_BIT) != 0;
      assert(ok);
    }
  };

  // Gets a hazard pointer from the list, and takes ownership of it.
  hazard_ptr* get_hazard_ptr() {
    // Mutex with block reclamation, which reads from the hazard pointer list
    tmc::tiny_lock_guard lg{blocks_lock};

    hazard_ptr* ptr = hazard_ptr_list;
    uintptr_t next_raw = ptr->next.load(std::memory_order_acquire);
    uintptr_t is_owned = next_raw & IS_OWNED_BIT;
    while (true) {
      if ((is_owned == 0) && ptr->take_ownership()) {
        break;
        return ptr;
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
    ptr->read_block = all_blocks.load(std::memory_order_relaxed);
    ptr->write_block = all_blocks.load(std::memory_order_relaxed);
    return ptr;
  }

  // May return a value or CLOSED
  aw_pull pull(hazard_ptr* hazptr) { return aw_pull(*this, hazptr); }

  static_assert(std::atomic<size_t>::is_always_lock_free);
  static_assert(std::atomic<data_block*>::is_always_lock_free);

  std::atomic<size_t> closed; // bit 0 = write closed. bit 1 = read closed
  std::atomic<size_t> write_closed_at;
  std::atomic<size_t> read_closed_at;
  std::atomic<data_block*> all_blocks;
  std::atomic<data_block*> blocks_tail;
  tmc::tiny_lock blocks_lock;
  char pad0[64 - 1 * (sizeof(void*))];
  std::atomic<size_t> write_offset;
  char pad1[64 - 1 * (sizeof(void*))];
  std::atomic<size_t> read_offset;
  char pad2[64 - 1 * (sizeof(void*))];
  hazard_ptr* hazard_ptr_list;

  channel()
      : closed{0}, write_closed_at{TMC_ALL_ONES}, read_closed_at{TMC_ALL_ONES} {
    // freelist.reserve(1024);
    auto block = new data_block(0);
    all_blocks = block;
    blocks_tail = block;
    read_offset = 0;
    write_offset = 0;
    hazard_ptr_list = new hazard_ptr;
    hazard_ptr_list->next = 0;
  }

  // Load src and move it into dst if src < dst.
  static inline void keep_min(size_t& dst, std::atomic<size_t> const& src) {
    size_t val = src.load(std::memory_order_acquire);
    if (val < dst) {
      dst = val;
    }
  }

  // Move src into dst if src < dst.
  static inline void keep_min(size_t& dst, size_t src) {
    if (src < dst) {
      dst = src;
    }
  }

  // Advances DstBlock to be equal to NewHead. Possibly reduces MinProtect if
  // DstBlock was already updated by its owning thread.
  void try_advance_block(
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

  data_block*
  unlink_blocks(hazard_ptr* HazPtr, data_block* OldHead, size_t ProtIdx) {
    // If this hazptr protects write_block, ProtIdx contains read_offset.
    // If this hazptr protects read_block, ProtIdx contains write_offset.
    // This ensures that in cases where only a single hazptr is active (which is
    // a producer or a consumer, but not both), the opposite end is also
    // protected.
    size_t minProtected = ProtIdx & ~BlockSizeMask; // round down to block index

    // Find the lowest/oldest offset that is protected by ProtIdx or any hazptr.
    hazard_ptr* curr = hazard_ptr_list;
    while (minProtected > OldHead->offset && curr != nullptr) {
      uintptr_t next_raw = curr->next.load(std::memory_order_acquire);
      hazard_ptr* next =
        reinterpret_cast<hazard_ptr*>(next_raw & ~IS_OWNED_BIT);
      uintptr_t is_owned = next_raw & IS_OWNED_BIT;
      if (is_owned) {
        keep_min(minProtected, curr->active_offset);
      }
      curr = next;
    }

    // If head block is protected, nothing can be reclaimed.
    if (minProtected == OldHead->offset) {
      return OldHead;
    }

    // Find the block associated with this offset.
    data_block* newHead = OldHead;
    while (newHead->offset < minProtected) {
      newHead = newHead->next;
    }

    // Then update all hazptrs to be at this block or later.
    curr = hazard_ptr_list;
    while (minProtected > OldHead->offset && curr != nullptr) {
      uintptr_t next_raw = curr->next.load(std::memory_order_acquire);
      hazard_ptr* next =
        reinterpret_cast<hazard_ptr*>(next_raw & ~IS_OWNED_BIT);
      uintptr_t is_owned = next_raw & IS_OWNED_BIT;
      if (is_owned) {
        try_advance_block(
          curr->write_block, minProtected, newHead, curr->active_offset
        );
        try_advance_block(
          curr->read_block, minProtected, newHead, curr->active_offset
        );
      }

      curr = next;
    }

    // minProtected may have been reduced by the double-check in
    // try_advance_block. If so, reduce newHead as well.
    if (minProtected < newHead->offset) {
      newHead = OldHead;
      while (newHead->offset < minProtected) {
        newHead = newHead->next;
      }
    }

    size_t roff = read_offset.load(std::memory_order_acquire);
    assert(newHead->offset <= roff);
    assert(newHead->offset <= write_offset.load(std::memory_order_acquire));
    return newHead;
  }

  // TODO add bool template argument to push this work to a background task
  // Access to this function (the blocks pointer specifically) must be
  // externally synchronized (via blocks_lock).
  void try_free_block(hazard_ptr* hazptr, size_t ProtIdx) {
    auto tid = this_thread::thread_slot;
    data_block* block = all_blocks.load(std::memory_order_acquire);
    data_block* newHead = unlink_blocks(hazptr, block, ProtIdx);
    if (newHead == block) {
      assert(newHead->offset == block->offset);
      return;
    }
    all_blocks.store(newHead, std::memory_order_release);
    // data_block* toDelete = block;
    //  while (toDelete != newHead) {
    //    data_block* next = toDelete->next.load(std::memory_order_relaxed);
    //    delete toDelete;
    //    toDelete = next;
    //  }
    while (true) {
      if (block == newHead) {
        assert(newHead->offset == block->offset);
        break;
      }
      std::array<data_block*, 4> unlinked;
      size_t unlinkedCount = 0;
      for (; unlinkedCount < unlinked.size(); ++unlinkedCount) {
        if (block == newHead) {
          break;
        }
        unlinked[unlinkedCount] = block;
        block = block->next.load(std::memory_order_acquire);
      }
      assert(unlinkedCount != 0);

      for (size_t i = 0; i < unlinkedCount; ++i) {
        unlinked[i]->reset_values();
      }

      data_block* tailBlock = blocks_tail.load(std::memory_order_acquire);
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
        next, unlinked[0], std::memory_order_acq_rel, std::memory_order_acquire
      ));

      blocks_tail.store(unlinked[unlinkedCount - 1]);
    }
  }

  data_block* find_block(data_block* block, size_t idx) {
    size_t offset = block->offset;
    // Find or allocate the associated block
    while (offset + BlockSize <= idx) {
      data_block* next = block->next.load(std::memory_order_acquire);
      if (next == nullptr) {
        data_block* newBlock = new data_block(offset + BlockSize);
        if (block->next.compare_exchange_strong(
              next, newBlock, std::memory_order_acq_rel,
              std::memory_order_acquire
            )) {
          next = newBlock;
        } else {
          delete newBlock;
        }
      }
      block = next;
      offset += BlockSize;
      assert(block->offset == offset);
    }
    size_t boff = block->offset;
    assert(idx >= boff && idx < boff + BlockSize);
    return block;
  }

  element* get_write_ticket(hazard_ptr* hazptr) {
    data_block* block = hazptr->write_block.load(std::memory_order_acquire);
    hazptr->active_offset.store(block->offset, std::memory_order_release);
    // seq_cst is needed here to create a StoreLoad barrier between setting
    // hazptr and reloading the block
    size_t idx = write_offset.fetch_add(1, std::memory_order_seq_cst);
    // Reload block in case it was modified after setting hazptr offset
    block = hazptr->write_block.load(std::memory_order_acquire);
    assert(idx >= block->offset);
    // close() will set `closed` before incrementing offset.
    // Thus we are guaranteed to see it if we acquire offset first.
    if (closed.load(std::memory_order_acquire)) {
      return nullptr;
    }
    block = find_block(block, idx);
    // Update last known block.
    // Note that if hazptr was to an older block, that block will still be
    // protected. This prevents a queue consisting of a single block from trying
    // to unlink/link that block to itself.
    hazptr->write_block.store(block, std::memory_order_release);
    element* elem = &block->values[idx & BlockSizeMask];
    return elem;
  }

  element* get_read_ticket(hazard_ptr* hazptr) {
    data_block* block = hazptr->read_block.load(std::memory_order_acquire);
    hazptr->active_offset.store(block->offset, std::memory_order_release);
    // seq_cst is needed here to create a StoreLoad barrier between setting
    // hazptr and reloading the block
    size_t idx = read_offset.fetch_add(1, std::memory_order_seq_cst);
    // Reload block in case it was modified after setting hazptr offset
    block = hazptr->read_block.load(std::memory_order_acquire);
    assert(idx >= block->offset);
    // close() will set `closed` before incrementing offset.
    // Thus we are guaranteed to see it if we acquire offset first.
    if (closed.load(std::memory_order_acquire)) {
      // If closed, continue draining until the queue is empty
      // As indicated by the write_closed_at index
      if (idx >= write_closed_at.load(std::memory_order_relaxed)) {
        // After queue is empty, we still need to mark each element as
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
    // protected. This prevents a queue consisting of a single block from trying
    // to unlink/link that block to itself.
    hazptr->read_block.store(block, std::memory_order_release);
    // Try to reclaim old blocks. Checking for index 1 ensures that at least
    // this thread's hazptr will already be advanced to the new block.
    if ((idx & BlockSizeMask) == 1 && blocks_lock.try_lock()) {
      // TODO just do a seq_cst load instead?
      size_t protIdx = write_offset.load(std::memory_order_relaxed);
      write_offset.compare_exchange_strong(
        protIdx, protIdx, std::memory_order_seq_cst
      );
      try_free_block(hazptr, protIdx);
      blocks_lock.unlock();
    }
    element* elem = &block->values[idx & BlockSizeMask];
    return elem;
  }

  template <typename U> queue_error write_element(element* elem, U&& u) {
    if (elem == nullptr) {
      return CLOSED;
    }
    size_t flags = elem->flags.load(std::memory_order_acquire);
    assert((flags & 1) == 0);

    // Check if consumer is waiting
    if (flags & 2) {
      // There was a consumer waiting for this data
      auto cons = elem->consumer;
      // Still need to store so block can be freed
      elem->flags.store(3, std::memory_order_release);
      cons->t = (1 << 31) | u;
      tmc::detail::post_checked(
        cons->continuation_executor, std::move(cons->continuation), cons->prio,
        cons->threadHint
      );
      return OK;
    }

    // No consumer waiting, store the data
    elem->data = std::forward<U>(u);

    // Finalize transaction
    size_t expected = 0;
    if (!elem->flags.compare_exchange_strong(
          expected, 1, std::memory_order_acq_rel, std::memory_order_acquire
        )) {
      // Consumer started waiting for this data during our RMW cycle
      assert(expected == 2);
      auto cons = elem->consumer;
      cons->t = (1 << 31) | elem->data;
      tmc::detail::post_checked(
        cons->continuation_executor, std::move(cons->continuation), cons->prio,
        cons->threadHint
      );
      elem->flags.store(3, std::memory_order_release);
    }
    return OK;
  }

  template <typename U> queue_error push(U&& u, hazard_ptr* hazptr) {
    // Get write ticket and associated block, protected by hazptr.
    element* elem = get_write_ticket(hazptr);

    // Store the data / wake any waiting consumers
    queue_error err = write_element(elem, std::forward<U>(u));

    // Then release the hazard pointer
    hazptr->active_offset.store(TMC_ALL_ONES, std::memory_order_release);

    return err;
  }

public:
  class aw_pull : private tmc::detail::AwaitTagNoGroupAsIs {
    T t; // by value for now, possibly zero-copy / by ref in future
    channel& queue;
    queue_error err;
    tmc::detail::type_erased_executor* continuation_executor;
    std::coroutine_handle<> continuation;
    size_t prio;
    size_t threadHint;
    hazard_ptr* hazptr;
    element* elem;

    aw_pull(channel& q, hazard_ptr* haz)
        : queue(q), err{OK},
          continuation_executor{tmc::detail::this_thread::executor},
          continuation{nullptr}, prio(tmc::detail::this_thread::this_task.prio),
          threadHint(tmc::detail::this_thread::thread_index), hazptr{haz} {}

    // May return a value or CLOSED
    friend channel;

  public:
    bool await_ready() {
      // Get read ticket and associated block, protected by hazptr.
      elem = queue.get_read_ticket(hazptr);
      if (elem == nullptr) {
        err = CLOSED;
        hazptr->active_offset.store(TMC_ALL_ONES, std::memory_order_release);
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
        hazptr->active_offset.store(TMC_ALL_ONES, std::memory_order_release);
        return true;
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
        hazptr->active_offset.store(TMC_ALL_ONES, std::memory_order_release);
        return false;
      }
      return true;
    }
    std::variant<T, queue_error> await_resume() {
      hazptr->active_offset.store(TMC_ALL_ONES, std::memory_order_release);
      if (err == OK) {
        return std::move(t);
      } else {
        return err;
      }
    }
  };

private:
  // May return OK, FULL, or CLOSED
  queue_error try_push();
  // May return a value, or EMPTY or CLOSED
  std::variant<T, queue_error> try_pull();

  // TODO separate drain() (async) and drain_sync()
  // will need a single location on the queue to store the drain async
  // continuation all consumers participate in checking this to awaken

  // All currently waiting producers will return CLOSED.
  // Consumers will continue to read data until the queue is drained,
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

  // If the queue is not already closed, it will be closed.
  // Then, waits for the queue to drain.
  // After all data has been consumed from the queue,
  // all consumers will return CLOSED.
  void drain_sync() {
    close(); // close() is idempotent and a precondition to call this.
    blocks_lock.spin_lock();
    size_t woff = write_closed_at.load(std::memory_order_seq_cst);
    size_t roff = read_offset.load(std::memory_order_seq_cst);

    // Fast-path reclaim blocks up to the earlier of read or write index
    size_t protIdx = woff;
    keep_min(protIdx, roff);
    try_free_block(hazard_ptr_list, protIdx);

    data_block* block = all_blocks.load(std::memory_order_seq_cst);
    size_t i = block->offset;

    // Slow-path wait for the queue to drain.
    // Check each element prior to write_closed_at write index.
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
        roff = read_offset.load(std::memory_order_seq_cst);
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
        cons->err = CLOSED;
        tmc::detail::post_checked(
          cons->continuation_executor, std::move(cons->continuation),
          cons->prio, cons->threadHint
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
    data_block* block = all_blocks.load(std::memory_order_acquire);
    size_t idx = block->offset;
    while (block != nullptr) {
      data_block* next = block->next.load(std::memory_order_acquire);
      delete block;
      block = next;
      idx = idx + BlockSize;
    }
    hazard_ptr* hazptr = hazard_ptr_list;
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

/// Tokens share ownership of a channel by reference counting.
/// Access to the channel (from multiple tokens) is thread-safe,
/// but access to a single token from multiple threads is not.
/// To access the channel from multiple threads or tasks concurrently,
/// make a copy of the token for each.
template <typename T, size_t BlockSize> class channel_token {
  using chan_t = channel<T, BlockSize>;
  using hazard_ptr = chan_t::hazard_ptr;
  std::shared_ptr<chan_t> q;
  hazard_ptr* haz_ptr;
  NO_CONCURRENT_ACCESS_LOCK;

  friend channel_token make_channel<T, BlockSize>();

  channel_token(std::shared_ptr<chan_t>&& QIn)
      : q{std::move(QIn)}, haz_ptr{nullptr} {}

public:
  channel_token(const channel_token& Other) : q(Other.q), haz_ptr{nullptr} {}
  channel_token& operator=(const channel_token& Other) {
    q = Other.q;
    assert(haz_ptr == nullptr);
  }

  ~channel_token() {
    if (haz_ptr != nullptr) {
      haz_ptr->release_ownership();
    }
  }

  hazard_ptr* get_hazard_ptr() {
    if (haz_ptr == nullptr) {
      haz_ptr = q->get_hazard_ptr();
    }
    return haz_ptr;
  }

  template <typename U> queue_error push(U&& u) {
    ASSERT_NO_CONCURRENT_ACCESS();
    hazard_ptr* hazptr = get_hazard_ptr();
    return q->template push<U>(std::forward<U>(u), hazptr);
  }

  // May return a value or CLOSED
  chan_t::aw_pull pull() {
    ASSERT_NO_CONCURRENT_ACCESS();
    hazard_ptr* hazptr = get_hazard_ptr();
    return q->pull(hazptr);
  }

  void close() {
    ASSERT_NO_CONCURRENT_ACCESS();
    q->close();
  }

  void drain_sync() {
    ASSERT_NO_CONCURRENT_ACCESS();
    q->drain_sync();
  }
};

template <typename T, size_t BlockSize>
static inline channel_token<T, BlockSize> make_channel() {
  auto chan = new channel<T, BlockSize>();
  return channel_token{std::shared_ptr<channel<T, BlockSize>>(chan)};
}

} // namespace tmc
