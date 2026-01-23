// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/thread_locals.hpp"
#include "tmc/detail/waiter_list.hpp"
#include "tmc/manual_reset_event.hpp"

#include <atomic>
#include <coroutine>

namespace tmc {
bool aw_manual_reset_event::await_suspend(
  std::coroutine_handle<> Outer
) noexcept {
  me.waiter.continuation = Outer;
  me.waiter.continuation_executor = tmc::detail::this_thread::executor;
  me.waiter.continuation_priority = tmc::detail::this_thread::this_task.prio;
  auto h = parent.head.load(std::memory_order_acquire);
  do {
    if (manual_reset_event::READY == h) {
      // It was ready, don't wait
      return false;
    }
    me.next = reinterpret_cast<tmc::detail::waiter_list_node*>(h);
  } while (!parent.head.compare_exchange_strong(
    h, reinterpret_cast<uintptr_t>(&me), std::memory_order_acq_rel,
    std::memory_order_acquire
  ));
  return true;
}

std::coroutine_handle<> aw_manual_reset_event_co_set::await_suspend(
  std::coroutine_handle<> Outer
) noexcept {
  auto h =
    parent.head.exchange(manual_reset_event::READY, std::memory_order_acq_rel);
  if (manual_reset_event::READY == h || manual_reset_event::NOT_READY == h) {
    // It was ready, or there are no waiters - just resume
    return Outer;
  }

  // Save the first / most recently added waiter for symmetric transfer.
  auto toWake = reinterpret_cast<tmc::detail::waiter_list_node*>(h);

  auto curr = toWake->next;
  while (curr != nullptr) {
    auto next = curr->next;
    curr->waiter.resume();
    curr = next;
  }

  return toWake->waiter.try_symmetric_transfer(Outer);
}

void manual_reset_event::set() noexcept {
  auto h = head.exchange(READY, std::memory_order_acq_rel);
  if (READY == h) {
    // It was ready, nothing to wake
    return;
  }
  auto curr = reinterpret_cast<tmc::detail::waiter_list_node*>(h);
  while (curr != nullptr) {
    auto next = curr->next;
    curr->waiter.resume();
    curr = next;
  }
}

manual_reset_event::~manual_reset_event() noexcept { set(); }
} // namespace tmc
