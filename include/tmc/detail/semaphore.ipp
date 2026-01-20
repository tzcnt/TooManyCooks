// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/waiter_list.hpp"
#include "tmc/semaphore.hpp"

#include <atomic>
#include <coroutine>
#include <cstddef>

namespace tmc {
semaphore_scope::~semaphore_scope() {
  if (parent != nullptr) {
    parent->release(1);
  }
}

bool aw_semaphore_acquire_scope::await_ready() noexcept {
  return tmc::detail::try_acquire(parent.value);
}

void aw_semaphore_acquire_scope::await_suspend(
  std::coroutine_handle<> Outer
) noexcept {
  me.suspend(parent.waiters, parent.value, Outer);
}

std::coroutine_handle<>
aw_semaphore_co_release::await_suspend(std::coroutine_handle<> Outer) noexcept {
  size_t old = parent.value.fetch_add(1, std::memory_order_acq_rel);
  size_t v = 1 + old;

  auto toWake = parent.waiters.maybe_wake(parent.value, v, old, true);
  if (toWake == nullptr) {
    return Outer;
  }

  return toWake->try_symmetric_transfer(Outer);
}

void semaphore::release(size_t ReleaseCount) noexcept {
  size_t old = value.fetch_add(ReleaseCount, std::memory_order_acq_rel);
  size_t v = ReleaseCount + old;

  waiters.maybe_wake(value, v, old, false);
}

semaphore::~semaphore() { waiters.wake_all(); }
} // namespace tmc
