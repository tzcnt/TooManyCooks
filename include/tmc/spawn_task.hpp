// Copyright (c) 2023-2025 Logan McDougall
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

namespace tmc {

template <
  typename Awaitable,
  typename Result = tmc::detail::get_awaitable_traits<Awaitable>::result_type>
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

// Primary template is forward-declared in "tmc/detail/aw_run_early.hpp".
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
    assert(!is_empty);
    is_empty = true; // signal that we initiated the work in some way
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
    assert(!is_empty);
    is_empty = true; // signal that we initiated the work in some way
#endif
    tmc::detail::initiate_one<Awaitable>(std::move(wrapped), executor, prio);
  }

#if !defined(NDEBUG)
  ~aw_spawned_task() noexcept {
    // This must be used, moved-from, or submitted for execution
    // in some way before destruction.
    assert(is_empty);
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
  inline aw_run_early<Awaitable> run_early() && {

#ifndef NDEBUG
    assert(!is_empty);
    is_empty = true; // signal that we initiated the work in some way
#endif
    return aw_run_early<Awaitable>(
      std::move(wrapped), executor, continuation_executor, prio
    );
  }
};

namespace detail {

template <typename Result> struct awaitable_traits<aw_spawned_task<Result>> {
  static constexpr configure_mode mode = WRAPPER;

  using result_type = Result;
  using self_type = aw_spawned_task<Result>;
  using awaiter_type = aw_spawned_task_impl<Result>;

  static awaiter_type get_awaiter(self_type&& Awaitable) {
    return std::forward<self_type>(Awaitable).operator co_await();
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
