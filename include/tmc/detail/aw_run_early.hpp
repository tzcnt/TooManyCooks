// Copyright (c) 2023-2024 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/mixins.hpp"
#include "tmc/detail/thread_locals.hpp"
#include "tmc/task.hpp"

#include <cassert>
#include <coroutine>

namespace tmc {
// Forward declared friend class.
// Defined in "tmc/spawn_task.hpp" which includes this header.

/// The customizable task wrapper / awaitable type returned by
/// `tmc::spawn(tmc::task<Result>)`.
template <typename Result> class aw_spawned_task;

/// The customizable task wrapper / awaitable type returned by
/// `tmc::spawn_tuple(tmc::task<Result>...)`.
template <typename... Result> class aw_spawned_task_tuple;

/// A wrapper that converts lazy task(s) to eager task(s),
/// and allows the task(s) to be awaited after it has been started.
/// It is created by calling `run_early()` on a parent awaitable
/// from `spawn()`.
///
/// `Result` is the type of a single result value (same as that of the wrapped
/// `task<Result>`).
template <typename Result>
class [[nodiscard("You must co_await aw_run_early. "
                  "It is not safe to destroy aw_run_early without first "
                  "awaiting it.")]] aw_run_early_impl;

template <typename Result> class aw_run_early_impl {
  friend class aw_spawned_task<Result>;
  detail::type_erased_executor* continuation_executor;
  std::coroutine_handle<> continuation;
  std::atomic<int64_t> done_count;
  Result result;

  // Private constructor from aw_spawned_task. Takes ownership of parent's
  // task.
  aw_run_early_impl(
    task<Result>&& Task, detail::type_erased_executor* Executor,
    detail::type_erased_executor* ContinuationExecutor, size_t Priority
  )
      : continuation{nullptr}, continuation_executor(ContinuationExecutor),
        done_count(1) {
    detail::set_continuation(Task, &continuation);
    detail::set_continuation_executor(Task, &continuation_executor);
    detail::set_done_count(Task, &done_count);
    detail::set_result_ptr(Task, &result);
    // TODO fence maybe not required if there's one inside the queue?
    std::atomic_thread_fence(std::memory_order_release);
    detail::post_checked(Executor, std::move(Task), Priority);
  }

public:
  /// Always suspends.
  inline bool await_ready() const noexcept { return false; }

  /// Suspends the outer coroutine, submits the wrapped task to the
  /// executor, and waits for it to complete.
  TMC_FORCE_INLINE inline bool await_suspend(std::coroutine_handle<> Outer
  ) noexcept {
    // For unknown reasons, it doesn't work to start with done_count at 0,
    // Then increment here and check before storing continuation...
    continuation = Outer;
    auto remaining = done_count.fetch_sub(1, std::memory_order_acq_rel);
    // Worker was already posted.
    // Suspend if remaining > 0 (worker is still running)
    if (remaining > 0) {
      return true;
    }
    // Resume if remaining <= 0 (worker already finished)
    if (continuation_executor == nullptr ||
        detail::this_thread::exec_is(continuation_executor)) {
      return false;
    } else {
      // Need to resume on a different executor
      detail::post_checked(
        continuation_executor, std::move(Outer),
        detail::this_thread::this_task.prio
      );
      return true;
    }
  }

  /// Returns the value provided by the wrapped function.
  inline Result&& await_resume() noexcept { return std::move(result); }

  // This must be awaited and the child task completed before destruction.
#ifndef NDEBUG
  ~aw_run_early_impl() noexcept { assert(done_count.load() < 0); }
#endif

  // Not movable or copyable due to child task being spawned in constructor,
  // and having pointers to this.
  aw_run_early_impl& operator=(const aw_run_early_impl& other) = delete;
  aw_run_early_impl(const aw_run_early_impl& other) = delete;
  aw_run_early_impl& operator=(const aw_run_early_impl&& other) = delete;
  aw_run_early_impl(const aw_run_early_impl&& other) = delete;
};

template <> class aw_run_early_impl<void> {
  friend class aw_spawned_task<void>;
  detail::type_erased_executor* continuation_executor;
  std::atomic<int64_t> done_count;
  std::coroutine_handle<> continuation;

  // Private constructor from aw_spawned_task. Takes ownership of parent's
  // task.
  aw_run_early_impl(
    task<void>&& Task, detail::type_erased_executor* Executor,
    detail::type_erased_executor* ContinuationExecutor, size_t Priority
  )
      : continuation{nullptr}, continuation_executor(ContinuationExecutor),
        done_count(1) {
    detail::set_continuation(Task, &continuation);
    detail::set_continuation_executor(Task, &continuation_executor);
    detail::set_done_count(Task, &done_count);
    // TODO fence maybe not required if there's one inside the queue?
    std::atomic_thread_fence(std::memory_order_release);
    detail::post_checked(Executor, std::move(Task), Priority);
  }

public:
  inline bool await_ready() const noexcept { return false; }

  /// Suspends the outer coroutine, submits the wrapped task to the
  /// executor, and waits for it to complete.
  TMC_FORCE_INLINE inline bool await_suspend(std::coroutine_handle<> Outer
  ) noexcept {
    // For unknown reasons, it doesn't work to start with done_count at 0,
    // Then increment here and check before storing continuation...
    continuation = Outer;
    auto remaining = done_count.fetch_sub(1, std::memory_order_acq_rel);
    // Worker was already posted.
    // Suspend if remaining > 0 (worker is still running)
    if (remaining > 0) {
      return true;
    }
    // Resume if remaining <= 0 (worker already finished)
    if (continuation_executor == nullptr ||
        detail::this_thread::exec_is(continuation_executor)) {
      return false;
    } else {
      // Need to resume on a different executor
      detail::post_checked(
        continuation_executor, std::move(Outer),
        detail::this_thread::this_task.prio
      );
      return true;
    }
  }

  /// Does nothing.
  inline void await_resume() noexcept {}

// This must be awaited and the child task completed before destruction.
#ifndef NDEBUG
  ~aw_run_early_impl() noexcept { assert(done_count.load() < 0); }
#endif

  // Not movable or copyable due to child task being spawned in constructor,
  // and having pointers to this.
  aw_run_early_impl& operator=(const aw_run_early_impl& Other) = delete;
  aw_run_early_impl(const aw_run_early_impl& Other) = delete;
  aw_run_early_impl& operator=(const aw_run_early_impl&& Other) = delete;
  aw_run_early_impl(const aw_run_early_impl&& Other) = delete;
};

template <typename Result>
using aw_run_early = detail::rvalue_only_awaitable<aw_run_early_impl<Result>>;

} // namespace tmc
