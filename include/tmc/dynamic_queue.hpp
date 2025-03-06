// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

// Provides tmc::dynamic_queue, an async queue of fixed size.
// This is an MPMC queue optimized for best performance at high concurrency.

enum queue_error { OK = 0, CLOSED = 1, EMPTY = 2, FULL = 3 };

#include "tmc/detail/concepts.hpp"
#include "tmc/detail/qu_lockfree.hpp"
#include "tmc/detail/thread_locals.hpp"

#include <coroutine>
#include <exception>
#include <variant>

namespace tmc {

template <typename T> class dynamic_queue {

public:
  struct aw_dynamic_queue_waiter_base {
    T t; // by value for now, possibly zero-copy / by ref in future
    dynamic_queue& queue;
    queue_error err;
    tmc::detail::type_erased_executor* continuation_executor;
    std::coroutine_handle<> continuation;
    size_t prio;
    template <typename U>
    aw_dynamic_queue_waiter_base(U&& u, dynamic_queue& q)
        : t(std::forward<U>(u)), queue(q), err{OK},
          continuation_executor{tmc::detail::this_thread::executor},
          continuation{nullptr},
          prio(tmc::detail::this_thread::this_task.prio) {}

    aw_dynamic_queue_waiter_base(dynamic_queue& q)
        : queue(q), err{OK},
          continuation_executor{tmc::detail::this_thread::executor},
          continuation{nullptr},
          prio(tmc::detail::this_thread::this_task.prio) {}
  };

  template <bool Must> class aw_dynamic_queue_push;
  template <bool Must> class aw_dynamic_queue_pull;

  // May return OK or CLOSED
  template <typename U> queue_error push(U t) {
    if (closed) {
      return CLOSED;
    }

    if (data.empty()) {
      // Queue is empty, might be consumers waiting
      aw_dynamic_queue_waiter_base* cons;
      if (consumers.try_dequeue(cons)) {
        cons->t = std::move(t);
        tmc::detail::post_checked(
          cons->continuation_executor, std::move(cons->continuation),
          cons->prio, TMC_ALL_ONES
        );
        return OK;
      }
    }
  AGAIN:
    data.enqueue(std::move(t));
    tmc::detail::memory_barrier();
    if (!consumers.empty()) {
      if (data.try_dequeue(t)) {
        tmc::detail::memory_barrier();
        aw_dynamic_queue_waiter_base* cons;
        if (consumers.try_dequeue(cons)) {
          cons->t = std::move(t);
          tmc::detail::post_checked(
            cons->continuation_executor, std::move(cons->continuation),
            cons->prio, TMC_ALL_ONES
          );
          return OK;
        } else {
          goto AGAIN;
        }
      }
    }
    return OK;
  }
  // May return a value or CLOSED
  aw_dynamic_queue_pull<false> pull() {
    return aw_dynamic_queue_pull<false>(*this);
  }

  tmc::queue::ConcurrentQueue<T> data;
  tmc::queue::ConcurrentQueue<aw_dynamic_queue_waiter_base*> consumers;

  bool closed;

  dynamic_queue() : closed{false} {}

  template <bool Must>
  class aw_dynamic_queue_pull : protected aw_dynamic_queue_waiter_base,
                                private tmc::detail::AwaitTagNoGroupAsIs {
    using aw_dynamic_queue_waiter_base::continuation;
    using aw_dynamic_queue_waiter_base::continuation_executor;
    using aw_dynamic_queue_waiter_base::err;
    using aw_dynamic_queue_waiter_base::queue;
    using aw_dynamic_queue_waiter_base::t;

    aw_dynamic_queue_pull(dynamic_queue& q) : aw_dynamic_queue_waiter_base(q) {}

    // May return a value or CLOSED
    friend aw_dynamic_queue_pull dynamic_queue::pull();

  public:
    bool await_ready() {
      if (queue.data.try_dequeue(t)) {
        return true;
      } else {
        if (queue.closed) {
          if constexpr (Must) {
            std::terminate();
          } else {
            err = CLOSED;
            return true;
          }
        }
        // queue is empty. unlock happens in await_suspend
        return false;
      }
    }
    void await_suspend(std::coroutine_handle<> Outer) {
      T elem;
      continuation = Outer;
      queue.consumers.enqueue(this);
    AGAIN:
      tmc::detail::memory_barrier();
      if (queue.consumers.empty()) {
        return;
      }
      tmc::detail::memory_barrier();
      if (!queue.data.empty() && queue.data.try_dequeue(elem)) {
        tmc::detail::memory_barrier();
        aw_dynamic_queue_waiter_base* cons;
        if (!queue.consumers.empty() && queue.consumers.try_dequeue(cons)) {
          cons->t = std::move(elem);
          tmc::detail::post_checked(
            cons->continuation_executor, std::move(cons->continuation),
            cons->prio, TMC_ALL_ONES
          );
          return;
        } else {
          queue.data.enqueue(std::move(elem));
          goto AGAIN;
        }
      }
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

  // template <bool Must> class aw_dynamic_queue_pop {
  //   dynamic_queue& queue;
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
  // aw_dynamic_queue_pop<false> pop() {}

  // Returns void. If the queue is closed, std::terminate will be called.
  aw_dynamic_queue_push<true> must_push() {}
  // Returns a value. If the queue is closed, std::terminate will be called.
  aw_dynamic_queue_pull<true> must_pull() {}
  // // Returns a value. If the queue is closed, std::terminate will be
  // called. aw_dynamic_queue_pop<true> must_pop() {}

  // All currently waiting producers will return CLOSED.
  // Consumers will continue to read data until the queue is drained,
  // at which point all consumers will return CLOSED.
  void close() { closed = true; }

  dynamic_queue(const dynamic_queue&) = delete;
  dynamic_queue& operator=(const dynamic_queue&) = delete;
  // TODO implement move constructor
};

} // namespace tmc
