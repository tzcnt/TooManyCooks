// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/thread_locals.hpp"
#include "tmc/detail/waiter_list.hpp"
#include "tmc/mutex.hpp"

#include <atomic>
#include <cassert>
#include <coroutine>

namespace tmc {
bool aw_mutex::await_ready() noexcept { return parent->try_lock(); }

bool aw_mutex::await_suspend(std::coroutine_handle<> Outer) noexcept {
  // Configure this awaiter
  me.continuation = Outer;
  me.continuation_executor = tmc::detail::this_thread::executor;
  me.continuation_priority = tmc::detail::this_thread::this_task.prio;

  // Add this awaiter to the waiter list
  parent->waiters.add_waiter(me);

  // Release the operation by increasing the waiter count
  auto add = TMC_ONE_BIT << mutex::WAITERS_OFFSET;
  auto v = add + parent->value.fetch_add(add, std::memory_order_acq_rel);

  // Using the fetched value, see if there are both resources available and
  // waiters to wake.
  parent->maybe_wake(v);
  return true;
}

void mutex::maybe_wake(size_t v) noexcept {
  size_t state, waiterCount, newV, wakeCount;
  do {
    unpack_value(v, state, waiterCount);
    if (state == LOCKED || waiterCount == 0) {
      return;
    }
    // By atomically modifying both values at once, this thread
    // "takes ownership" of the lock and 1 waiter simultaneously.
    newV = pack_value(LOCKED, waiterCount - 1);
  } while (!value.compare_exchange_strong(
    v, newV, std::memory_order_acq_rel, std::memory_order_acquire
  ));

  waiters.must_wake_n(1);
}

bool mutex::try_lock() noexcept {
  auto v = value.load(std::memory_order_relaxed);
  size_t state, waiterCount, newV;
  do {
    unpack_value(v, state, waiterCount);
    if (LOCKED == state) {
      return false;
    }
    newV = pack_value(LOCKED, waiterCount);
  } while (!value.compare_exchange_strong(
    v, newV, std::memory_order_acq_rel, std::memory_order_acquire
  ));
  return true;
}

void mutex::unlock() noexcept {
  assert(is_locked());
  size_t v = UNLOCKED | value.fetch_or(UNLOCKED, std::memory_order_release);
  maybe_wake(v);
}

mutex::~mutex() { waiters.wake_all(); }

} // namespace tmc
