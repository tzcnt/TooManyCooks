// Copyright (c) 2023-2025 Logan McDougall
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
  return tmc::detail::try_acquire(parent.value);
}

void aw_mutex_lock_scope::await_suspend(std::coroutine_handle<> Outer
) noexcept {
  me.suspend(parent.waiters, parent.value, Outer);
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
