// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/waiter_list.hpp"
#include "tmc/mutex.hpp"

#include <atomic>
#include <cassert>
#include <coroutine>
#include <cstddef>

namespace tmc {
mutex_scope::~mutex_scope() {
  if (parent != nullptr) {
    parent->unlock();
  }
}

bool aw_mutex_lock_scope::await_ready() noexcept {
  return tmc::detail::try_acquire(
    parent.load(std::memory_order_relaxed)->value
  );
}

void aw_mutex_lock_scope::await_suspend(
  std::coroutine_handle<> Outer
) noexcept {
  // This may be resumed immediately after we call add_waiter(). Access to
  // any member variable after that point is UB. However we need to use the
  // value of parent after calling add_waiter(). Thus we need to ensure that
  // we have loaded it into a local variable prior.

  // The simplest way to ensure this is to make parent atomic (to guarantee
  // the load actually happens now) and use acquire-acquire ordering to ensure
  // the load cannot be moved past the cmpxchg in add_waiter().

  // In practice this results in identical codegen on Clang.
  me.suspend(parent.load(std::memory_order_acquire), Outer);
}

std::coroutine_handle<>
aw_mutex_co_unlock::await_suspend(std::coroutine_handle<> Outer) noexcept {
  assert(parent.is_locked());
  size_t old =
    parent.value.fetch_or(mutex::UNLOCKED, std::memory_order_acq_rel);
  size_t v = mutex::UNLOCKED | old;

  auto toWake = parent.waiters.maybe_wake(parent.value, v, old, true);
  if (toWake == nullptr) {
    return Outer;
  }

  return toWake->try_symmetric_transfer(Outer);
}

void mutex::unlock() noexcept {
  assert(is_locked());
  size_t old = value.fetch_or(UNLOCKED, std::memory_order_acq_rel);
  size_t v = UNLOCKED | old;
  waiters.maybe_wake(value, v, old, false);
}

mutex::~mutex() { waiters.wake_all(); }
} // namespace tmc
