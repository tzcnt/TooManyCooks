// Copyright (c) 2023-2024 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/aw_run_early.hpp"
#include "tmc/detail/concepts.hpp" // IWYU pragma: keep
#include "tmc/detail/mixins.hpp"
#include "tmc/detail/thread_locals.hpp"
#include "tmc/task.hpp"

#include <cassert>
#include <coroutine>
#include <tuple>

namespace tmc {

template <typename Result> class aw_spawned_task_impl;

template <typename Result> class aw_spawned_task_impl {
  task<Result> wrapped;
  tmc::detail::type_erased_executor* executor;
  tmc::detail::type_erased_executor* continuation_executor;
  size_t prio;
  Result result;
  friend aw_spawned_task<Result>;
  aw_spawned_task_impl(
    task<Result> Task, tmc::detail::type_erased_executor* Executor,
    tmc::detail::type_erased_executor* ContinuationExecutor, size_t Prio
  )
      : wrapped{std::move(Task)}, executor{Executor},
        continuation_executor{ContinuationExecutor}, prio{Prio} {}

public:
  /// Always suspends.
  inline bool await_ready() const noexcept { return false; }

  /// Suspends the outer coroutine, submits the wrapped task to the
  /// executor, and waits for it to complete.
  TMC_FORCE_INLINE inline void await_suspend(std::coroutine_handle<> Outer
  ) noexcept {
#ifndef TMC_TRIVIAL_TASK
    assert(wrapped);
#endif
    tmc::detail::set_continuation(wrapped, Outer.address());
    tmc::detail::set_continuation_executor(wrapped, continuation_executor);
    tmc::detail::set_result_ptr(wrapped, &result);
    tmc::detail::post_checked(executor, std::move(wrapped), prio);
  }

  /// Returns the value provided by the wrapped task.
  inline Result&& await_resume() noexcept { return std::move(result); }
};

template <> class aw_spawned_task_impl<void> {
  task<void> wrapped;
  tmc::detail::type_erased_executor* executor;
  tmc::detail::type_erased_executor* continuation_executor;
  size_t prio;
  friend aw_spawned_task<void>;

  inline aw_spawned_task_impl(
    task<void> Task, tmc::detail::type_erased_executor* Executor,
    tmc::detail::type_erased_executor* ContinuationExecutor, size_t Prio
  )
      : wrapped{std::move(Task)}, executor{Executor},
        continuation_executor{ContinuationExecutor}, prio{Prio} {}

public:
  /// Always suspends.
  inline bool await_ready() const noexcept { return false; }

  /// Suspends the outer coroutine, submits the wrapped task to the
  /// executor, and waits for it to complete.
  TMC_FORCE_INLINE inline void await_suspend(std::coroutine_handle<> Outer
  ) noexcept {
#ifndef TMC_TRIVIAL_TASK
    assert(wrapped);
#endif
    tmc::detail::set_continuation(wrapped, Outer.address());
    tmc::detail::set_continuation_executor(wrapped, continuation_executor);
    tmc::detail::post_checked(executor, std::move(wrapped), prio);
  }

  /// Does nothing.
  inline void await_resume() noexcept {}
};

// Primary template is forward-declared in "tmc/detail/aw_run_early.hpp".
template <typename Result>
class [[nodiscard("You must use the aw_spawned_task<Result> by one of: 1. "
                  "co_await 2. run_early()")]] aw_spawned_task
    : public tmc::detail::run_on_mixin<aw_spawned_task<Result>>,
      public tmc::detail::resume_on_mixin<aw_spawned_task<Result>>,
      public tmc::detail::with_priority_mixin<aw_spawned_task<Result>> {
  friend class tmc::detail::run_on_mixin<aw_spawned_task<Result>>;
  friend class tmc::detail::resume_on_mixin<aw_spawned_task<Result>>;
  friend class tmc::detail::with_priority_mixin<aw_spawned_task<Result>>;
  task<Result> wrapped;
  tmc::detail::type_erased_executor* executor;
  tmc::detail::type_erased_executor* continuation_executor;
  size_t prio;

public:
  /// It is recommended to call `spawn()` instead of using this constructor
  /// directly.
  aw_spawned_task(task<Result>&& Task)
      : wrapped(std::move(Task)), executor(tmc::detail::this_thread::executor),
        continuation_executor(tmc::detail::this_thread::executor),
        prio(tmc::detail::this_thread::this_task.prio) {}

  aw_spawned_task_impl<Result> operator co_await() && {
    return aw_spawned_task_impl<Result>(
      std::move(wrapped), executor, continuation_executor, prio
    );
  }

#if !defined(NDEBUG) && !defined(TMC_TRIVIAL_TASK)
  ~aw_spawned_task() noexcept {
    // If you spawn a task that returns a non-void type,
    // then you must co_await the return of spawn!
    assert(!wrapped);
  }
#endif
  aw_spawned_task(const aw_spawned_task&) = delete;
  aw_spawned_task& operator=(const aw_spawned_task&) = delete;
  aw_spawned_task(aw_spawned_task&& Other)
      : wrapped(std::move(Other.wrapped)), executor(std::move(Other.executor)),
        continuation_executor(std::move(Other.continuation_executor)),
        prio(Other.prio) {}
  aw_spawned_task& operator=(aw_spawned_task&& Other) {
    wrapped = std::move(Other.wrapped);
    executor = std::move(Other.executor);
    continuation_executor = std::move(Other.continuation_executor);
    prio = Other.prio;
    return *this;
  }

  /// Submits the wrapped task immediately, without suspending the current
  /// coroutine. You must await the return type before destroying it.
  inline aw_run_early<Result> run_early() && {
    return aw_run_early<Result>(
      std::move(wrapped), executor, continuation_executor, prio
    );
  }
};

template <>
class [[nodiscard(
  "You must use the aw_spawned_task<void> by one of: 1. co_await 2. "
  "detach() 3. run_early()"
)]] aw_spawned_task<void>
    : public tmc::detail::run_on_mixin<aw_spawned_task<void>>,
      public tmc::detail::resume_on_mixin<aw_spawned_task<void>>,
      public tmc::detail::with_priority_mixin<aw_spawned_task<void>> {
  friend class tmc::detail::run_on_mixin<aw_spawned_task<void>>;
  friend class tmc::detail::resume_on_mixin<aw_spawned_task<void>>;
  friend class tmc::detail::with_priority_mixin<aw_spawned_task<void>>;
  task<void> wrapped;
  tmc::detail::type_erased_executor* executor;
  tmc::detail::type_erased_executor* continuation_executor;
  size_t prio;

public:
  /// It is recommended to call `spawn()` instead of using this constructor
  /// directly.
  aw_spawned_task(task<void>&& Task)
      : wrapped(std::move(Task)), executor(tmc::detail::this_thread::executor),
        continuation_executor(tmc::detail::this_thread::executor),
        prio(tmc::detail::this_thread::this_task.prio) {}

  aw_spawned_task_impl<void> operator co_await() && {
    return aw_spawned_task_impl<void>(
      std::move(wrapped), executor, continuation_executor, prio
    );
  }

  /// Submits the wrapped task to the executor immediately. It cannot be awaited
  /// afterward.
  void detach() {
#ifndef TMC_TRIVIAL_TASK
    assert(wrapped);
#endif
    tmc::detail::post_checked(executor, std::move(wrapped), prio);
  }

#if !defined(NDEBUG) && !defined(TMC_TRIVIAL_TASK)
  ~aw_spawned_task() noexcept {
    // If you spawn a task that returns a void type,
    // then you must co_await or detach the return of spawn!
    assert(!wrapped);
  }
#endif
  aw_spawned_task(const aw_spawned_task&) = delete;
  aw_spawned_task& operator=(const aw_spawned_task&) = delete;
  aw_spawned_task(aw_spawned_task&& Other)
      : wrapped(std::move(Other.wrapped)), executor(std::move(Other.executor)),
        continuation_executor(std::move(Other.continuation_executor)),
        prio(Other.prio) {}
  aw_spawned_task& operator=(aw_spawned_task&& Other) {
    wrapped = std::move(Other.wrapped);
    executor = std::move(Other.executor);
    continuation_executor = std::move(Other.continuation_executor);
    prio = Other.prio;
    return *this;
  }

  /// Submits the wrapped task to the executor immediately, without suspending
  /// the current coroutine. You must await the returned before destroying it.
  inline aw_run_early<void> run_early() && {
    return aw_run_early<void>(
      std::move(wrapped), executor, continuation_executor, prio
    );
  }
};

/// `spawn()` allows you to customize the execution behavior of a task.
///
/// Before the task is submitted for execution, you may call any or all of
/// `run_on()`, `resume_on()`, `with_priority()`. The task must then be
/// submitted for execution by calling exactly one of: `co_await`, `run_early()`
/// or `detach()`.
template <typename Result> aw_spawned_task<Result> spawn(task<Result>&& Task) {
  return aw_spawned_task<Result>(std::move(Task));
}

} // namespace tmc
