// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/compat.hpp"
#include "tmc/detail/concepts_awaitable.hpp" // IWYU pragma: keep
#include "tmc/detail/concepts_work_item.hpp"
#include "tmc/detail/mixins.hpp"
#include "tmc/detail/task_unsafe.hpp"
#include "tmc/detail/thread_locals.hpp"
#include "tmc/ex_any.hpp"
#include "tmc/work_item.hpp"

#include <cassert>
#include <coroutine>
#include <functional>
#include <type_traits>

namespace tmc {

/// The customizable task wrapper / awaitable type returned by
/// `tmc::spawn(std::function)`.
template <typename Result> class aw_spawn_func;

template <typename Result> class aw_spawn_func_impl {
  std::function<Result()> wrapped;
  tmc::ex_any* executor;
  tmc::ex_any* continuation_executor;
  size_t prio;

  struct empty {};
  using ResultStorage = std::conditional_t<
    std::is_void_v<Result>, empty, tmc::detail::result_storage_t<Result>>;
  TMC_NO_UNIQUE_ADDRESS ResultStorage result;

  using AwaitableTraits =
    tmc::detail::get_awaitable_traits<tmc::detail::task_unsafe<Result>>;

  friend aw_spawn_func<Result>;

  aw_spawn_func_impl(
    std::function<Result()> Func, tmc::ex_any* Executor,
    tmc::ex_any* ContinuationExecutor, size_t Prio
  )
      : wrapped(std::move(Func)), executor(Executor),
        continuation_executor(ContinuationExecutor), prio(Prio) {}

public:
  /// Always suspends.
  inline bool await_ready() const noexcept {
    // Always suspends, due to the possibility to resume on another executor.
    return false;
  }

  /// Suspends the outer coroutine, submits the wrapped task to the
  /// executor, and waits for it to complete.
  inline void await_suspend(std::coroutine_handle<> Outer) noexcept {
#if TMC_WORK_ITEM_IS(CORO)
    tmc::detail::task_unsafe<Result> t(tmc::detail::into_task(wrapped));
    AwaitableTraits::set_continuation(t, Outer.address());
    AwaitableTraits::set_continuation_executor(t, continuation_executor);
    if constexpr (!std::is_void_v<Result>) {
      AwaitableTraits::set_result_ptr(t, &result);
    }
    tmc::detail::post_checked(executor, std::move(t), prio);
#else
    tmc::detail::post_checked(
      executor,
      [this, Outer,
       ContinuationPrio = tmc::detail::this_thread::this_task.prio]() mutable {
        if constexpr (std::is_void_v<Result>) {
          wrapped();
        } else {
          result = wrapped();
        }
        if (continuation_executor == nullptr ||
            tmc::detail::this_thread::exec_prio_is(
              continuation_executor, ContinuationPrio
            )) {
          Outer.resume();
        } else {
          tmc::detail::post_checked(
            continuation_executor, std::move(Outer), ContinuationPrio
          );
        }
      },
      prio
    );
#endif
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

template <typename Result> class aw_spawn_func_fork_impl {
  std::coroutine_handle<> continuation;
  tmc::ex_any* continuation_executor;
  std::atomic<ptrdiff_t> done_count;

  struct empty {};
  using ResultStorage = std::conditional_t<
    std::is_void_v<Result>, empty, tmc::detail::result_storage_t<Result>>;
  TMC_NO_UNIQUE_ADDRESS ResultStorage result;

  using AwaitableTraits =
    tmc::detail::get_awaitable_traits<tmc::detail::task_unsafe<Result>>;

  friend aw_spawn_func<Result>;

  aw_spawn_func_fork_impl(
    std::function<Result()> Func, tmc::ex_any* Executor,
    tmc::ex_any* ContinuationExecutor, size_t Prio
  )
      : continuation_executor(ContinuationExecutor) {
#if TMC_WORK_ITEM_IS(CORO)
    tmc::detail::task_unsafe<Result> t(tmc::detail::into_task(Func));
    AwaitableTraits::set_continuation(t, &continuation);
    AwaitableTraits::set_continuation_executor(t, &continuation_executor);
    AwaitableTraits::set_done_count(t, &done_count);
    if constexpr (!std::is_void_v<Result>) {
      AwaitableTraits::set_result_ptr(t, &result);
    }
    done_count.store(1, std::memory_order_release);
    tmc::detail::post_checked(Executor, std::move(t), Prio);
#else
    done_count.store(1, std::memory_order_release);
    tmc::detail::post_checked(
      Executor,
      [this, Func,
       ContinuationPrio = tmc::detail::this_thread::this_task.prio]() mutable {
        if constexpr (std::is_void_v<Result>) {
          Func();
        } else {
          result = Func();
        }

        tmc::detail::awaitable_customizer<Result> customizer;
        customizer.continuation = &continuation;
        customizer.continuation_executor = &continuation_executor;
        customizer.done_count = &done_count;
        // Cannot rely on the thread-local value of prio, as this is a deferred
        // construction
        customizer.flags = ContinuationPrio;

        std::coroutine_handle<> continuation = customizer.resume_continuation();
        if (continuation != std::noop_coroutine()) {
          continuation.resume();
        }
      },
      Prio
    );
#endif
  }

public:
  /// Always suspends.
  inline bool await_ready() const noexcept {
    // Always suspends, due to the possibility to resume on another executor.
    return false;
  }

  /// Suspends the outer coroutine, submits the wrapped task to the
  /// executor, and waits for it to complete.
  inline bool await_suspend(std::coroutine_handle<> Outer) noexcept {
#ifndef NDEBUG
    assert(done_count.load() >= 0 && "You may only co_await this once.");
#endif
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
        tmc::detail::this_thread::this_task.prio
      );
      return true;
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

  // This must be awaited and the child task completed before destruction.
#ifndef NDEBUG
  ~aw_spawn_func_fork_impl() noexcept {
    assert(
      done_count.load() < 0 &&
      "You must co_await the fork() awaitable before it goes out of scope."
    );
  }
#endif

  // Not movable or copyable due to child task being spawned in constructor,
  // and having pointers to this.
  aw_spawn_func_fork_impl& operator=(const aw_spawn_func_fork_impl& other
  ) = delete;
  aw_spawn_func_fork_impl(const aw_spawn_func_fork_impl& other) = delete;
  aw_spawn_func_fork_impl& operator=(const aw_spawn_func_fork_impl&& other
  ) = delete;
  aw_spawn_func_fork_impl(const aw_spawn_func_fork_impl&& other) = delete;
};

template <typename Result>
using aw_spawn_func_fork =
  tmc::detail::rvalue_only_awaitable<aw_spawn_func_fork_impl<Result>>;

/// Wraps a function into a new task by `std::bind`ing the Func to its Args,
/// and wrapping them into a type that allows you to customize the task
/// behavior before submitting it for execution.
///
/// Before the task is submitted for execution, you may call any or all of
/// `run_on()`, `resume_on()`, `with_priority()`. The task must then be
/// submitted for execution by calling exactly one of: `co_await`,
/// `fork()` or `detach()`.
template <typename Func, typename... Arguments>
auto spawn_func(Func&& func, Arguments&&... args)
  -> aw_spawn_func<decltype(func(args...))> {
  return aw_spawn_func<decltype(func(args...))>(
    std::bind(static_cast<Func&&>(func), static_cast<Arguments&&>(args)...)
  );
}

template <typename Result>
class [[nodiscard("You must await or initiate the result of spawn_func()."
)]] aw_spawn_func
    : public tmc::detail::run_on_mixin<aw_spawn_func<Result>>,
      public tmc::detail::resume_on_mixin<aw_spawn_func<Result>>,
      public tmc::detail::with_priority_mixin<aw_spawn_func<Result>> {
  friend class tmc::detail::run_on_mixin<aw_spawn_func<Result>>;
  friend class tmc::detail::resume_on_mixin<aw_spawn_func<Result>>;
  friend class tmc::detail::with_priority_mixin<aw_spawn_func<Result>>;
  std::function<Result()> wrapped;
  tmc::ex_any* executor;
  tmc::ex_any* continuation_executor;
  size_t prio;
#ifndef NDEBUG
  bool is_empty;
#endif

  friend class aw_spawn_func_impl<Result>;

public:
  /// It is recommended to call `spawn()` instead of using this constructor
  /// directly.
  aw_spawn_func(std::function<Result()>&& Func)
      : wrapped(std::move(Func)), executor(tmc::detail::this_thread::executor),
        continuation_executor(tmc::detail::this_thread::executor),
        prio(tmc::detail::this_thread::this_task.prio)
#ifndef NDEBUG
        ,
        is_empty(false)
#endif
  {
  }

  aw_spawn_func_impl<Result> operator co_await() && noexcept {
#ifndef NDEBUG
    assert(!is_empty && "You may only submit or co_await this once.");
    is_empty = true;
#endif
    return aw_spawn_func_impl<Result>(
      wrapped, executor, continuation_executor, prio
    );
  }

  /// Submits the task to the executor immediately, without suspending the
  /// current coroutine. You must join the forked task by awaiting the returned
  /// awaitable before it goes out of scope.
  [[nodiscard(
    "You must co_await the fork() awaitable before it goes out of scope."
  )]] aw_spawn_func_fork<Result>
  fork() && {
#ifndef NDEBUG
    assert(!is_empty && "You may only submit or co_await this once.");
    is_empty = true;
#endif
    return aw_spawn_func_fork<Result>(
      wrapped, executor, continuation_executor, prio
    );
  }

  /// Submit the tasks to the executor immediately. They cannot be awaited
  /// afterward.
  void detach()
    requires(std::is_void_v<Result>)
  {
#ifndef NDEBUG
    assert(!is_empty && "You may only submit or co_await this once.");
    is_empty = true;
#endif
#if TMC_WORK_ITEM_IS(CORO)
    tmc::detail::post_checked(executor, tmc::detail::into_task(wrapped), prio);
#else
    tmc::detail::post_checked(executor, std::move(wrapped), prio);
#endif
  }

#ifndef NDEBUG
  ~aw_spawn_func() noexcept {
    // This must be used, moved-from, or submitted for execution
    // in some way before destruction.
    assert(is_empty && "You must submit or co_await this.");
  }
#endif

  aw_spawn_func(const aw_spawn_func&) = delete;
  aw_spawn_func& operator=(const aw_spawn_func&) = delete;
  aw_spawn_func(aw_spawn_func&& Other)
      : wrapped(std::move(Other.wrapped)), executor(std::move(Other.executor)),
        continuation_executor(std::move(Other.continuation_executor)),
        prio(Other.prio) {
#ifndef NDEBUG
    is_empty = Other.is_empty;
    Other.is_empty = true;
#endif
  }
  aw_spawn_func& operator=(aw_spawn_func&& Other) {
    wrapped = std::move(Other.wrapped);
    executor = std::move(Other.executor);
    continuation_executor = std::move(Other.continuation_executor);
    prio = Other.prio;
#ifndef NDEBUG
    is_empty = Other.is_empty;
    Other.is_empty = true;
#endif
    return *this;
  }
};

namespace detail {

template <typename Result> struct awaitable_traits<aw_spawn_func<Result>> {
  static constexpr configure_mode mode = WRAPPER;

  using result_type = Result;
  using self_type = aw_spawn_func<Result>;
  using awaiter_type = aw_spawn_func_impl<Result>;

  static awaiter_type get_awaiter(self_type&& Awaitable) noexcept {
    return std::forward<self_type>(Awaitable).operator co_await();
  }
};

} // namespace detail

} // namespace tmc
