// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/compat.hpp"
#include "tmc/detail/concepts.hpp" // IWYU pragma: keep
#include "tmc/detail/mixins.hpp"
#include "tmc/detail/thread_locals.hpp"
#include "tmc/task.hpp"

#include <cassert>
#include <coroutine>

namespace tmc {

/// The customizable task wrapper / awaitable type returned by
/// `tmc::spawn()`.
template <
  typename Awaitable,
  typename Result = tmc::detail::awaitable_result_t<Awaitable>>
class aw_spawned_task;

/// A wrapper that converts lazy task(s) to eager task(s),
/// and allows the task(s) to be awaited after it has been started.
/// It is created by calling `run_early()` on a parent awaitable
/// from `spawn()`.
///
/// `Result` is the type of a single result value.
template <
  typename Awaitable,
  typename Result = tmc::detail::awaitable_result_t<Awaitable>>
class [[nodiscard("You must co_await aw_run_early. "
                  "It is not safe to destroy aw_run_early without first "
                  "awaiting it.")]] aw_run_early_impl;

template <typename Awaitable, typename Result> class aw_run_early_impl {
  std::coroutine_handle<> continuation;
  tmc::detail::type_erased_executor* continuation_executor;
  std::atomic<ptrdiff_t> done_count;
  tmc::detail::result_storage_t<Result> result;

  using AwaitableTraits = tmc::detail::get_awaitable_traits<Awaitable>;

  friend class aw_spawned_task<Awaitable>;

  // Private constructor from aw_spawned_task. Takes ownership of parent's
  // task.
  aw_run_early_impl(
    Awaitable&& Task, tmc::detail::type_erased_executor* Executor,
    tmc::detail::type_erased_executor* ContinuationExecutor, size_t Priority
  )
      : continuation{nullptr}, continuation_executor(ContinuationExecutor),
        done_count(1) {
    AwaitableTraits::set_continuation(Task, &continuation);
    AwaitableTraits::set_continuation_executor(Task, &continuation_executor);
    AwaitableTraits::set_done_count(Task, &done_count);
    AwaitableTraits::set_result_ptr(Task, &result);
    tmc::detail::initiate_one<Awaitable>(std::move(Task), Executor, Priority);
  }

public:
  /// Always suspends.
  inline bool await_ready() const noexcept { return false; }

  /// Suspends the outer coroutine, submits the wrapped task to the
  /// executor, and waits for it to complete.
  TMC_FORCE_INLINE inline bool await_suspend(std::coroutine_handle<> Outer
  ) noexcept {
    continuation = Outer;
    auto remaining = done_count.fetch_sub(1, std::memory_order_acq_rel);
    // Worker was already posted.
    // Suspend if remaining > 0 (worker is still running)
    if (remaining > 0) {
      return true;
    }
    // Resume if remaining <= 0 (worker already finished)
    if (continuation_executor == nullptr ||
        tmc::detail::this_thread::exec_is(continuation_executor)) {
      return false;
    } else {
      // Need to resume on a different executor
      tmc::detail::post_checked(
        continuation_executor, std::move(Outer),
        tmc::detail::this_thread::this_task.prio, TMC_ALL_ONES
      );
      return true;
    }
  }

  /// Returns the value provided by the wrapped function.
  inline Result&& await_resume() noexcept {
    if constexpr (std::is_default_constructible_v<Result>) {
      return std::move(result);
    } else {
      return *std::move(result);
    }
  }

  // This must be awaited and the child task completed before destruction.
#ifndef NDEBUG
  ~aw_run_early_impl() noexcept {
    assert(
      done_count.load() < 0 && "You must co_await the result of run_early()."
    );
  }
#endif

  // Not movable or copyable due to child task being spawned in constructor,
  // and having pointers to this.
  aw_run_early_impl& operator=(const aw_run_early_impl& other) = delete;
  aw_run_early_impl(const aw_run_early_impl& other) = delete;
  aw_run_early_impl& operator=(const aw_run_early_impl&& other) = delete;
  aw_run_early_impl(const aw_run_early_impl&& other) = delete;
};

template <typename Awaitable> class aw_run_early_impl<Awaitable, void> {
  std::coroutine_handle<> continuation;
  tmc::detail::type_erased_executor* continuation_executor;
  std::atomic<ptrdiff_t> done_count;

  using AwaitableTraits = tmc::detail::get_awaitable_traits<Awaitable>;

  friend class aw_spawned_task<Awaitable>;

  // Private constructor from aw_spawned_task. Takes ownership of parent's
  // task.
  aw_run_early_impl(
    Awaitable&& Task, tmc::detail::type_erased_executor* Executor,
    tmc::detail::type_erased_executor* ContinuationExecutor, size_t Priority
  )
      : continuation{nullptr}, continuation_executor(ContinuationExecutor),
        done_count(1) {
    AwaitableTraits::set_continuation(Task, &continuation);
    AwaitableTraits::set_continuation_executor(Task, &continuation_executor);
    AwaitableTraits::set_done_count(Task, &done_count);
    tmc::detail::initiate_one<Awaitable>(std::move(Task), Executor, Priority);
  }

public:
  inline bool await_ready() const noexcept { return false; }

  /// Suspends the outer coroutine, submits the wrapped task to the
  /// executor, and waits for it to complete.
  TMC_FORCE_INLINE inline bool await_suspend(std::coroutine_handle<> Outer
  ) noexcept {
    continuation = Outer;
    auto remaining = done_count.fetch_sub(1, std::memory_order_acq_rel);
    // Worker was already posted.
    // Suspend if remaining > 0 (worker is still running)
    if (remaining > 0) {
      return true;
    }
    // Resume if remaining <= 0 (worker already finished)
    if (continuation_executor == nullptr ||
        tmc::detail::this_thread::exec_is(continuation_executor)) {
      return false;
    } else {
      // Need to resume on a different executor
      tmc::detail::post_checked(
        continuation_executor, std::move(Outer),
        tmc::detail::this_thread::this_task.prio, TMC_ALL_ONES
      );
      return true;
    }
  }

  /// Does nothing.
  inline void await_resume() noexcept {}

// This must be awaited and the child task completed before destruction.
#ifndef NDEBUG
  ~aw_run_early_impl() noexcept {
    assert(done_count.load() < 0 && "You must submit or co_await this.");
  }
#endif

  // Not movable or copyable due to child task being spawned in constructor,
  // and having pointers to this.
  aw_run_early_impl& operator=(const aw_run_early_impl& Other) = delete;
  aw_run_early_impl(const aw_run_early_impl& Other) = delete;
  aw_run_early_impl& operator=(const aw_run_early_impl&& Other) = delete;
  aw_run_early_impl(const aw_run_early_impl&& Other) = delete;
};

template <typename Awaitable>
using aw_run_early =
  tmc::detail::rvalue_only_awaitable<aw_run_early_impl<Awaitable>>;

template <
  typename Awaitable,
  typename Result = tmc::detail::awaitable_result_t<Awaitable>>
class aw_spawned_task_impl;

template <typename Awaitable, typename Result> class aw_spawned_task_impl {
  Awaitable wrapped;
  tmc::detail::type_erased_executor* executor;
  tmc::detail::type_erased_executor* continuation_executor;
  size_t prio;

  struct empty {};
  using ResultStorage = std::conditional_t<
    std::is_void_v<Result>, empty, tmc::detail::result_storage_t<Result>>;
  TMC_NO_UNIQUE_ADDRESS ResultStorage result;

  friend aw_spawned_task<Awaitable>;

  aw_spawned_task_impl(
    Awaitable Task, tmc::detail::type_erased_executor* Executor,
    tmc::detail::type_erased_executor* ContinuationExecutor, size_t Prio
  )
      : wrapped{std::move(Task)}, executor{Executor},
        continuation_executor{ContinuationExecutor}, prio{Prio} {}

  template <typename T>
  TMC_FORCE_INLINE inline void
  initiate(T&& Task, std::coroutine_handle<> Outer) {
    tmc::detail::get_awaitable_traits<T>::set_continuation(
      Task, Outer.address()
    );
    tmc::detail::get_awaitable_traits<T>::set_continuation_executor(
      Task, continuation_executor
    );
    if constexpr (!std::is_void_v<Result>) {
      tmc::detail::get_awaitable_traits<T>::set_result_ptr(Task, &result);
    }
    tmc::detail::initiate_one<T>(std::move(Task), executor, prio);
  }

public:
  /// Always suspends.
  inline bool await_ready() const noexcept { return false; }

  /// Suspends the outer coroutine, submits the wrapped task to the
  /// executor, and waits for it to complete.
  TMC_FORCE_INLINE inline void await_suspend(std::coroutine_handle<> Outer
  ) noexcept {
    if constexpr (tmc::detail::get_awaitable_traits<Awaitable>::mode ==
                  tmc::detail::WRAPPER) {
      initiate(tmc::detail::safe_wrap(std::move(wrapped)), Outer);
    } else {
      initiate(std::move(wrapped), Outer);
    }
  }

  /// Returns the value provided by the wrapped task.
  inline std::add_rvalue_reference_t<Result> await_resume() noexcept
    requires(!std::is_void_v<Result>)
  {
    if constexpr (std::is_default_constructible_v<Result>) {
      return std::move(result);
    } else {
      return *std::move(result);
    }
  }

  /// Does nothing.
  inline void await_resume() noexcept
    requires(std::is_void_v<Result>)
  {}
};

template <typename Awaitable, typename Result>
class [[nodiscard("You must await or initiate the result of spawn()."
)]] aw_spawned_task
    : public tmc::detail::run_on_mixin<aw_spawned_task<Awaitable, Result>>,
      public tmc::detail::resume_on_mixin<aw_spawned_task<Awaitable, Result>>,
      public tmc::detail::with_priority_mixin<
        aw_spawned_task<Awaitable, Result>> {
  friend class tmc::detail::run_on_mixin<aw_spawned_task<Awaitable, Result>>;
  friend class tmc::detail::resume_on_mixin<aw_spawned_task<Awaitable, Result>>;
  friend class tmc::detail::with_priority_mixin<
    aw_spawned_task<Awaitable, Result>>;
  Awaitable wrapped;
  tmc::detail::type_erased_executor* executor;
  tmc::detail::type_erased_executor* continuation_executor;
  size_t prio;

#ifndef NDEBUG
  bool is_empty;
#endif

public:
  /// It is recommended to call `spawn()` instead of using this constructor
  /// directly.
  aw_spawned_task(Awaitable&& Task)
      : wrapped(std::move(Task)), executor(tmc::detail::this_thread::executor),
        continuation_executor(tmc::detail::this_thread::executor),
        prio(tmc::detail::this_thread::this_task.prio)
#ifndef NDEBUG
        ,
        is_empty(false)
#endif
  {
  }

  aw_spawned_task_impl<Awaitable> operator co_await() && {

#ifndef NDEBUG
    assert(!is_empty && "You may only submit or co_await this once.");
    is_empty = true;
#endif
    return aw_spawned_task_impl<Awaitable>(
      std::move(wrapped), executor, continuation_executor, prio
    );
  }

  /// Submits the wrapped task to the executor immediately. It cannot be awaited
  /// afterward.
  void detach()
    requires(!std::is_void_v<Awaitable>)
  {
#ifndef NDEBUG
    assert(!is_empty && "You may only submit or co_await this once.");
    is_empty = true;
#endif
    tmc::detail::initiate_one<Awaitable>(std::move(wrapped), executor, prio);
  }

#if !defined(NDEBUG)
  ~aw_spawned_task() noexcept {
    // This must be used, moved-from, or submitted for execution
    // in some way before destruction.
    assert(is_empty && "You must submit or co_await this.");
  }
#endif
  aw_spawned_task(const aw_spawned_task&) = delete;
  aw_spawned_task& operator=(const aw_spawned_task&) = delete;
  aw_spawned_task(aw_spawned_task&& Other)
      : wrapped(std::move(Other.wrapped)), executor(std::move(Other.executor)),
        continuation_executor(std::move(Other.continuation_executor)),
        prio(Other.prio) {
#if !defined(NDEBUG)
    is_empty = Other.is_empty;
    Other.is_empty = true;
#endif
  }
  aw_spawned_task& operator=(aw_spawned_task&& Other) {
    wrapped = std::move(Other.wrapped);
    executor = std::move(Other.executor);
    continuation_executor = std::move(Other.continuation_executor);
    prio = Other.prio;
#if !defined(NDEBUG)
    is_empty = Other.is_empty;
    Other.is_empty = true;
#endif
    return *this;
  }

  /// Submits the wrapped task immediately, without suspending the current
  /// coroutine. You must await the return type before destroying it.
  [[nodiscard("You must co_await the result of run_early()."
  )]] inline aw_run_early<Awaitable>
  run_early() && {

#ifndef NDEBUG
    assert(!is_empty && "You may only submit or co_await this once.");
    is_empty = true;
#endif
    return aw_run_early<Awaitable>(
      std::move(wrapped), executor, continuation_executor, prio
    );
  }
};

namespace detail {

template <typename Awaitable, typename Result>
struct awaitable_traits<aw_spawned_task<Awaitable, Result>> {
  static constexpr configure_mode mode = WRAPPER;

  using result_type = Result;
  using self_type = aw_spawned_task<Awaitable, Result>;
  using awaiter_type = aw_spawned_task_impl<Awaitable, Result>;

  static awaiter_type get_awaiter(self_type&& awaitable) {
    return std::forward<self_type>(awaitable).operator co_await();
  }
};

} // namespace detail

/// `spawn()` allows you to customize the execution behavior of a task or
/// awaitable.
///
/// Before the task is submitted for execution, you may call any or all of
/// `run_on()`, `resume_on()`, `with_priority()`. The task must then be
/// submitted for execution by calling exactly one of: `co_await`, `run_early()`
/// or `detach()`.
template <typename Awaitable>
aw_spawned_task<Awaitable> spawn(Awaitable&& Task) {
  return aw_spawned_task<Awaitable>(std::move(Task));
}

} // namespace tmc
