// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/current.hpp"
#include "tmc/detail/compat.hpp"
#include "tmc/detail/concepts_awaitable.hpp" // IWYU pragma: keep
#include "tmc/detail/concepts_work_item.hpp"
#include "tmc/detail/mixins.hpp"
#include "tmc/detail/task_wrapper.hpp"
#include "tmc/detail/thread_locals.hpp"
#include "tmc/ex_any.hpp"

#include <cassert>
#include <coroutine>
#include <type_traits>

namespace tmc {
namespace detail {

template <typename Awaitable>
TMC_FORCE_INLINE inline void initiate_one(
  Awaitable&& Item, tmc::ex_any* Executor, size_t Priority
) noexcept {
  if constexpr (tmc::detail::get_awaitable_traits<Awaitable>::mode ==
                  TMC_TASK ||
                tmc::detail::get_awaitable_traits<Awaitable>::mode ==
                  COROUTINE) {
    tmc::detail::post_checked(
      Executor, static_cast<Awaitable&&>(Item), Priority
    );
  } else if constexpr (tmc::detail::get_awaitable_traits<Awaitable>::mode ==
                       ASYNC_INITIATE) {
    tmc::detail::get_awaitable_traits<Awaitable>::async_initiate(
      static_cast<Awaitable&&>(Item), Executor, Priority
    );
  } else { // WRAPPER
    tmc::detail::post_checked(
      Executor, tmc::detail::safe_wrap(static_cast<Awaitable&&>(Item)), Priority
    );
  }
}
} // namespace detail

/// The customizable task wrapper / awaitable type returned by
/// `tmc::spawn()`.
template <
  typename Awaitable,
  typename Result = tmc::detail::awaitable_result_t<Awaitable>>
class aw_spawn;

/// A wrapper that converts lazy task(s) to eager task(s),
/// and allows the task(s) to be awaited after it has been started.
/// It is created by calling `fork()` on a parent awaitable
/// from `spawn()`.
///
/// `Result` is the type of a single result value.
template <
  typename Awaitable,
  typename Result = tmc::detail::awaitable_result_t<Awaitable>>
class [[nodiscard(
  "You must co_await aw_spawn_fork. "
  "It is not safe to destroy aw_spawn_fork without first "
  "awaiting it."
)]] aw_spawn_fork_impl;

template <typename Awaitable, typename Result> class aw_spawn_fork_impl {
  std::coroutine_handle<> continuation;
  tmc::ex_any* continuation_executor;
  std::atomic<ptrdiff_t> done_count;
  struct empty {};
  using ResultStorage = std::conditional_t<
    std::is_void_v<Result>, empty, tmc::detail::result_storage_t<Result>>;
  TMC_NO_UNIQUE_ADDRESS ResultStorage result;

  friend class aw_spawn<Awaitable>;

  template <typename T>
  TMC_FORCE_INLINE inline void
  initiate(T&& Task, tmc::ex_any* Executor, size_t Priority) {
    tmc::detail::get_awaitable_traits<T>::set_continuation(Task, &continuation);
    tmc::detail::get_awaitable_traits<T>::set_continuation_executor(
      Task, &continuation_executor
    );
    tmc::detail::get_awaitable_traits<T>::set_done_count(Task, &done_count);
    if constexpr (!std::is_void_v<Result>) {
      tmc::detail::get_awaitable_traits<T>::set_result_ptr(Task, &result);
    }
    tmc::detail::initiate_one(static_cast<T&&>(Task), Executor, Priority);
  }

  // Private constructor from aw_spawn. Takes ownership of parent's
  // task.
  aw_spawn_fork_impl(
    Awaitable&& Task, tmc::ex_any* Executor, tmc::ex_any* ContinuationExecutor,
    size_t Priority
  )
      : continuation{nullptr}, continuation_executor(ContinuationExecutor),
        done_count(1) {
    initiate(
      tmc::detail::into_known<false>(static_cast<Awaitable&&>(Task)), Executor,
      Priority
    );
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

  /// Returns the value provided by the wrapped function.
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
  ~aw_spawn_fork_impl() noexcept {
    assert(
      done_count.load() < 0 &&
      "You must co_await the fork() awaitable before it goes out of scope."
    );
  }
#endif

  // Not movable or copyable due to child task being spawned in constructor,
  // and having pointers to this.
  aw_spawn_fork_impl& operator=(const aw_spawn_fork_impl& other) = delete;
  aw_spawn_fork_impl(const aw_spawn_fork_impl& other) = delete;
  aw_spawn_fork_impl& operator=(aw_spawn_fork_impl&& other) = delete;
  aw_spawn_fork_impl(aw_spawn_fork_impl&& other) = delete;
};

template <typename Awaitable>
using aw_spawn_fork =
  tmc::detail::rvalue_only_awaitable<aw_spawn_fork_impl<Awaitable>>;

template <
  typename Awaitable,
  typename Result = tmc::detail::awaitable_result_t<Awaitable>>
class aw_spawn_impl;

template <typename Awaitable, typename Result> class aw_spawn_impl {
  Awaitable wrapped;
  tmc::ex_any* executor;
  tmc::ex_any* continuation_executor;
  size_t prio;

  struct empty {};
  using ResultStorage = std::conditional_t<
    std::is_void_v<Result>, empty, tmc::detail::result_storage_t<Result>>;
  TMC_NO_UNIQUE_ADDRESS ResultStorage result;

  friend aw_spawn<Awaitable>;

  aw_spawn_impl(
    Awaitable&& Task, tmc::ex_any* Executor, tmc::ex_any* ContinuationExecutor,
    size_t Prio
  )
      : wrapped{static_cast<Awaitable&&>(Task)}, executor{Executor},
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
    tmc::detail::initiate_one(static_cast<T&&>(Task), executor, prio);
  }

public:
  /// Always suspends.
  inline bool await_ready() const noexcept { return false; }

  /// Suspends the outer coroutine, submits the wrapped task to the
  /// executor, and waits for it to complete.
  inline void await_suspend(std::coroutine_handle<> Outer) noexcept {
    initiate(
      tmc::detail::into_known<false>(static_cast<Awaitable&&>(wrapped)), Outer
    );
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
class [[nodiscard(
  "You must await or initiate the result of spawn()."
)]] TMC_CORO_AWAIT_ELIDABLE aw_spawn
    : public tmc::detail::run_on_mixin<aw_spawn<Awaitable, Result>>,
      public tmc::detail::resume_on_mixin<aw_spawn<Awaitable, Result>>,
      public tmc::detail::with_priority_mixin<aw_spawn<Awaitable, Result>> {
  friend class tmc::detail::run_on_mixin<aw_spawn<Awaitable, Result>>;
  friend class tmc::detail::resume_on_mixin<aw_spawn<Awaitable, Result>>;
  friend class tmc::detail::with_priority_mixin<aw_spawn<Awaitable, Result>>;
  Awaitable wrapped;
  tmc::ex_any* executor;
  tmc::ex_any* continuation_executor;
  size_t prio;

#ifndef NDEBUG
  bool is_empty;
#endif

public:
  /// It is recommended to call `spawn()` instead of using this constructor
  /// directly.
  aw_spawn(Awaitable&& Task)
      : wrapped(static_cast<Awaitable&&>(Task)),
        executor(tmc::detail::this_thread::executor),
        continuation_executor(tmc::detail::this_thread::executor),
        prio(tmc::detail::this_thread::this_task.prio)
#ifndef NDEBUG
        ,
        is_empty(false)
#endif
  {
  }

  aw_spawn_impl<Awaitable> operator co_await() && noexcept {

#ifndef NDEBUG
    assert(!is_empty && "You may only submit or co_await this once.");
    is_empty = true;
#endif
    return aw_spawn_impl<Awaitable>(
      static_cast<Awaitable&&>(wrapped), executor, continuation_executor, prio
    );
  }

  /// Submits the wrapped task to the executor immediately. It cannot be awaited
  /// afterward.
  void detach()
    requires(std::is_void_v<Result>)
  {
#ifndef NDEBUG
    assert(!is_empty && "You may only submit or co_await this once.");
    is_empty = true;
#endif
    tmc::detail::initiate_one(
      static_cast<Awaitable&&>(wrapped), executor, prio
    );
  }

#if !defined(NDEBUG)
  ~aw_spawn() noexcept {
    // This must be used, moved-from, or submitted for execution
    // in some way before destruction.
    assert(is_empty && "You must submit or co_await this.");
  }
#endif
  aw_spawn(const aw_spawn&) = delete;
  aw_spawn& operator=(const aw_spawn&) = delete;
  aw_spawn(aw_spawn&& Other)
      : wrapped(static_cast<Awaitable&&>(Other.wrapped)),
        executor(std::move(Other.executor)),
        continuation_executor(std::move(Other.continuation_executor)),
        prio(Other.prio) {
#if !defined(NDEBUG)
    is_empty = Other.is_empty;
    Other.is_empty = true;
#endif
  }
  aw_spawn& operator=(aw_spawn&& Other) {
    wrapped = static_cast<Awaitable&&>(Other.wrapped);
    executor = std::move(Other.executor);
    continuation_executor = std::move(Other.continuation_executor);
    prio = Other.prio;
#if !defined(NDEBUG)
    is_empty = Other.is_empty;
    Other.is_empty = true;
#endif
    return *this;
  }

  /// Submits the task to the executor immediately, without suspending the
  /// current coroutine. You must join the forked task by awaiting the returned
  /// awaitable before it goes out of scope.
  [[nodiscard(
    "You must co_await the fork() awaitable before it goes out of scope."
  )]] inline aw_spawn_fork<Awaitable>
  fork() && {

#ifndef NDEBUG
    assert(!is_empty && "You may only submit or co_await this once.");
    is_empty = true;
#endif
    return aw_spawn_fork<Awaitable>(
      static_cast<Awaitable&&>(wrapped), executor, continuation_executor, prio
    );
  }
};

namespace detail {

template <typename Awaitable, typename Result>
struct awaitable_traits<aw_spawn<Awaitable, Result>> {
  static constexpr configure_mode mode = WRAPPER;

  using result_type = Result;
  using self_type = aw_spawn<Awaitable, Result>;
  using awaiter_type = aw_spawn_impl<Awaitable, Result>;

  static awaiter_type get_awaiter(self_type&& awaitable) noexcept {
    return static_cast<self_type&&>(awaitable).operator co_await();
  }
};

} // namespace detail

/// `spawn()` allows you to customize the execution behavior of a task or
/// awaitable.
///
/// Before the task is submitted for execution, you may call any or all of
/// `run_on()`, `resume_on()`, `with_priority()`. The task must then be
/// submitted for execution by calling exactly one of: `co_await`, `fork()`
/// or `detach()`.
template <typename Awaitable>
aw_spawn<tmc::detail::forward_awaitable<Awaitable>>
spawn(TMC_CORO_AWAIT_ELIDABLE_ARGUMENT Awaitable&& Aw) {
  return aw_spawn<tmc::detail::forward_awaitable<Awaitable>>(
    static_cast<Awaitable&&>(Aw)
  );
}

/// This is a dummy awaitable. Don't store this in a variable.
/// For HALO to work, you must `co_await tmc::fork_clang()` immediately, which
/// will return the real awaitable (that you can await later to join the forked
/// task).
template <typename Awaitable>
class TMC_CORO_AWAIT_ELIDABLE aw_fork_clang : tmc::detail::AwaitTagNoGroupAsIs {
  Awaitable wrapped;
  tmc::ex_any* executor;
  size_t prio;

public:
  aw_fork_clang(Awaitable&& Task, tmc::ex_any* Executor, size_t Priority)
      : wrapped(static_cast<Awaitable&&>(Task)), executor{Executor},
        prio{Priority} {}

  /// Never suspends.
  bool await_ready() const noexcept { return true; }

  /// Never suspends.
  void await_suspend(std::coroutine_handle<>) noexcept {}

  /// Returns the value provided by the wrapped function.
  aw_spawn_fork<Awaitable> await_resume() noexcept {
    return tmc::spawn(static_cast<Awaitable&&>(wrapped))
      .run_on(executor)
      .with_priority(prio)
      .fork();
  }
};

/// Similar to `tmc::spawn(Aw).fork()` but allows the child task's
/// allocation to be elided by combining it into the parent's allocation (HALO).
/// This works by using specific attributes that are only available on Clang
/// 20+. You can safely call this function on other compilers but no
/// HALO-specific optimizations will be applied.
///
/// IMPORTANT: This returns a dummy awaitable. For HALO to work, you should
/// not store the dummy awaitable. Instead, `co_await` this expression
/// immediately, which returns the real awaitable that you
/// can use to join the forked task later. Proper usage:
/// ```
/// auto forked_task = co_await tmc::fork_clang(some_task());
/// do_some_other_work();
/// co_await std::move(forked_task);
/// ```
template <typename Awaitable, typename Exec = tmc::ex_any*>
[[nodiscard(
  "You must co_await fork_clang() immediately for HALO to be possible."
)]]
aw_fork_clang<tmc::detail::forward_awaitable<Awaitable>> fork_clang(
  TMC_CORO_AWAIT_ELIDABLE_ARGUMENT Awaitable&& Aw,
  Exec&& Executor = tmc::current_executor(),
  size_t Priority = tmc::current_priority()
) {
  return aw_fork_clang<tmc::detail::forward_awaitable<Awaitable>>(
    static_cast<Awaitable&&>(Aw),
    tmc::detail::get_executor_traits<Exec>::type_erased(Executor), Priority
  );
}

} // namespace tmc
