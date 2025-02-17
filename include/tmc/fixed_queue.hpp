// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

// Provides tmc::fixed_queue, an async queue of fixed size.
// This is an MPMC queue but has best performance when used with low
// concurrency.

// Producers may suspend when calling co_await push() if the queue is full.
// Consumers retrieve values in FIFO order with pull().
// Consumers retrieve values in LIFO order with pop();

enum queue_error { OK = 0, CLOSED = 1, EMPTY = 2, FULL = 3 };

#include "bounded_queue.hpp"
#include "tmc/detail/thread_locals.hpp"
#include "tmc/detail/tiny_lock.hpp"

#include <array>
#include <coroutine>
#include <exception>
#include <variant>

namespace tmc {

template <typename T, size_t Size = 256> class fixed_queue {
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

  struct aw_fixed_queue_waiter_base {
    T t; // by value for now, possibly zero-copy / by ref in future
    fixed_queue& queue;
    queue_error err;
    tmc::detail::type_erased_executor* continuation_executor;
    std::coroutine_handle<> continuation;
    size_t prio;
    template <typename U>
    aw_fixed_queue_waiter_base(U&& u, fixed_queue& q)
        : t(std::forward<U>(u)), queue(q), err{OK},
          continuation_executor{tmc::detail::this_thread::executor},
          continuation{nullptr},
          prio(tmc::detail::this_thread::this_task.prio) {}

    aw_fixed_queue_waiter_base(fixed_queue& q)
        : queue(q), err{OK},
          continuation_executor{tmc::detail::this_thread::executor},
          continuation{nullptr},
          prio(tmc::detail::this_thread::this_task.prio) {}
  };

  template <bool Must> class aw_fixed_queue_push;
  template <bool Must> class aw_fixed_queue_pull;

  // May return OK or CLOSED
  template <typename U> aw_fixed_queue_push<false> push(U&& t) {
    return aw_fixed_queue_push<false>(*this, std::forward<U>(t));
  }
  // May return a value or CLOSED
  aw_fixed_queue_pull<false> pull() {
    return aw_fixed_queue_pull<false>(*this);
  }

  struct waiter_block {
    // waiter_block* prev;
    waiter_block* next;
    // Unlike the main queue, this isn't circular
    // Offsets will never exceed 5
    size_t write_offset;
    size_t read_offset;
    std::array<aw_fixed_queue_waiter_base*, 13> values;

    waiter_block() : next{nullptr}, write_offset{0}, read_offset{0} {}
    inline bool can_push() { return write_offset < values.size(); }
    inline void push(aw_fixed_queue_waiter_base* v) {
      values[write_offset] = v;
      ++write_offset;
    }
    inline bool can_pull() { return read_offset < write_offset; }
    inline aw_fixed_queue_waiter_base* pull() {
      auto v = values[read_offset];
      ++read_offset;
      return v;
    }
  };

  struct waiter_list {
    waiter_block* head;
    // insertion point, not necessarily the end of the list's capacity
    waiter_block* tail;

    waiter_list() : head{nullptr}, tail{nullptr} {}

    inline void push(aw_fixed_queue_waiter_base* prod) {
      auto ptail = tail;
      if (ptail == nullptr) {
        assert(head == nullptr);
        ptail = new waiter_block;
        tail = ptail;
        head = ptail;
      } else if (!ptail->can_push()) {
        if (ptail->next != nullptr) {
          ptail = ptail->next;
          tail = ptail;
          assert(ptail->can_push());
        } else {
          auto next = new waiter_block;
          ptail->next = next;
          ptail = next;
          tail = ptail;
        }
      }
      ptail->push(prod);
    }

    inline aw_fixed_queue_waiter_base* pull() {
      auto phead = head;
      if (phead == nullptr || !phead->can_pull()) {
        return nullptr;
      }
      auto v = phead->pull();
      if (!phead->can_pull() && !phead->can_push()) {
        phead->write_offset = 0;
        phead->read_offset = 0;
        if (phead != tail) {
          // Move empty head block to tail for reuse
          head = phead->next;
          phead->next = tail->next;
          tail->next = phead;
        }
      }
      return v;
    }

    inline void close() {
      for (auto* waiter = pull(); waiter != nullptr; waiter = pull()) {
        waiter->err = CLOSED;
        tmc::detail::post_checked(
          waiter->continuation_executor, std::move(waiter->continuation),
          waiter->prio
        );
      }
    }
    ~waiter_list() {
      auto phead = head;
      while (phead != nullptr) {
        auto next = phead->next;
        delete phead;
        phead = next;
      }
    }
  };

  // TODO align this (conditionally if coro aligned allocation is supported?)
  tmc::tiny_lock lock;
  bool closed;
  // If write_offset == read_offset, queue is empty
  // If write_offset == read_offset - 1, queue is full
  // 1 element of capacity is wasted to make this work
  size_t write_offset;
  size_t read_offset;
  waiter_list producers;
  waiter_list consumers;

  // TODO replace this with aligned storage
  // (support non-default-constructible types)
  std::array<T, Capacity> data;

  fixed_queue() : closed{false}, write_offset{0}, read_offset{0} {}

  template <bool Must>
  class aw_fixed_queue_push final : protected aw_fixed_queue_waiter_base {
    using aw_fixed_queue_waiter_base::continuation;
    using aw_fixed_queue_waiter_base::continuation_executor;
    using aw_fixed_queue_waiter_base::err;
    using aw_fixed_queue_waiter_base::queue;
    using aw_fixed_queue_waiter_base::t;
    template <typename U>
    aw_fixed_queue_push(fixed_queue& q, U&& u)
        : aw_fixed_queue_waiter_base(std::forward<U>(u), q) {}

    template <typename U> friend aw_fixed_queue_push fixed_queue::push(U&&);

  public:
    bool await_ready() {
      queue.lock.spin_lock();
      if (queue.closed) {
        if constexpr (Must) {
          std::terminate();
        } else {
          err = CLOSED;
          queue.lock.unlock();
          return true;
        }
      }

      if (queue.write_offset == queue.read_offset) {
        // Queue is empty, might be consumers waiting
        auto cons = queue.consumers.pull();
        if (cons != nullptr) {
          queue.lock.unlock();
          cons->t = std::move(t);
          tmc::detail::post_checked(
            cons->continuation_executor, std::move(cons->continuation),
            cons->prio
          );
          return true;
        }
      }
      if (queue.write_offset - queue.read_offset != CapacityMask) {
        // Queue isn't full, we can write
        size_t pos = queue.write_offset & CapacityMask;
        ++queue.write_offset;
        queue.data[pos] =
          std::move(t); // TODO emplace if not default constructible
        queue.lock.unlock();
        return true;
      } else {
        // queue is full. unlock happens in await_suspend
        return false;
      }
    }

    void await_suspend(std::coroutine_handle<> Outer) {
      continuation = Outer;
      queue.producers.push(this);
      queue.lock.unlock();
    }
    queue_error await_resume()
      requires(!Must)
    {
      return err;
    }
    void await_resume()
      requires(Must)
    {}

    /// If the push operation returns CLOSED, the value will not be enqueued.
    /// You can retrieve the value that failed to enqueue here.
    T&& get_rejected_value() { return std::move(t); }
  };

  template <bool Must>
  class aw_fixed_queue_pull : protected aw_fixed_queue_waiter_base {
    using aw_fixed_queue_waiter_base::continuation;
    using aw_fixed_queue_waiter_base::continuation_executor;
    using aw_fixed_queue_waiter_base::err;
    using aw_fixed_queue_waiter_base::queue;
    using aw_fixed_queue_waiter_base::t;

    aw_fixed_queue_pull(fixed_queue& q) : aw_fixed_queue_waiter_base(q) {}

    // May return a value or CLOSED
    friend aw_fixed_queue_pull fixed_queue::pull();

  public:
    bool await_ready() {
      queue.lock.spin_lock();
      size_t roff = queue.read_offset;
      size_t woff = queue.write_offset;
      if (roff != woff) {
        // Data is ready in the queue
        size_t rpos = roff & CapacityMask;
        queue.read_offset = roff + 1;
        // TODO in-place destroy if not default constructible
        t = std::move(queue.data[rpos]);

        if (woff - roff == CapacityMask) {
          // Queue was full before we pulled, might be producers waiting
          auto prod = queue.producers.pull();
          if (prod != nullptr) {
            size_t wpos = woff & CapacityMask;
            queue.write_offset = woff + 1;
            queue.data[wpos] =
              std::move(prod->t); // TODO emplace if not default constructible
            queue.lock.unlock();
            tmc::detail::post_checked(
              prod->continuation_executor, std::move(prod->continuation),
              prod->prio
            );
            return true;
          }
        }

        queue.lock.unlock();
        return true;
      } else {
        if (queue.closed) {
          if constexpr (Must) {
            std::terminate();
          } else {
            err = CLOSED;
            queue.lock.unlock();
            return true;
          }
        }
        // queue is empty. unlock happens in await_suspend
        return false;
      }
    }
    void await_suspend(std::coroutine_handle<> Outer) {
      continuation = Outer;
      queue.consumers.push(this);
      queue.lock.unlock();
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

  // template <bool Must> class aw_fixed_queue_pop {
  //   fixed_queue& queue;
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
  queue_error try_push() {}
  // May return a value, or EMPTY or CLOSED
  std::variant<T, queue_error> try_pull() {}
  // May return a value, or EMPTY OR CLOSED
  std::variant<T, queue_error> try_pop() {}

  // // May return a value or CLOSED
  // aw_fixed_queue_pop<false> pop() {}

  // Returns void. If the queue is closed, std::terminate will be called.
  aw_fixed_queue_push<true> must_push() {}
  // Returns a value. If the queue is closed, std::terminate will be called.
  aw_fixed_queue_pull<true> must_pull() {}
  // // Returns a value. If the queue is closed, std::terminate will be
  // called. aw_fixed_queue_pop<true> must_pop() {}

  // All currently waiting producers will return CLOSED.
  // Consumers will continue to read data until the queue is drained,
  // at which point all consumers will return CLOSED.
  void close() {
    tmc::tiny_lock_guard lg(lock);
    closed = true;
    producers.close();
    if (write_offset == read_offset) {
      // Queue is empty, wake up all consumers
      consumers.close();
    }
  }

  ~fixed_queue() {
    tmc::tiny_lock_guard lg(lock);
    // Wakeup any remaining producers and consumers immediately
    // TODO should this be an error? any waiters at this point are likely to
    // trigger a race condition
    producers.close();
    consumers.close();
  }
};

} // namespace tmc
