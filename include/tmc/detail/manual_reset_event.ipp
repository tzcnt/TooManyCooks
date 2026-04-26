// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/impl.hpp" // IWYU pragma: keep

#include "tmc/detail/thread_locals.hpp"
#include "tmc/detail/waiter_list.hpp"
#include "tmc/manual_reset_event.hpp"

#include <array>
#include <atomic>
#include <coroutine>

namespace tmc {
bool aw_manual_reset_event::await_suspend(
  std::coroutine_handle<> Outer
) noexcept {
  me.waiter.continuation = Outer;
  me.waiter.continuation_executor = tmc::detail::this_thread::executor();
  me.waiter.continuation_priority = tmc::detail::this_thread::this_task().prio;
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

size_t manual_reset_event::set() noexcept {
  auto h = head.exchange(READY, std::memory_order_acq_rel);
  if (READY == h) {
    // It was ready, nothing to wake
    return 0;
  }

  std::array<tmc::work_item, 64> batch;
  size_t batchSize = 0;
  tmc::ex_any* continuationExecutor = nullptr;
  size_t continuationPriority = 0;
  size_t wakeCount = 0;

  auto curr = reinterpret_cast<tmc::detail::waiter_list_node*>(h);
  while (curr != nullptr) {
    auto next = curr->next;

    if (batchSize == 0) {
      continuationExecutor = curr->waiter.continuation_executor;
      continuationPriority = curr->waiter.continuation_priority;
    }

    batch[batchSize] = std::move(curr->waiter.continuation);
    ++batchSize;
    ++wakeCount;
    if (batchSize == batch.size()) {
      continuationExecutor->post_bulk(
        batch.data(), batchSize, continuationPriority
      );
      batchSize = 0;
    }

    curr = next;
  }

  if (batchSize != 0) {
    continuationExecutor->post_bulk(batch.data(), batchSize, continuationPriority);
  }

  return wakeCount;
}

manual_reset_event::~manual_reset_event() noexcept { set(); }
} // namespace tmc
