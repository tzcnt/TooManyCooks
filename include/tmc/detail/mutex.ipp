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
bool aw_mutex::await_ready() noexcept { return parent.try_lock(); }

bool aw_mutex::await_suspend(std::coroutine_handle<> Outer) noexcept {
  // Configure this awaiter
  me.waiter.continuation = Outer;
  me.waiter.continuation_executor = tmc::detail::this_thread::executor;
  me.waiter.continuation_priority = tmc::detail::this_thread::this_task.prio;

  // Add this awaiter to the waiter list
  parent.waiters.add_waiter(me);

  // Release the operation by increasing the waiter count
  auto add = TMC_ONE_BIT << tmc::detail::WAITERS_OFFSET;
  auto v = add + parent.value.fetch_add(add, std::memory_order_acq_rel);

  // Using the fetched value, see if there are both resources available and
  // waiters to wake.
  parent.maybe_wake(v);
  return true;
}

mutex_scope::~mutex_scope() {
  if (parent != nullptr) {
    parent->unlock();
  }
}

bool aw_mutex_lock_scope::await_ready() noexcept { return parent.try_lock(); }

bool aw_mutex_lock_scope::await_suspend(std::coroutine_handle<> Outer
) noexcept {
  // Configure this awaiter
  me.waiter.continuation = Outer;
  me.waiter.continuation_executor = tmc::detail::this_thread::executor;
  me.waiter.continuation_priority = tmc::detail::this_thread::this_task.prio;

  // Add this awaiter to the waiter list
  parent.waiters.add_waiter(me);

  // Release the operation by increasing the waiter count
  auto add = TMC_ONE_BIT << tmc::detail::WAITERS_OFFSET;
  auto v = add + parent.value.fetch_add(add, std::memory_order_acq_rel);

  // Using the fetched value, see if there are both resources available and
  // waiters to wake.
  parent.maybe_wake(v);
  return true;
}

std::coroutine_handle<>
aw_mutex_co_unlock::await_suspend(std::coroutine_handle<> Outer) noexcept {
  assert(parent.is_locked());
  size_t v = mutex::UNLOCKED |
             parent.value.fetch_or(mutex::UNLOCKED, std::memory_order_release);
  tmc::detail::half_word state;
  size_t waiterCount, newV, wakeCount;
  do {
    tmc::detail::unpack_value(v, state, waiterCount);
    // assert(state == mutex::UNLOCKED);
    if (waiterCount == 0) {
      // No waiters - just resume
      return Outer;
    }
    // By atomically modifying both values at once, this thread
    // "takes ownership" of the lock and 1 waiter simultaneously.
    newV = tmc::detail::pack_value(mutex::LOCKED, waiterCount - 1);
  } while (!parent.value.compare_exchange_strong(
    v, newV, std::memory_order_acq_rel, std::memory_order_acquire
  ));

  auto toWake = parent.waiters.must_take_1();
  return toWake->waiter.try_symmetric_transfer(Outer);
}

void mutex::maybe_wake(size_t v) noexcept {
  tmc::detail::half_word state;
  size_t waiterCount, newV, wakeCount;
  do {
    tmc::detail::unpack_value(v, state, waiterCount);
    if (state == LOCKED || waiterCount == 0) {
      return;
    }
    // By atomically modifying both values at once, this thread
    // "takes ownership" of the lock and 1 waiter simultaneously.
    newV = tmc::detail::pack_value(LOCKED, waiterCount - 1);
  } while (!value.compare_exchange_strong(
    v, newV, std::memory_order_acq_rel, std::memory_order_acquire
  ));

  auto toWake = waiters.must_take_1();
  toWake->waiter.resume();
}

bool mutex::try_lock() noexcept {
  auto v = value.load(std::memory_order_relaxed);
  tmc::detail::half_word state;
  size_t waiterCount, newV;
  do {
    tmc::detail::unpack_value(v, state, waiterCount);
    if (LOCKED == state) {
      return false;
    }
    newV = tmc::detail::pack_value(LOCKED, waiterCount);
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
