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
#include <variant>

// TODO mask with highest bit set to 0 when comparing indexes

namespace tmc {
enum queue_error { OK = 0, CLOSED = 1, EMPTY = 2, FULL = 3 };

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
    template <typename U>
    aw_ticket_queue_waiter_base(U&& u, ticket_queue& q)
        : t(std::forward<U>(u)), queue(q), err{OK},
          continuation_executor{tmc::detail::this_thread::executor},
          continuation{nullptr},
          prio(tmc::detail::this_thread::this_task.prio) {}

    aw_ticket_queue_waiter_base(ticket_queue& q)
        : queue(q), err{OK},
          continuation_executor{tmc::detail::this_thread::executor},
          continuation{nullptr},
          prio(tmc::detail::this_thread::this_task.prio) {}
  };

  template <bool Must> class aw_ticket_queue_push;
  template <bool Must> class aw_ticket_queue_pull;

  // May return a value or CLOSED
  aw_ticket_queue_pull<false> pull() {
    return aw_ticket_queue_pull<false>(*this);
  }

  struct element {
    std::atomic<size_t> flags;
    aw_ticket_queue_waiter_base* consumer;
    T data;
    element() : flags{0} {}
  };

  struct data_block {
    std::atomic<data_block*> next;
    size_t offset; // amount to subtract from raw index when calculating index
                   // into this block
    std::array<element, Capacity> values;

    data_block(size_t offset) : next{nullptr}, offset(offset), values{} {}
  };

  std::atomic<bool> closed;
  std::atomic<size_t> closed_at;
  std::atomic<data_block*> blocks;
  tmc::tiny_lock blocks_lock;
  char pad0[64 - 3 * (sizeof(size_t))];
  // If write_offset == read_offset, queue is empty
  // If write_offset == read_offset - 1, queue is full
  // 1 element of capacity is wasted to make this work
  std::atomic<size_t> write_offset;
  std::atomic<data_block*> write_block;
  char pad1[64 - 2 * (sizeof(size_t))];
  std::atomic<size_t> read_offset;
  std::atomic<data_block*> read_block;
  char pad2[64 - 2 * (sizeof(size_t))];

  ticket_queue() : closed{false}, closed_at{TMC_ALL_ONES} {
    auto block = new data_block(0);
    blocks = block;
    write_block = block;
    read_block = block;
    // Init in this order to create a release-sequence with offsets
    write_offset = 0;
    read_offset = 0;
  }

  // TODO add bool template argument to push this work to a background task
  // Access to this function (the blocks pointer specifically) must be
  // externally synchronized (via blocks_lock).
  template <bool Wait> void try_free_block() {
    // Both writer&reader must have moved on to the next block
    // TODO think about the order / kind of these atomic operations
    // size_t woff = write_offset.load(std::memory_order_relaxed);
    // data_block* wblock = write_block.load(std::memory_order_acquire);
    // if (woff - wblock->offset < Capacity) {
    //   return;
    // }
    // size_t roff = read_offset.load(std::memory_order_relaxed);
    // data_block* rblock = read_block.load(std::memory_order_acquire);
    // if (roff - rblock->offset < Capacity) {
    //   return;
    // }

    // size_t up_to = woff;
    // if (roff < woff) {
    //   up_to = roff;
    // }

    // TODO think about the order / kind of these atomic loads
    data_block* block = blocks.load(std::memory_order_acquire);
    while (true) {
      data_block* wblock = write_block.load(std::memory_order_acquire);
      data_block* rblock = read_block.load(std::memory_order_acquire);
      if (block == wblock || block == rblock) {
        // Cannot free currently active block
        return;
      }

      for (size_t idx = 0; idx < Capacity; ++idx) {
        auto v = &block->values[idx];
        if constexpr (Wait) {
          // Data is present at these elements; wait for the queue to drain
          while (v->flags.load(std::memory_order_acquire) != 3) {
            TMC_CPU_PAUSE();
          }
        } else {
          // All elements in this block must be done before it can be deleted.
          size_t flags = v->flags.load(std::memory_order_acquire);
          if (flags != 3) {
            return;
          }
        }
        ++idx;
      }

      data_block* next = block->next.load(std::memory_order_acquire);
      assert(next != nullptr);
      // TODO update read_offset / write_offset
      // How to do this atomically?
      // One way - separate read_block / write_block again
      // And use CMPXCHG16B (Double Width CAS)
      // This is not supported on 32 bit
      // Another way - move the indexes into the block itself
      data_block* expected = block;
      [[maybe_unused]] bool ok = blocks.compare_exchange_strong(
        expected, next, std::memory_order_acq_rel, std::memory_order_acquire
      );
      assert(ok);

      delete block;
      block = next;
    }
  }

  // Modified idx and block_head by reference.
  // Returns the currently active block that can be accessed via idx.
  data_block*
  get_active_block(size_t& idx, std::atomic<data_block*>& block_head) {
    data_block* block = block_head.load(std::memory_order_acquire);
    size_t oldVal = block->offset;

    if (idx < block->offset) {
      // An uncommon race condition - the write/read block has been passed
      // already. We can still find the right block starting from the blocks
      // base
      block = blocks.load(std::memory_order_acquire);
      assert(block->offset <= idx);
      for (size_t i = block->offset + Capacity; i <= idx; i += Capacity) {
        block = block->next;
      }
      assert(block->offset <= idx);
      assert(idx < block->offset + Capacity);
    }
    data_block* oldHead = block;
    idx = idx - block->offset;
    // If we ran out of space in the current block, allocate a new one
    while (idx >= Capacity) {
      idx -= Capacity;
      // TODO this just appends new blocks forever
      // Try storing the base offset in the block itself
      // And then atomic swapping block head instead of
      // `idx -= Capacity`;
      // Note that read and write are both bumping the same next pointer here
      // Also need to scan the entire block to ensure all elements have been
      // consumed before swapping the block
      if (block->next != nullptr) {
        block = block->next.load(std::memory_order_acquire);
        continue;
      }
      auto newBlock = new data_block(block->offset + Capacity);
      data_block* expected = nullptr;
      if (block->next.compare_exchange_strong(
            expected, newBlock, std::memory_order_acq_rel,
            std::memory_order_acquire
          )) {
        block = newBlock;
        // TODO this is vulnerable to ABA
        if (block_head.compare_exchange_strong(
              oldHead, block, std::memory_order_acq_rel,
              std::memory_order_acquire
            )) {
          // test for ABA race condition
          assert(oldHead->offset == oldVal);
          // if (blocks_lock.try_lock()) {
          //   try_free_block<false>();
          //   blocks_lock.unlock();
          // }
        }
      } else {
        delete newBlock;
        block = expected;
      }
    }
    return block;
  }

  template <typename U> queue_error push(U&& u) {
    // Get write ticket and find the associated position
    size_t idx = write_offset.fetch_add(1, std::memory_order_acq_rel);
    // close() will set `closed` before incrementing write_offset
    // thus we are guaranteed to see it if we acquire write_offset first
    if (closed.load(std::memory_order_relaxed)) {
      return CLOSED;
    }

    size_t origIdx = idx;
    data_block* block = get_active_block(idx, write_block);

    element* elem = &block->values[idx];
    size_t flags = elem->flags.load(std::memory_order_relaxed);
    assert((flags & 1) == 0);

    // Check if consumer is waiting
    if (flags & 2) {
      // There was a consumer waiting for this data
      auto cons = elem->consumer;
      // Still need to store so block can be freed
      elem->flags.store(3, std::memory_order_release);
      cons->t = std::forward<U>(u);
      tmc::detail::post_checked(
        cons->continuation_executor, std::move(cons->continuation), cons->prio
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
      cons->t = std::move(elem->data);
      tmc::detail::post_checked(
        cons->continuation_executor, std::move(cons->continuation), cons->prio
      );
      elem->flags.store(3, std::memory_order_release);
    }
    return OK;
  }

  template <bool Must>
  class aw_ticket_queue_pull : protected aw_ticket_queue_waiter_base,
                               private tmc::detail::AwaitTagNoGroupAsIs {
    using aw_ticket_queue_waiter_base::continuation;
    using aw_ticket_queue_waiter_base::continuation_executor;
    using aw_ticket_queue_waiter_base::err;
    using aw_ticket_queue_waiter_base::queue;
    using aw_ticket_queue_waiter_base::t;
    element* elem;

    aw_ticket_queue_pull(ticket_queue& q) : aw_ticket_queue_waiter_base(q) {}

    // May return a value or CLOSED
    friend aw_ticket_queue_pull ticket_queue::pull();

  public:
    bool await_ready() {
      // Get read ticket and find the associated position
      size_t roff = queue.read_offset.fetch_add(1, std::memory_order_acq_rel);
      size_t idx = roff;

      data_block* block = queue.get_active_block(idx, queue.read_block);

      elem = &block->values[idx];

      // If closed, continue draining until the queue is empty
      // As indicated by the closed_at index
      if (queue.closed.load(std::memory_order_relaxed)) {
        if (roff >= queue.closed_at.load(std::memory_order_acquire)) {
          err = CLOSED;
          elem->flags.store(3, std::memory_order_release);
          return true;
        }
      }

      // Check if data is ready
      size_t flags = elem->flags.load(std::memory_order_relaxed);
      assert((flags & 2) == 0);

      if (flags & 1) {
        // Data is already ready here.
        t = std::move(elem->data);
        // Still need to store so block can be freed
        elem->flags.store(3, std::memory_order_release);
        return true;
      }
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
        return false;
      }
      return true;
    }
    std::variant<T, queue_error> await_resume()
      requires(!Must)
    {
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

  // template <bool Must> class aw_ticket_queue_pop {
  //   ticket_queue& queue;
  //   bool await_ready() {}
  //   bool await_suspend() {}
  //   std::variant<T, queue_error> await_resume()
  //     requires(!Must)
  //   {}
  //   T await_resume()
  //     requires(Must)
  //   {}
  // };

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

  // All currently waiting producers will return CLOSED.
  // Consumers will continue to read data until the queue is drained,
  // at which point all consumers will return CLOSED.
  void close() {
    closed.store(true, std::memory_order_seq_cst);
    blocks_lock.spin_lock();
    // try_free_block<true>();
    size_t woff = write_offset.fetch_add(1, std::memory_order_seq_cst);
    closed_at.store(woff, std::memory_order_seq_cst);
    size_t roff = read_offset.load(std::memory_order_seq_cst);
    data_block* block = blocks;
    size_t i = block->offset;

    // Wait for the queue to drain (elements prior to closed_at write index)
    while (true) {
      while (i < roff) {
        if (i < woff) {
          size_t idx = i & CapacityMask;
          auto v = &block->values[idx];
          // Data is present at these elements; wait for the queue to drain
          while (v->flags.load(std::memory_order_acquire) != 3) {
            TMC_CPU_PAUSE();
          }
        } else { // i >= woff
          break;
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
        TMC_CPU_PAUSE();
        roff = read_offset.load(std::memory_order_seq_cst);
      } else {
        break;
      }
    }

    // `closed` is accessed by relaxed load in consumer.
    // In order to ensure that it is seen in a timely fashion, this
    // creates a release sequence with the acquire load in consumer.
    roff = read_offset.fetch_add(1, std::memory_order_seq_cst);

    // No data will be written to these elements. They are past the closed_at
    // write index. `roff` is now the closed-at read index. Consumers will be
    // waiting at indexes prior to this index.
    while (i < roff) {
      size_t idx = i & CapacityMask;
      auto v = &block->values[idx];

      // Wait for consumer to appear
      size_t flags = v->flags.load(std::memory_order_acquire);
      while ((flags & 2) == 0) {
        // std::printf("waiting\n");
        TMC_CPU_PAUSE();
        flags = v->flags.load(std::memory_order_acquire);
      }

      // If flags & 1 is set, it indicates the consumer saw the closed flag
      // and did not wait. Otherwise, wakeup the waiting consumer.
      if ((flags & 1) == 0) {
        auto cons = v->consumer;
        cons->err = CLOSED;
        tmc::detail::post_checked(
          cons->continuation_executor, std::move(cons->continuation), cons->prio
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

  ~ticket_queue() {
    // TODO delete read blocks
    // auto phead = block_list;
    // while (phead != nullptr) {
    //   auto next = phead->next;
    //   delete phead;
    //   phead = next;
    // }
  }

  ticket_queue(const ticket_queue&) = delete;
  ticket_queue& operator=(const ticket_queue&) = delete;
  // TODO implement move constructor
};

} // namespace tmc
