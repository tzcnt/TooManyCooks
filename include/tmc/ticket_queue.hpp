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
  };

  struct data_block {
    std::atomic<data_block*> next;
    std::array<element, Capacity> values;

    data_block() : next{nullptr}, values{} {}
  };

  // TODO align this (conditionally if coro aligned allocation is supported?)
  std::atomic<bool> closed;
  std::atomic<size_t> closed_at;
  // If write_offset == read_offset, queue is empty
  // If write_offset == read_offset - 1, queue is full
  // 1 element of capacity is wasted to make this work
  std::atomic<size_t> write_offset;
  std::atomic<size_t> read_offset;
  std::atomic<data_block*> write_block;
  std::atomic<data_block*> read_block;

  ticket_queue()
      : closed{false}, closed_at{TMC_ALL_ONES}, write_offset{0}, read_offset{0},
        write_block{new data_block}, read_block{write_block.load()} {}

  template <typename U> queue_error push(U&& u) {
    // Get write ticket and find the associated position
    size_t idx = write_offset.fetch_add(1, std::memory_order_acq_rel);
    // close() will set `closed` before incrementing write_offset
    // thus we are guaranteed to see it if we acquire write_offset first
    if (closed.load(std::memory_order_relaxed)) {
      return CLOSED;
    }
    data_block* block = write_block;
    element* elem;
    while (idx >= Capacity) {
      // TODO this just appends new blocks forever
      // Try storing the base offset in the block itself
      // And then atomic swapping write_block instead of
      // `write_offset -= Capacity`;
      // Note that read and write are both bumping the same next pointer here
      // Also need to scan the entire block to ensure all elements have been
      // consumed before swapping the block
      if (block->next == nullptr) {
        auto newBlock = new data_block;
        data_block* expected = nullptr;
        if (block->next.compare_exchange_weak(
              expected, newBlock, std::memory_order_acq_rel,
              std::memory_order_acquire
            )) {
          block = newBlock;
        } else {
          delete newBlock;
          block = expected;
        }
      } else {
        block = block->next.load(std::memory_order_acquire);
      }
      idx -= Capacity;
    }
    elem = &block->values[idx];
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
    if (!elem->flags.compare_exchange_weak(
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
      data_block* block = queue.read_block;

      // Get read ticket and find the associated position
      size_t roff = queue.read_offset.fetch_add(1, std::memory_order_acq_rel);
      size_t idx = roff;

      // If we ran out of space in the current block, allocate a new one
      while (idx >= Capacity) {
        // TODO this just appends new blocks forever
        // Try storing the base offset in the block itself
        // And then atomic swapping read_block instead of
        // `read_offset -= Capacity`;
        // Note that read and write are both bumping the same next pointer here
        // Also need to scan the entire block to ensure all elements have been
        // consumed before swapping the block
        if (block->next == nullptr) {
          auto newBlock = new data_block;
          data_block* expected = nullptr;
          if (block->next.compare_exchange_weak(
                expected, newBlock, std::memory_order_acq_rel,
                std::memory_order_acquire
              )) {
            block = newBlock;
          } else {
            delete newBlock;
            block = expected;
          }
        } else {
          block = block->next.load(std::memory_order_acquire);
        }
        idx -= Capacity;
      }
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
      if (!elem->flags.compare_exchange_weak(
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
    size_t i = 0;
    size_t woff = write_offset.fetch_add(1, std::memory_order_seq_cst);
    closed_at.store(woff, std::memory_order_seq_cst);
    size_t roff = read_offset.load(std::memory_order_seq_cst);
    data_block* block = read_block;

    // Wait for the queue to drain (elements prior to closed_at write index)
    while (true) {
      while (i < roff) {
        if (i > 0 && (i & CapacityMask) == 0) {
          data_block* next = block->next.load(std::memory_order_acquire);
          while (next == nullptr) {
            // A block is being constructed. Wait for it.
            TMC_CPU_PAUSE();
            next = block->next.load(std::memory_order_acquire);
          }
          block = next;
        }
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

      // If flags & 1 is set, it indicates the consumer saw the closed flag and
      // did not wait. Otherwise, wakeup the waiting consumer.
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
  }

  ~ticket_queue() {
    // TODO delete read blocks
    // auto phead = write_block;
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
