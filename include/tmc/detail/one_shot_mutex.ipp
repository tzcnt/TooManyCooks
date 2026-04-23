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

[[nodiscard]] static one_shot_mutex_waiter*
one_shot_mutex_waiters(uintptr_t Value) noexcept {
  return reinterpret_cast<one_shot_mutex_waiter*>(
    Value & ~one_shot_mutex_state::RUNNING
  );
}

static void release_one_shot_mutex_state(one_shot_mutex_state* State) noexcept {
  if (State->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    assert(State->waiters.load(std::memory_order_acquire) == 0);
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
  uintptr_t head = State->waiters.load(std::memory_order_acquire);
  uintptr_t desired;
  do {
    Waiter.next = tmc::detail::one_shot_mutex_waiters(head);
    desired = reinterpret_cast<uintptr_t>(&Waiter) |
              tmc::detail::one_shot_mutex_state::RUNNING;
  } while (!State->waiters.compare_exchange_strong(
    head, desired, std::memory_order_acq_rel, std::memory_order_acquire
  ));

  if (tmc::detail::one_shot_mutex_running(head)) {
    return std::noop_coroutine();
  }

  // No runner existed prior to the CAS above, so this await_suspend path
  // cannot be resumed until we return the new runner handle.
  return static_cast<std::coroutine_handle<>>(run_loop(State));
}

tmc::task<void>
one_shot_mutex::run_loop(tmc::detail::one_shot_mutex_state* State) {
  State->refs.fetch_add(1, std::memory_order_relaxed);
  while (true) {
    auto stateWord = State->waiters.exchange(
      tmc::detail::one_shot_mutex_state::RUNNING, std::memory_order_acq_rel
    );
    auto* curr = tmc::detail::one_shot_mutex_waiters(stateWord);
    if (curr == nullptr) {
      uintptr_t expected = tmc::detail::one_shot_mutex_state::RUNNING;
      if (State->waiters.compare_exchange_strong(
            expected, 0, std::memory_order_acq_rel, std::memory_order_acquire
          )) {
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
  // 0 (no waiters) or 1 (this is the running task) are valid
  assert(State->waiters.load(std::memory_order_acquire) < 2);
  tmc::detail::release_one_shot_mutex_state(State);
}
} // namespace tmc
