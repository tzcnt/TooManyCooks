// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/impl.hpp" // IWYU pragma: keep

#include "tmc/aw_resume_on.hpp"
#include "tmc/detail/thread_locals.hpp"
#include "tmc/one_shot_mutex.hpp"

#include <atomic>
#include <cassert>
#include <coroutine>

namespace tmc {
namespace detail {

void release_one_shot_mutex_state(one_shot_mutex_state* State) noexcept {
  if (State->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    assert(State->waiters.load(std::memory_order_acquire) == nullptr);
    assert(!State->running.load(std::memory_order_acquire));
    delete State;
  }
}
} // namespace detail

std::coroutine_handle<>
aw_one_shot_mutex_lock::await_suspend(std::coroutine_handle<> Outer) noexcept {
  auto* mutex = parent.load(std::memory_order_acquire);
  me.continuation = Outer;
  me.continuation_executor = tmc::detail::this_thread::executor();
  me.continuation_priority = tmc::detail::this_thread::this_task().prio;
  return mutex->enqueue(me);
}

std::coroutine_handle<>
one_shot_mutex::enqueue(tmc::detail::one_shot_mutex_waiter& Waiter) noexcept {
  auto* State = state;
  // The awaiting coroutine can be resumed immediately once it is queued, which
  // means the wrapper may be destroyed before this await_suspend path fully
  // unwinds. Take a provisional ref before mutating State so it stays alive
  // until we either hand the ref to a new runner or drop it back.
  State->refs.fetch_add(1, std::memory_order_relaxed);

  auto* head = State->waiters.load(std::memory_order_acquire);
  do {
    Waiter.next = head;
  } while (!State->waiters.compare_exchange_strong(
    head, &Waiter, std::memory_order_acq_rel, std::memory_order_acquire
  ));

  if (State->running.exchange(true, std::memory_order_acq_rel)) {
    tmc::detail::release_one_shot_mutex_state(State);
    return std::noop_coroutine();
  }

  return static_cast<std::coroutine_handle<>>(run_loop(State));
}

tmc::task<void>
one_shot_mutex::run_loop(tmc::detail::one_shot_mutex_state* State) {
  while (true) {
    auto* curr = State->waiters.exchange(nullptr, std::memory_order_acq_rel);
    if (curr == nullptr) {
      State->running.store(false, std::memory_order_release);
      if (State->waiters.load(std::memory_order_acquire) == nullptr ||
          State->running.exchange(true, std::memory_order_acq_rel)) {
        break;
      }
      continue;
    }

    while (curr != nullptr) {
      auto* next = curr->next;
      auto continuation = curr->continuation;
      tmc::ex_any* continuationExecutor = curr->continuation_executor;
      auto continuationPriority = curr->continuation_priority;

      co_await tmc::resume_on(continuationExecutor)
        .with_priority(continuationPriority);

      continuation.resume();
      curr = next;
    }
  }
  tmc::detail::release_one_shot_mutex_state(State);
}

one_shot_mutex::~one_shot_mutex() {
  auto* State = state;
  assert(State->waiters.load(std::memory_order_acquire) == nullptr);
  tmc::detail::release_one_shot_mutex_state(State);
}
} // namespace tmc
