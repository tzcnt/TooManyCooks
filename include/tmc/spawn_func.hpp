#pragma once
#include "tmc/detail/concepts.hpp" // IWYU pragma: keep
#include "tmc/detail/mixins.hpp"
#include "tmc/detail/thread_locals.hpp"
#include "tmc/task.hpp"
#include <cassert>
#include <coroutine>
#include <functional>

namespace tmc {

/// The customizable task wrapper / awaitable type returned by
/// `tmc::spawn(std::function)`.
template <typename Result> class aw_spawned_func;

template <typename Result> class aw_spawned_func_impl;

template <typename Result> class aw_spawned_func_impl {
  std::function<Result()> wrapped;
  detail::type_erased_executor* executor;
  detail::type_erased_executor* continuation_executor;
  size_t prio;
  Result result;
  friend aw_spawned_func<Result>;
  aw_spawned_func_impl(
    std::function<Result()> Func, detail::type_erased_executor* Executor,
    detail::type_erased_executor* ContinuationExecutor, size_t Prio
  )
      : wrapped(std::move(Func)), executor(Executor),
        continuation_executor(ContinuationExecutor), prio(Prio) {}

public:
  /// Always suspends.
  inline bool await_ready() const noexcept { return false; }

  /// Suspends the outer coroutine, submits the wrapped task to the
  /// executor, and waits for it to complete.
  TMC_FORCE_INLINE inline void await_suspend(std::coroutine_handle<> Outer
  ) noexcept {
#if TMC_WORK_ITEM_IS(CORO)
    detail::unsafe_task<Result> t(detail::into_task(wrapped));
    auto& p = t.promise();
    p.continuation = Outer.address();
    p.continuation_executor = continuation_executor;
    p.result_ptr = &result;
    executor->post(std::move(t), prio);
#else
    executor->post(
      [this, Outer]() {
        result = wrapped();
        if (continuation_executor == nullptr ||
            detail::this_thread::exec_is(continuation_executor)) {
          Outer.resume();
        } else {
          continuation_executor->post(Outer, prio);
        }
      },
      prio
    );
#endif
  }

  /// Returns the value provided by the wrapped task.
  inline Result&& await_resume() noexcept {
    // This appears to never be used - the 'this' parameter to
    // await_resume() is always an lvalue
    return std::move(result);
  }
};

template <> class aw_spawned_func_impl<void> {
  std::function<void()> wrapped;
  detail::type_erased_executor* executor;
  detail::type_erased_executor* continuation_executor;
  size_t prio;
  friend aw_spawned_func<void>;

  aw_spawned_func_impl(
    std::function<void()> Func, detail::type_erased_executor* Executor,
    detail::type_erased_executor* ContinuationExecutor, size_t Prio
  )
      : wrapped(std::move(Func)), executor(Executor),
        continuation_executor(ContinuationExecutor), prio(Prio) {}

public:
  /// Always suspends.
  inline bool await_ready() const noexcept { return false; }

  /// Suspends the outer coroutine, submits the wrapped task to the
  /// executor, and waits for it to complete.
  TMC_FORCE_INLINE inline void await_suspend(std::coroutine_handle<> Outer
  ) noexcept {
#if TMC_WORK_ITEM_IS(CORO)
    detail::unsafe_task<void> t(detail::into_task(wrapped));
    auto& p = t.promise();
    p.continuation = Outer.address();
    p.continuation_executor = continuation_executor;
    executor->post(std::move(t), prio);
#else
    executor->post(
      [this, Outer]() {
        wrapped();
        if (continuation_executor == nullptr ||
            detail::this_thread::exec_is(continuation_executor)) {
          Outer.resume();
        } else {
          continuation_executor->post(Outer, prio);
        }
      },
      prio
    );
#endif
  }

  /// Does nothing.
  inline void await_resume() noexcept {}
};

/// Wraps a function into a new task by `std::bind`ing the Func to its Args, and
/// wrapping them into a type that allows you to customize the task behavior
/// before submitting it for execution.
///
/// Before the task is submitted for execution, you may call any or all of
/// `run_on()`, `resume_on()`, `with_priority()`. The task must then be
/// submitted for execution by calling exactly one of: `co_await`, `run_early()`
/// or `detach()`.
template <typename Func, typename... Arguments>
auto spawn(Func&& func, Arguments&&... args)
  -> aw_spawned_func<decltype(func(args...))> {
  return aw_spawned_func<decltype(func(args...))>(
    std::bind(static_cast<Func&&>(func), static_cast<Arguments&&>(args)...)
  );
}

template <typename Result>
class [[nodiscard("You must co_await aw_spawned_func<Result>."
)]] aw_spawned_func
    : public detail::run_on_mixin<aw_spawned_func<Result>>,
      public detail::resume_on_mixin<aw_spawned_func<Result>>,
      public detail::with_priority_mixin<aw_spawned_func<Result>> {
  friend class detail::run_on_mixin<aw_spawned_func<Result>>;
  friend class detail::resume_on_mixin<aw_spawned_func<Result>>;
  friend class detail::with_priority_mixin<aw_spawned_func<Result>>;
  std::function<Result()> wrapped;
  detail::type_erased_executor* executor;
  detail::type_erased_executor* continuation_executor;
  Result result;
  size_t prio;
#ifndef NDEBUG
  bool did_await;
#endif

  friend class aw_spawned_func_impl<Result>;

public:
  /// It is recommended to call `spawn()` instead of using this constructor
  /// directly.
  aw_spawned_func(std::function<Result()>&& Func)
      : wrapped(std::move(Func)), executor(detail::this_thread::executor),
        continuation_executor(detail::this_thread::executor),
        prio(detail::this_thread::this_task.prio)
#ifndef NDEBUG
        ,
        did_await(false)
#endif
  {
  }

  aw_spawned_func_impl<Result> operator co_await() && {
#ifndef NDEBUG
    did_await = true;
#endif
    return aw_spawned_func_impl<Result>(
      wrapped, executor, continuation_executor, prio
    );
  }

#ifndef NDEBUG
  ~aw_spawned_func() noexcept {
    // If you spawn a function that returns a non-void type,
    // then you must co_await the return of spawn!
    assert(did_await);
  }
#endif

  aw_spawned_func(const aw_spawned_func&) = delete;
  aw_spawned_func& operator=(const aw_spawned_func&) = delete;
  aw_spawned_func(aw_spawned_func&& Other) {
    wrapped = std::move(Other.wrapped);
    result = std::move(Other.result);
    prio = Other.prio;
#ifndef NDEBUG
    did_await = Other.did_await;
    Other.did_await = true; // prevent other from posting
#endif
  }
  aw_spawned_func& operator=(aw_spawned_func&& Other) {
    wrapped = std::move(Other.wrapped);
    result = std::move(Other.result);
    prio = Other.prio;
#ifndef NDEBUG
    did_await = Other.did_await;
    Other.did_await = true; // prevent other from posting
#endif
    return *this;
  }
};

template <>
class [[nodiscard("You must use the aw_spawned_func<void> by one of: 1. "
                  "co_await or 2. detach().")]] aw_spawned_func<void>
    : public detail::run_on_mixin<aw_spawned_func<void>>,
      public detail::resume_on_mixin<aw_spawned_func<void>>,
      public detail::with_priority_mixin<aw_spawned_func<void>> {
  friend class detail::run_on_mixin<aw_spawned_func<void>>;
  friend class detail::resume_on_mixin<aw_spawned_func<void>>;
  friend class detail::with_priority_mixin<aw_spawned_func<void>>;
  std::function<void()> wrapped;
  detail::type_erased_executor* executor;
  detail::type_erased_executor* continuation_executor;
  size_t prio;
#ifndef NDEBUG
  bool did_await;
#endif

  friend class aw_spawned_func_impl<void>;

public:
  /// It is recommended to call `spawn()` instead of using this constructor
  /// directly.
  aw_spawned_func(std::function<void()>&& Func)
      : wrapped(std::move(Func)), executor(detail::this_thread::executor),
        continuation_executor(detail::this_thread::executor),
        prio(detail::this_thread::this_task.prio)
#ifndef NDEBUG
        ,
        did_await(false)
#endif
  {
  }

  aw_spawned_func_impl<void> operator co_await() && {
#ifndef NDEBUG
    did_await = true;
#endif
    return aw_spawned_func_impl<void>(
      wrapped, executor, continuation_executor, prio
    );
  }

  /// Submit the tasks to the executor immediately. They cannot be awaited
  /// afterward.
  void detach() {
#ifndef NDEBUG
    assert(!did_await);
    did_await = true;
#endif
#if TMC_WORK_ITEM_IS(CORO)
    executor->post(detail::into_task(wrapped), prio);
#else
    executor->post(std::move(wrapped), prio);
#endif
  }

#ifndef NDEBUG
  ~aw_spawned_func() noexcept { assert(did_await); }
#endif

  aw_spawned_func(const aw_spawned_func&) = delete;
  aw_spawned_func& operator=(const aw_spawned_func&) = delete;
  aw_spawned_func(aw_spawned_func&& Other) {
    wrapped = std::move(Other.wrapped);
    prio = Other.prio;
#ifndef NDEBUG
    did_await = Other.did_await;
    Other.did_await = true; // prevent other from posting
#endif
  }
  aw_spawned_func& operator=(aw_spawned_func&& Other) {
    wrapped = std::move(Other.wrapped);
    prio = Other.prio;
#ifndef NDEBUG
    did_await = Other.did_await;
    Other.did_await = true; // prevent other from posting
#endif
    return *this;
  }
};

} // namespace tmc
