// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/waiter_list.hpp"
#include "tmc/semaphore.hpp"

#include <atomic>
#include <cassert>
#include <coroutine>

namespace tmc {
bool aw_semaphore::await_ready() noexcept { return parent->try_acquire(); }

bool aw_semaphore::await_suspend(std::coroutine_handle<> Outer) noexcept {
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

  // Release the operation by increasing the waiter count
  auto add = TMC_ONE_BIT << semaphore::WAITERS_OFFSET;
  auto v = add + parent->value.fetch_add(add, std::memory_order_acq_rel);

  // Using the fetched value, see if there are both resources available and
  // waiters to wake.
  parent->maybe_wake(v);
  return true;
}

void semaphore::maybe_wake(size_t v) noexcept {
  size_t count, waiterCount, newV, wakeCount;
  do {
    unpack_value(v, count, waiterCount);
    if (count == 0 || waiterCount == 0) {
      return;
    }
    // By atomically subtracting from both values at once, this thread
    // "takes ownership" of wakeCount number of resources and waiters
    // simultaneously.
    if (count < waiterCount) {
      newV = pack_value(0, waiterCount - count);
      wakeCount = count;
    } else {
      newV = pack_value(count - waiterCount, 0);
      wakeCount = waiterCount;
    }
  } while (!value.compare_exchange_strong(
    v, newV, std::memory_order_acq_rel, std::memory_order_acquire
  ));

  auto toWake = waiters.load(std::memory_order_acquire);
  for (size_t i = 0; i < wakeCount; ++i) {
    do {
      // should be guaranteed to see at least wakeCount waiters
      assert(toWake != nullptr);
    } while (!waiters.compare_exchange_strong(
      toWake, toWake->next, std::memory_order_acq_rel, std::memory_order_acquire
    ));
    toWake->resume();
  }
}

bool semaphore::try_acquire() noexcept {
  auto v = value.load(std::memory_order_relaxed);
  size_t count, waiterCount, newV;
  do {
    unpack_value(v, count, waiterCount);
    if (0 == count) {
      return false;
    }
    newV = pack_value(count - 1, waiterCount);
  } while (!value.compare_exchange_strong(
    v, newV, std::memory_order_acq_rel, std::memory_order_acquire
  ));
  return true;
}

void semaphore::release(size_t ReleaseCount) noexcept {
  size_t v =
    ReleaseCount + value.fetch_add(ReleaseCount, std::memory_order_release);
  maybe_wake(v);
}

semaphore::~semaphore() {
  auto curr = waiters.exchange(nullptr, std::memory_order_acq_rel);
  while (curr != nullptr) {
    auto next = curr->next;
    curr->resume();
    curr = next;
  }
}

} // namespace tmc
