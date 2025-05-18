// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/auto_reset_event.hpp"
#include "tmc/detail/thread_locals.hpp"
#include "tmc/detail/waiter_list.hpp"

#include <atomic>
#include <cassert>
#include <coroutine>

namespace tmc {
bool aw_auto_reset_event::await_ready() noexcept {
  auto v = parent.value.load(std::memory_order_relaxed);

  tmc::detail::half_word count;
  size_t waiterCount, newV;
  do {
    tmc::detail::unpack_value(v, count, waiterCount);
    if (0 == count) {
      return false;
    }
    newV = tmc::detail::pack_value(count - 1, waiterCount);
  } while (!parent.value.compare_exchange_strong(
    v, newV, std::memory_order_acq_rel, std::memory_order_acquire
  ));
  return true;
}

bool aw_auto_reset_event::await_suspend(std::coroutine_handle<> Outer
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
aw_auto_reset_event_co_set::await_suspend(std::coroutine_handle<> Outer
) noexcept {
  // same as aw_semaphore_co_release::await_suspend, but saturates count to 1
  size_t v = 1 + parent.value.fetch_add(1, std::memory_order_release);

  tmc::detail::half_word count;
  size_t waiterCount, newV, wakeCount;
  do {
    tmc::detail::unpack_value(v, count, waiterCount);
    // By atomically subtracting from both values at once, this thread
    // "takes ownership" of wakeCount number of resources and waiters
    // simultaneously.
    if (count <= waiterCount) {
      newV = tmc::detail::pack_value(0, waiterCount - count);
      wakeCount = count;
    } else {
      // Saturate the readiness to 1
      newV = tmc::detail::pack_value(1, 0);
      wakeCount = waiterCount;
    }
  } while (!parent.value.compare_exchange_strong(
    v, newV, std::memory_order_acq_rel, std::memory_order_acquire
  ));

  // No early return in the do loop because we need to saturate the readiness.
  if (wakeCount == 0) {
    return Outer;
  }

  auto toWake = parent.waiters.must_take_1();
  --wakeCount;
  if (wakeCount != 0) {
    parent.waiters.must_wake_n(wakeCount);
  }
  return toWake->waiter.try_symmetric_transfer(Outer);
}

void auto_reset_event::maybe_wake(size_t v) noexcept {
  // same as aw_semaphore::maybe_wake, but saturates count to 1
  tmc::detail::half_word count;
  size_t waiterCount, newV, wakeCount;
  do {
    tmc::detail::unpack_value(v, count, waiterCount);
    // By atomically subtracting from both values at once, this thread
    // "takes ownership" of wakeCount number of resources and waiters
    // simultaneously.
    if (count <= waiterCount) {
      newV = tmc::detail::pack_value(0, waiterCount - count);
      wakeCount = count;
    } else {
      // Saturate the readiness to 1
      newV = tmc::detail::pack_value(1, 0);
      wakeCount = waiterCount;
    }
  } while (!value.compare_exchange_strong(
    v, newV, std::memory_order_acq_rel, std::memory_order_acquire
  ));

  // No early return in the do loop because we need to saturate the readiness.
  if (wakeCount == 0) {
    return;
  }

  waiters.must_wake_n(wakeCount);
}

void auto_reset_event::reset() noexcept {
  auto v = value.load(std::memory_order_relaxed);

  tmc::detail::half_word count;
  size_t waiterCount, newV;
  do {
    tmc::detail::unpack_value(v, count, waiterCount);
    newV = tmc::detail::pack_value(0, waiterCount);
  } while (!value.compare_exchange_strong(
    v, newV, std::memory_order_acq_rel, std::memory_order_acquire
  ));
}

void auto_reset_event::set() noexcept {
  size_t v = 1 + value.fetch_add(1, std::memory_order_release);
  maybe_wake(v);
}

auto_reset_event::~auto_reset_event() { waiters.wake_all(); }

} // namespace tmc
