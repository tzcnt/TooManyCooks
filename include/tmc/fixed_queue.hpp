// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

// Provides tmc::fixed_queue, an async deque of fixed size.
// This is an MPMC queue but has best performance when used with low
// concurrency.

// Producers may suspend when calling co_await push() if the queue is full.
// Consumers retrieve values in FIFO order with pull().
// Consumers retrieve values in LIFO order with pop();

enum queue_error { OK = 0, CLOSED = 1, EMPTY = 2, FULL = 3 };

#include "tmc/detail/concepts.hpp"

#include <array>
#include <coroutine>
#include <expected>

namespace tmc {

template <size_t Count, typename Result> class fixed_queue {
  std::array<Result, Count> data; // TODO replace this with aligned storage
  size_t count;
  struct waiter_block {
    waiter_block* next;
    waiter_block* prev;
    std::array<std::coroutine_handle<>, 6> values;
  };
  waiter_block* producers_head;
  waiter_block* producers_tail;
  waiter_block* consumers_head;
  waiter_block* consumers_tail;

  template <bool Must> class aw_fixed_queue_push {
    fixed_queue& queue;
    bool await_ready() {}
    bool await_suspend() {}
    queue_error await_resume()
      requires(!Must)
    {}
    void await_resume()
      requires(Must)
    {}
  };

  template <bool Must> class aw_fixed_queue_pull {
    fixed_queue& queue;
    bool await_ready() {}
    bool await_suspend() {}
    std::expected<Result, queue_error> await_resume()
      requires(!Must)
    {}
    Result await_resume()
      requires(Must)
    {}
  };

  template <bool Must> class aw_fixed_queue_pop {
    fixed_queue& queue;
    bool await_ready() {}
    bool await_suspend() {}
    std::expected<Result, queue_error> await_resume()
      requires(!Must)
    {}
    Result await_resume()
      requires(Must)
    {}
  };

public:
  // May return OK, FULL, or CLOSED
  queue_error try_push() {}
  // May return a value, or EMPTY or CLOSED
  std::expected<Result, queue_error> try_pull() {}
  // May return a value, or EMPTY OR CLOSED
  std::expected<Result, queue_error> try_pop() {}

  // May return OK or CLOSED
  aw_fixed_queue_push<false> push() {}
  // May return a value or CLOSED
  aw_fixed_queue_pull<false> pull() {}
  // May return a value or CLOSED
  aw_fixed_queue_pop<false> pop() {}

  // Returns void. If the queue is closed, std::terminate will be called.
  aw_fixed_queue_must<true> must_push() {}
  // Returns a value. If the queue is closed, std::terminate will be called.
  aw_fixed_queue_must<true> must_pull() {}
  // Returns a value. If the queue is closed, std::terminate will be called.
  aw_fixed_queue_must<true> must_pop() {}
};
} // namespace tmc