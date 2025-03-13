// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

// Provides tmc::ticket_queue, an async MPMC queue of unbounded size.

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
template <typename T, size_t Size = 256> class queue_handle;
template <typename T, size_t Size = 256> class ticket_queue {
  // TODO allow size == 1 (empty queue that always directly transfers)
  static_assert(Size > 1);

public:
  // Round Capacity up to next power of 2
  static constexpr size_t Capacity = [](size_t x) consteval -> size_t {
    --x;
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    for (std::size_t i = 1; i < sizeof(size_t); i <<= 1) {
      x |= x >> (i << 3);
    }
    ++x;
    return x;
  }(Size);
  static constexpr size_t CapacityMask = Capacity - 1;

  struct aw_ticket_queue_waiter_base {
    T t; // by value for now, possibly zero-copy / by ref in future
    ticket_queue& queue;
    queue_error err;
    tmc::detail::type_erased_executor* continuation_executor;
    std::coroutine_handle<> continuation;
    size_t prio;
    size_t threadHint;
    template <typename U>
    aw_ticket_queue_waiter_base(U&& u, ticket_queue& q)
        : t(std::forward<U>(u)), queue(q), err{OK},
          continuation_executor{tmc::detail::this_thread::executor},
          continuation{nullptr}, prio(tmc::detail::this_thread::this_task.prio),
          threadHint(tmc::detail::this_thread::thread_index) {}

    aw_ticket_queue_waiter_base(ticket_queue& q)
        : queue(q), err{OK},
          continuation_executor{tmc::detail::this_thread::executor},
          continuation{nullptr}, prio(tmc::detail::this_thread::this_task.prio),
          threadHint(tmc::detail::this_thread::thread_index) {}
  };

  template <bool Must> class aw_ticket_queue_push;
  template <bool Must> class aw_ticket_queue_pull;

  friend queue_handle<T, Size>;

  struct element {
    std::atomic<size_t> flags;
    aw_ticket_queue_waiter_base* consumer;
    T data;
    static constexpr size_t UNPADLEN =
      sizeof(size_t) + sizeof(void*) + sizeof(T);
    static constexpr size_t PADLEN = UNPADLEN < 64 ? (64 - UNPADLEN) : 0;
    char pad[PADLEN];
    element() { flags.store(0, std::memory_order_release); }
  };

  struct data_block;

  struct data_block {
    size_t offset;
    std::atomic<data_block*> next;
    // TODO try interleaving these values
    std::array<element, Capacity> values;

    data_block(size_t Offset) : values{} {
      offset = Offset;
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
  aw_ticket_queue_pull<false> pull(hazard_ptr* hazptr) {
    return aw_ticket_queue_pull<false>(*this, hazptr);
  }

  data_block* ALL_BLOCKS = reinterpret_cast<data_block*>(1);
  data_block* WRITE_BLOCKS = reinterpret_cast<data_block*>(2);
  data_block* READ_BLOCKS = reinterpret_cast<data_block*>(3);

  enum op_name {
    ALLOC_RAW,
    FREE_RAW,
    ALLOC_LIST,
    FREE_LIST,
    LINK_TO,
    LINK_FROM,
    UNLINK_TO,
    UNLINK_FROM
  };
  struct operation {
    data_block* target;
    size_t offset;
    op_name op;
    size_t tid;
  };
  struct alignas(64) tracker {
    tmc::tiny_lock lock;
    std::vector<operation> ops;
    tracker() {}
  };

  struct tracker_scope {
    tracker* t;
    tmc::tiny_lock_guard lg;
    tracker_scope(tracker* tr) : t{tr}, lg{t->lock} {}

    inline void push_op(size_t off, op_name op, data_block* tar, size_t tid) {
      t->ops.emplace_back(tar, off, op, tid);
    }
    tracker_scope(tracker_scope const&) = delete;
    tracker_scope& operator=(tracker_scope const&) = delete;
  };

  static inline thread_local std::vector<tracker*> tracker_pool;

  tmc::tiny_lock trackers_lock;
  std::unordered_map<data_block*, tracker*> blocks_map;
  tracker_scope get_tracker(data_block* b) {
    assert(b != nullptr);
    assert(!tracker_pool.empty());
    tracker* tr = tracker_pool.back();
    trackers_lock.spin_lock();
    auto p = blocks_map.insert({b, tr});
    tracker* t;
    if (p.second) {
      trackers_lock.unlock();
      t = tr;
      tracker_pool.pop_back();
    } else {
      t = p.first->second;
      trackers_lock.unlock();
    }
    assert(t != nullptr);
    return tracker_scope(t);
  }

  std::pair<tracker_scope, tracker_scope>
  get_two_trackers(data_block* b1, data_block* b2) {
    assert(b1 != nullptr);
    assert(b2 != nullptr);
    assert(!tracker_pool.empty());
    tracker* t1;
    tracker* t2;
    tracker* tr = tracker_pool.back();
    trackers_lock.spin_lock();
    auto p = blocks_map.insert({b1, tr});
    if (p.second) {
      t1 = tr;
      tracker_pool.pop_back();
    } else {
      t1 = p.first->second;
    }

    tr = tracker_pool.back();
    p = blocks_map.insert({b2, tr});
    if (p.second) {
      trackers_lock.unlock();
      t2 = tr;
      tracker_pool.pop_back();
    } else {
      t2 = p.first->second;
      trackers_lock.unlock();
    }
    assert(t1 != nullptr);
    assert(t2 != nullptr);
    return std::make_pair(t1, t2);
  }

  // std::vector<data_block*> freelist;
  // tmc::tiny_lock freelist_lock;

  std::atomic<size_t> closed;
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

  ticket_queue()
      : blocks_map{1000}, closed{0}, write_closed_at{TMC_ALL_ONES},
        read_closed_at{TMC_ALL_ONES} {
    // freelist.reserve(1024);
    auto block = new data_block(0);
    all_blocks = block;
    blocks_tail = block;
    read_offset = 0;
    write_offset = 0;
    hazard_ptr_list = new hazard_ptr;
    hazard_ptr_list->next = 0;
  }

  static_assert(std::atomic<data_block*>::is_always_lock_free);

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
    size_t minProtected = ProtIdx & ~CapacityMask; // round down to block index

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
        data_block* b = unlinked[i];
        for (size_t i = 0; i < Capacity; ++i) {
          b->values[i].flags.store(0, std::memory_order_relaxed);
        }
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
        size_t boff = tailBlock->offset + Capacity;
        for (; i < unlinkedCount - 1; ++i) {
          data_block* b = unlinked[i];
          b->offset = boff;
          b->next.store(unlinked[i + 1], std::memory_order_release);
          boff += Capacity;
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
    while (offset + Capacity <= idx) {
      data_block* next = block->next.load(std::memory_order_acquire);
      if (next == nullptr) {
        data_block* newBlock = new data_block(offset + Capacity);
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
      offset += Capacity;
      assert(block->offset == offset);
    }
    size_t boff = block->offset;
    assert(idx >= boff && idx < boff + Capacity);
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
    if ((idx & CapacityMask) == 0 && blocks_lock.try_lock()) {
      size_t protIdx = read_offset.load(std::memory_order_relaxed);
      read_offset.compare_exchange_strong(
        protIdx, protIdx, std::memory_order_seq_cst
      );
      try_free_block(hazptr, protIdx);
      blocks_lock.unlock();
    }
    element* elem = &block->values[idx & CapacityMask];
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
        element* elem = &block->values[idx & CapacityMask];
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
    if ((idx & CapacityMask) == 0 && blocks_lock.try_lock()) {
      size_t protIdx = write_offset.load(std::memory_order_relaxed);
      write_offset.compare_exchange_strong(
        protIdx, protIdx, std::memory_order_seq_cst
      );
      try_free_block(hazptr, protIdx);
      blocks_lock.unlock();
    }
    element* elem = &block->values[idx & CapacityMask];
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

  template <bool Must>
  class aw_ticket_queue_pull : protected aw_ticket_queue_waiter_base,
                               private tmc::detail::AwaitTagNoGroupAsIs {
    using aw_ticket_queue_waiter_base::continuation;
    using aw_ticket_queue_waiter_base::continuation_executor;
    using aw_ticket_queue_waiter_base::err;
    using aw_ticket_queue_waiter_base::queue;
    using aw_ticket_queue_waiter_base::t;
    hazard_ptr* hazptr;
    element* elem;

    aw_ticket_queue_pull(ticket_queue& q, hazard_ptr* haz)
        : aw_ticket_queue_waiter_base(q), hazptr{haz} {}

    // May return a value or CLOSED
    friend aw_ticket_queue_pull ticket_queue::pull(hazard_ptr*);

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
    std::variant<T, queue_error> await_resume()
      requires(!Must)
    {
      hazptr->active_offset.store(TMC_ALL_ONES, std::memory_order_release);
      if (err == OK) {
        return std::move(t);
      } else {
        return err;
      }
    }
    T&& await_resume()
      requires(Must)
    {
      return std::move(t);
    }
  };

public:
  // May return OK, FULL, or CLOSED
  queue_error try_push();
  // May return a value, or EMPTY or CLOSED
  std::variant<T, queue_error> try_pull();
  // May return a value, or EMPTY OR CLOSED
  std::variant<T, queue_error> try_pop();

  // // May return a value or CLOSED
  // aw_ticket_queue_pop<false> pop() {}

  // Returns void. If the queue is closed, std::terminate will be called.
  aw_ticket_queue_push<true> must_push();
  // Returns a value. If the queue is closed, std::terminate will be called.
  aw_ticket_queue_pull<true> must_pull();
  // // Returns a value. If the queue is closed, std::terminate will be
  // called. aw_ticket_queue_pop<true> must_pop() {}

  // TODO separate close() and drain() operations
  // TODO separate drain() (async) and drain_sync()
  // will need a single location on the queue to store the drain async
  // continuation all consumers participate in checking this to awaken

  // All currently waiting producers will return CLOSED.
  // Consumers will continue to read data until the queue is drained,
  // at which point all consumers will return CLOSED.
  void close() {
    blocks_lock.spin_lock();
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

    blocks_lock.unlock();
  }

  // Waits for the queue to drain.
  void drain_sync() {
    blocks_lock.spin_lock();
    size_t woff = write_closed_at.load(std::memory_order_seq_cst);
    size_t roff = read_offset.load(std::memory_order_seq_cst);

    data_block* block = all_blocks.load(std::memory_order_seq_cst);
    size_t i = block->offset;
    // drained_to.load(std::memory_order_acquire);

    // Wait for the queue to drain (elements prior to write_closed_at write
    // index)
    while (true) {
      while (i < roff && i < woff) {
        size_t idx = i & CapacityMask;
        auto v = &block->values[idx];
        // Data is present at these elements; wait for the queue to drain
        while (v->flags.load(std::memory_order_acquire) != 3) {
          TMC_CPU_PAUSE();
        }

        ++i;
        if ((i & CapacityMask) == 0) {
          data_block* next = block->next.load(std::memory_order_acquire);
          while (next == nullptr) {
            // A block is being constructed. Wait for it.
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
      size_t idx = i & CapacityMask;
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
      if ((i & CapacityMask) == 0) {
        data_block* next = block->next.load(std::memory_order_acquire);
        while (next == nullptr) {
          // A block is being constructed. Wait for it.
          TMC_CPU_PAUSE();
          next = block->next.load(std::memory_order_acquire);
        }
        block = next;
      }
    }
    blocks_lock.unlock();
  }

  void dump_ops(tracker* track) {
    for (size_t i = 0; i < track->ops.size(); ++i) {
      operation& op = track->ops[i];
      const char* opName;
      switch (op.op) {
      case ALLOC_RAW:
        opName = "ALLOC_RAW";
        break;
      case ALLOC_LIST:
        opName = "ALLOC_LIST";
        break;
      case FREE_RAW:
        opName = "FREE_RAW";
        break;
      case FREE_LIST:
        opName = "FREE_LIST";
        break;
      case LINK_TO:
        opName = "LINK_TO";
        break;
      case LINK_FROM:
        opName = "LINK_FROM";
        break;
      case UNLINK_TO:
        opName = "UNLINK_TO";
        break;
      case UNLINK_FROM:
        opName = "UNLINK_FROM";
        break;
      }
      std::printf(
        "%-2zu | %-12s: %p %zu\n", op.tid, opName, op.target, op.offset
      );
    }
  }

  ~ticket_queue() {
    // drain_sync();
    blocks_lock.spin_lock();
    data_block* block = all_blocks.load(std::memory_order_acquire);
    size_t idx = block->offset;
    // std::unordered_set<data_block*> freed_block_set;
    while (block != nullptr) {
      data_block* next = block->next.load(std::memory_order_acquire);
      if (idx != block->offset) {
        std::printf("idx %zu != offset %zu\n", idx, block->offset);
      }
      delete block;
      // freed_block_set.emplace(block);
      block = next;
      idx = idx + Capacity;
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
    // freelist_lock.spin_lock();
    // for (size_t i = 0; i < freelist.size(); ++i) {
    //   block = freelist[i];
    //   delete block;
    //   freed_block_set.emplace(block);
    // }
    // std::unordered_set<data_block*> leaked_block_set;
    // for (auto it = blocks_map.begin(); it != blocks_map.end(); ++it) {
    //   if (!freed_block_set.contains(it->first)) {
    //     leaked_block_set.emplace(it->first);
    //   }
    // }
    // if (!leaked_block_set.empty()) {
    //   for (auto it = leaked_block_set.begin(); it !=
    //   leaked_block_set.end();
    //        ++it) {
    //     data_block* leak = *it;
    //     tracker* track = blocks_map[leak];
    //     // tracker* track = it->second;
    //     std::printf(
    //       "leaked: %p  %zu\n", leak,
    //       leak->info.load(std::memory_order_acquire).offset
    //     );

    //     dump_ops(track);

    //     operation lastOp = track->ops.back();
    //     if (lastOp.op != LINK_FROM) {
    //       std::printf("UNKNOWN FINAL OPERATION\n");
    //       std::terminate();
    //     }
    //     if (!blocks_map.contains(lastOp.target)) {
    //       std::printf("UNKNOWN FINAL TARGET %p\n", lastOp.target);
    //       std::terminate();
    //     }

    //     std::printf("\nFINAL LINK_FROM NEIGHBOR LOG (%p)\n",
    //     lastOp.target); dump_ops(blocks_map[lastOp.target]);
    //     std::fflush(stdout);

    //     std::terminate();
    //   }
    // }
    // for (auto it = blocks_map.begin(); it != blocks_map.end(); ++it) {
    //   tracker* tr = it->second;
    //   delete tr;
    // }
  }

  ticket_queue(const ticket_queue&) = delete;
  ticket_queue& operator=(const ticket_queue&) = delete;
  // TODO implement move constructor
};

/// Handles share ownership of a queue by reference counting.
/// Access to the queue (from multiple handles) is thread-safe,
/// but access to a single handle from multiple threads is not.
/// To access the queue from multiple threads or tasks concurrently,
/// make a copy of the handle for each.
template <typename T, size_t Size> class queue_handle {
  using queue_t = ticket_queue<T, Size>;
  using hazard_ptr = queue_t::hazard_ptr;
  std::shared_ptr<queue_t> q;
  hazard_ptr* haz_ptr;
  NO_CONCURRENT_ACCESS_LOCK;
  friend queue_t;
  queue_handle(std::shared_ptr<queue_t>&& QIn)
      : q{std::move(QIn)}, haz_ptr{nullptr} {}

public:
  void print_tids() {}
  static inline queue_handle make() {
    return queue_handle{std::make_shared<queue_t>()};
  }
  queue_handle(const queue_handle& Other) : q(Other.q), haz_ptr{nullptr} {}
  queue_handle& operator=(const queue_handle& Other) {
    q = Other.q;
    assert(haz_ptr == nullptr);
  }

  ~queue_handle() {
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
  queue_t::template aw_ticket_queue_pull<false> pull() {
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
  // TODO make queue call close() and drain_sync() in its destructor
};

} // namespace tmc
