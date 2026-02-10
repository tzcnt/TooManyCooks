// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/impl.hpp" // IWYU pragma: keep

#include "tmc/detail/thread_locals.hpp"
#include "tmc/detail/waiter_list.hpp"

#include <atomic>
#include <cassert>
#include <cstddef>

namespace tmc {
namespace detail {

void waiter_list_node::suspend(
  tmc::detail::waiter_data_base* Parent, std::coroutine_handle<> Outer
) noexcept {
  auto& parentList = Parent->waiters;
  auto& parentValue = Parent->value;

  // Configure this awaiter
  waiter.continuation = Outer;
  waiter.continuation_executor = tmc::detail::this_thread::executor();
  waiter.continuation_priority = tmc::detail::this_thread::this_task().prio;

  // Add this awaiter to the waiter list
  Parent->waiters.add_waiter(*this);

  // Release the operation by increasing the waiter count
  size_t add = TMC_ONE_BIT << tmc::detail::WAITERS_OFFSET;
  size_t old = parentValue.fetch_add(add, std::memory_order_acq_rel);
  size_t v = add + old;

  // Using the fetched value, see if there are both resources available and
  // waiters to wake.
  parentList.maybe_wake(parentValue, v, old, false);
}

void waiter_list_waiter::resume() noexcept {
  tmc::detail::post_checked(
    continuation_executor, std::move(continuation), continuation_priority
  );
}

std::coroutine_handle<> waiter_list_waiter::try_symmetric_transfer(
  std::coroutine_handle<> Outer
) noexcept {
  if (tmc::detail::this_thread::exec_prio_is(
        continuation_executor, continuation_priority
      )) {
    // Post Outer, symmetric transfer to this
    tmc::detail::post_checked(
      continuation_executor, std::move(Outer), continuation_priority
    );
    return continuation;
  } else {
    // Post this, return Outer (symmetric transfer back to caller)
    resume();
    return Outer;
  }
}

void waiter_list::add_waiter(tmc::detail::waiter_list_node& w) noexcept {
  auto h = head.load(std::memory_order_acquire);
  do {
    w.next = h;
  } while (!head.compare_exchange_strong(
    h, &w, std::memory_order_acq_rel, std::memory_order_acquire
  ));
}

void waiter_list::wake_all() noexcept {
  auto curr = head.exchange(nullptr, std::memory_order_acq_rel);
  while (curr != nullptr) {
    auto next = curr->next;
    curr->waiter.resume();
    curr = next;
  }
}

waiter_list_node* waiter_list::take_all() noexcept {
  return head.exchange(nullptr, std::memory_order_acq_rel);
}

waiter_list_waiter* waiter_list::maybe_wake(
  std::atomic<size_t>& value, size_t v, size_t old, bool symmetric
) noexcept {
  {
    tmc::detail::half_word oldCount;
    size_t oldWaiterCount;
    tmc::detail::unpack_value(old, oldCount, oldWaiterCount);
    // 4 possible prior states:
    // - count == 0 && waitercount == 0 -> nothing to wake
    // - count > 0 && waiterCount > 0 -> another thread is already executing a
    // wake operation
    // - count == 0 && waiterCount > 0 -> can wake if count++
    // - count > 0 && waitercount == 0 -> can wake if waiterCount++
    if ((oldCount == 0) == (oldWaiterCount == 0)) {
      return nullptr;
    }
  }
  {
    tmc::detail::half_word count;
    size_t waiterCount, wakeCount;
    size_t totalWakeCount = 0;
    tmc::detail::waiter_list_node wakeHead;
    wakeHead.next = nullptr;
    tmc::detail::waiter_list_node* wakeTail = &wakeHead;
    // Only one thread can enter the below section at a time.
    // Transitioning from 0/N or N/0 to N/N state acquires the critical section.
    // Transitioning back to 0/0, 0/N, or N/0 state releases the critical
    // section. The critical section is only needed to control access to the
    // `next` pointer of the shared waiters list, which occurs in must_take_1.
    while (true) {
      tmc::detail::unpack_value(v, count, waiterCount);
      wakeCount = count < waiterCount ? count : waiterCount;
      if (wakeCount == 0) {
        break;
      }
      totalWakeCount += wakeCount;

      // Take N waiters
      for (size_t i = 0; i < wakeCount; ++i) {
        auto toWake = must_take_1();
        wakeTail->next = toWake;
        wakeTail = toWake;
      }

      // (maybe) release the critical section
      size_t newV = tmc::detail::pack_value(
        count - static_cast<half_word>(wakeCount), waiterCount - wakeCount
      );
      while (!value.compare_exchange_strong(
        v, newV, std::memory_order_acq_rel, std::memory_order_acquire
      )) {
        tmc::detail::unpack_value(v, count, waiterCount);
        assert(count >= wakeCount);
        assert(waiterCount >= wakeCount);
        newV = tmc::detail::pack_value(
          count - static_cast<half_word>(wakeCount), waiterCount - wakeCount
        );
      }

      // Update the value of v and run again. If both values are still non-zero,
      // then the critical section was not actually released and we should
      // continue to take more waiters.
      v = newV;
    }

    if (totalWakeCount == 0) {
      return nullptr;
    }

    // wakeHead is a dummy object; its next pointer is the first real waiter
    auto toWake = wakeHead.next;

    tmc::detail::waiter_list_waiter* symmetric_task = nullptr;
    if (symmetric) {
      // Capture the first element of the list for symmetric transfer.
      // Caller will handle it.
      symmetric_task = &toWake->waiter;
      toWake = toWake->next;
      --totalWakeCount;
    }

    // Resume the rest of the waiters.
    for (size_t i = 0; i < totalWakeCount; ++i) {
      auto next = toWake->next;
      toWake->waiter.resume();
      toWake = next;
    }

    return symmetric_task;
  }
}

waiter_list_node* waiter_list::must_take_1() noexcept {
  auto toWake = head.load(std::memory_order_acquire);
  do {
    // should be guaranteed to see at least 1 waiter
    assert(toWake != nullptr);
  } while (!head.compare_exchange_strong(
    toWake, toWake->next, std::memory_order_acq_rel, std::memory_order_acquire
  ));
  return toWake;
}

bool try_acquire(std::atomic<size_t>& Value) noexcept {
  auto v = Value.load(std::memory_order_relaxed);

  tmc::detail::half_word count;
  size_t waiterCount, newV;
  do {
    tmc::detail::unpack_value(v, count, waiterCount);
    if (count <= waiterCount) {
      return false;
    }
    newV = tmc::detail::pack_value(count - 1, waiterCount);
  } while (!Value.compare_exchange_strong(
    v, newV, std::memory_order_acq_rel, std::memory_order_acquire
  ));
  return true;
}

} // namespace detail

bool aw_acquire::await_ready() noexcept {
  return tmc::detail::try_acquire(
    parent.load(std::memory_order_relaxed)->value
  );
}

void aw_acquire::await_suspend(std::coroutine_handle<> Outer) noexcept {
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
} // namespace tmc
