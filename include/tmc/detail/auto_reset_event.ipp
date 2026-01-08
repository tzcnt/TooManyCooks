// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/auto_reset_event.hpp"
#include "tmc/detail/waiter_list.hpp"

#include <atomic>
#include <coroutine>
#include <cstddef>

namespace tmc {
std::coroutine_handle<> aw_auto_reset_event_co_set::await_suspend(
  std::coroutine_handle<> Outer
) noexcept {
  size_t old = parent.value.load(std::memory_order_acquire);
  size_t v;
  do {
    tmc::detail::half_word oldCount;
    size_t oldWaiterCount;
    tmc::detail::unpack_value(old, oldCount, oldWaiterCount);
    // count is only allowed to be 1 greater than waiterCount
    if (oldCount >= oldWaiterCount + 1) {
      return Outer;
    }
    v = 1 + old;
  } while (!parent.value.compare_exchange_strong(
    old, v, std::memory_order_acq_rel, std::memory_order_acquire
  ));

  auto toWake = parent.waiters.maybe_wake(parent.value, v, old, true);
  if (toWake == nullptr) {
    return Outer;
  }

  return toWake->try_symmetric_transfer(Outer);
}

void auto_reset_event::reset() noexcept { tmc::detail::try_acquire(value); }

void auto_reset_event::set() noexcept {
  size_t old = value.load(std::memory_order_acquire);
  size_t v;
  do {
    tmc::detail::half_word oldCount;
    size_t oldWaiterCount;
    tmc::detail::unpack_value(old, oldCount, oldWaiterCount);
    // count is only allowed to be 1 greater than waiterCount
    if (oldCount >= oldWaiterCount + 1) {
      return;
    }
    v = 1 + old;
  } while (!value.compare_exchange_strong(
    old, v, std::memory_order_acq_rel, std::memory_order_acquire
  ));
  waiters.maybe_wake(value, v, old, false);
}

auto_reset_event::~auto_reset_event() { waiters.wake_all(); }
} // namespace tmc
