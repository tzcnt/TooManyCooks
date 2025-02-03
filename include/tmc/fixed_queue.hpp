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

#include "tmc/detail/concepts.hpp"

#include <array>
#include <coroutine>

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

  class aw_fixed_queue_push {
    fixed_queue& queue;
    bool await_ready() {}
    bool await_suspend() {}
    void await_resume() {}
  };

  class aw_fixed_queue_pull {
    fixed_queue& queue;
    bool await_ready() {}
    bool await_suspend() {}
    void await_resume() {}
  };

  class aw_fixed_queue_pop {
    fixed_queue& queue;
    bool await_ready() {}
    bool await_suspend() {}
    void await_resume() {}
  };

public:
  aw_fixed_queue_push push() {}
  aw_fixed_queue_pull pull() {}
  aw_fixed_queue_pop pop() {}
};
} // namespace tmc