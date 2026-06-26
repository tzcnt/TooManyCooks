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
namespace detail {
// Submits a chain of waiters to their continuation executors, grouping
// consecutive waiters that share an executor and priority into bulk posts.
// Returns the number of waiters woken.
inline size_t wake_waiters_in_batches(tmc::detail::waiter_list_node* curr) noexcept {
  std::array<tmc::work_item, 64> batch;
  size_t batchSize = 0;
  tmc::ex_any* continuationExecutor = nullptr;
  size_t continuationPriority = 0;
  size_t wakeCount = 0;

  while (curr != nullptr) {
    auto next = curr->next;

    // A batch can only be posted to a single executor at a single priority.
    // If this waiter's executor or priority differs from the batch in
    // progress, post that batch before starting a new one for this waiter.
    if (batchSize != 0 && (curr->waiter.continuation_executor != continuationExecutor ||
                           curr->waiter.continuation_priority != continuationPriority)) {
      continuationExecutor->post_bulk(batch.data(), batchSize, continuationPriority);
      batchSize = 0;
    }

    if (batchSize == 0) {
      continuationExecutor = curr->waiter.continuation_executor;
      continuationPriority = curr->waiter.continuation_priority;
    }

    batch[batchSize] = curr->waiter.continuation;
    ++batchSize;
    ++wakeCount;
    if (batchSize == batch.size()) {
      continuationExecutor->post_bulk(batch.data(), batchSize, continuationPriority);
      batchSize = 0;
    }

    curr = next;
  }

  if (batchSize != 0) {
    continuationExecutor->post_bulk(batch.data(), batchSize, continuationPriority);
  }

  return wakeCount;
}
} // namespace detail

bool aw_manual_reset_event::await_suspend(std::coroutine_handle<> Outer) noexcept {
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

std::coroutine_handle<>
aw_manual_reset_event_co_set::await_suspend(std::coroutine_handle<> Outer) noexcept {
  auto h = parent.head.exchange(manual_reset_event::READY, std::memory_order_acq_rel);
  if (manual_reset_event::READY == h || manual_reset_event::NOT_READY == h) {
    // It was ready, or there are no waiters - just resume
    return Outer;
  }

  // Save the first / most recently added waiter for symmetric transfer.
  auto toWake = reinterpret_cast<tmc::detail::waiter_list_node*>(h);

  // Wake the remaining waiters in batches, then count toWake itself, which is
  // woken below by symmetric transfer.
  wakeCount = 1 + tmc::detail::wake_waiters_in_batches(toWake->next);

  return toWake->waiter.try_symmetric_transfer(Outer);
}

size_t manual_reset_event::waiter_count() noexcept {
  auto h = head.load(std::memory_order_acquire);
  if (h == NOT_READY || h == READY) {
    return 0;
  }
  size_t count = 0;
  auto curr = reinterpret_cast<tmc::detail::waiter_list_node*>(h);
  while (curr != nullptr) {
    ++count;
    curr = curr->next;
  }
  return count;
}

size_t manual_reset_event::set() noexcept {
  auto h = head.exchange(READY, std::memory_order_acq_rel);
  if (READY == h) {
    // It was ready, nothing to wake
    return 0;
  }

  return tmc::detail::wake_waiters_in_batches(
    reinterpret_cast<tmc::detail::waiter_list_node*>(h)
  );
}

manual_reset_event::~manual_reset_event() noexcept { set(); }
} // namespace tmc
