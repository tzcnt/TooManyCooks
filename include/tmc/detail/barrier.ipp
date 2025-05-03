// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/barrier.hpp"
#include "tmc/detail/waiter_list.hpp"

#include <atomic>
#include <coroutine>

namespace tmc {
bool aw_barrier::await_suspend(std::coroutine_handle<> Outer) noexcept {
  // Configure this awaiter
  me.continuation = Outer;
  me.continuation_executor = tmc::detail::this_thread::executor;
  me.continuation_priority = tmc::detail::this_thread::this_task.prio;

  // Add this awaiter to the waiter list
  auto h = parent->waiters.load(std::memory_order_acquire);
  do {
    me.next = h;
  } while (!parent->waiters.compare_exchange_strong(
    h, &me, std::memory_order_acq_rel, std::memory_order_acquire
  ));

  // Decrement and check the barrier count
  auto remaining = parent->done_count.fetch_sub(1, std::memory_order_acq_rel);
  if (remaining > 0) {
    return true;
  }

  // If the waiters are resumed before this is reset, they may immediately
  // await this again, and resume immediately. To prevent this, we must get the
  // list of waiters to be resumed, then set this to the not-ready state,
  // then resume the waiters.

  // Get the waiters
  auto curr = parent->waiters.exchange(nullptr, std::memory_order_acq_rel);

  // Reset this
  parent->done_count = parent->start_count.load();

  // Resume the waiters
  while (curr != nullptr) {
    auto next = curr->next;
    if (curr != &me) {
      // Symmetric transfer to this coroutine.
      // Others are posted to the executor.
      curr->resume();
    }
    curr = next;
  }
  return false;
}

barrier::~barrier() {
  auto curr = waiters.exchange(nullptr, std::memory_order_acq_rel);
  while (curr != nullptr) {
    auto next = curr->next;
    curr->resume();
    curr = next;
  }
}
} // namespace tmc
