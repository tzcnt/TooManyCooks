#pragma once
#include "tmc/detail/concepts.hpp" // IWYU pragma: keep
#include "tmc/detail/thread_locals.hpp"
#include "tmc/task.hpp"
#include <cassert>
#include <coroutine>
#include <functional>

namespace tmc {

/// The customizable task wrapper / awaitable type returned by
/// `tmc::spawn(std::function)`.
template <typename Result> class aw_spawned_func;

template <typename Result, bool RValue> class aw_spawned_func_impl;

template <typename Result, bool RValue> class aw_spawned_func_impl {
  aw_spawned_func<Result>& me;
  friend aw_spawned_func<Result>;
  aw_spawned_func_impl(aw_spawned_func<Result>& Me);

public:
  /// Always suspends.
  inline bool await_ready() const noexcept;

  /// Suspends the outer coroutine, submits the wrapped task to the
  /// executor, and waits for it to complete.
  inline void await_suspend(std::coroutine_handle<> Outer) noexcept;

  /// Returns the value provided by the wrapped task.
  inline Result& await_resume() noexcept
    requires(!RValue);

  /// Returns the value provided by the wrapped task.
  inline Result&& await_resume() noexcept
    requires(RValue);
};

template <> class aw_spawned_func_impl<void, false> {
  aw_spawned_func<void>& me;
  friend aw_spawned_func<void>;

  inline aw_spawned_func_impl(aw_spawned_func<void>& Me);

public:
  /// Always suspends.
  inline bool await_ready() const noexcept;

  /// Suspends the outer coroutine, submits the wrapped task to the
  /// executor, and waits for it to complete.
  inline void await_suspend(std::coroutine_handle<> Outer) noexcept;

  /// Does nothing.
  inline void await_resume() noexcept;
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
)]] aw_spawned_func {
  detail::type_erased_executor* executor;
  detail::type_erased_executor* continuation_executor;
  std::function<Result()> wrapped;
  Result result;
  size_t prio;
  bool did_await;

  friend class aw_spawned_func_impl<Result, false>;
  friend class aw_spawned_func_impl<Result, true>;

public:
  /// It is recommended to call `spawn()` instead of using this constructor
  /// directly.
  aw_spawned_func(std::function<Result()>&& Func)
      : executor(detail::this_thread::executor),
        continuation_executor(detail::this_thread::executor),
        wrapped(std::move(Func)), prio(detail::this_thread::this_task.prio),
        did_await(false) {}

  aw_spawned_func_impl<Result, false> operator co_await() & {
    return aw_spawned_func_impl<Result, false>(*this);
  }

  aw_spawned_func_impl<Result, true> operator co_await() && {
    return aw_spawned_func_impl<Result, true>(*this);
  }

  ~aw_spawned_func() noexcept {
    // If you spawn a function that returns a non-void type,
    // then you must co_await the return of spawn!
    assert(did_await);
  }
  aw_spawned_func(const aw_spawned_func&) = delete;
  aw_spawned_func& operator=(const aw_spawned_func&) = delete;
  aw_spawned_func(aw_spawned_func&& Other) {
    wrapped = std::move(Other.wrapped);
    result = std::move(Other.result);
    prio = Other.prio;
    did_await = Other.did_await;
    Other.did_await = true; // prevent other from posting
  }
  aw_spawned_func& operator=(aw_spawned_func&& Other) {
    wrapped = std::move(Other.wrapped);
    result = std::move(Other.result);
    prio = Other.prio;
    did_await = Other.did_await;
    Other.did_await = true; // prevent other from posting
    return *this;
  }

  /// After the spawned function completes, the outer coroutine will be resumed
  /// on the provided executor.
  inline aw_spawned_func& resume_on(detail::type_erased_executor* Executor) {
    continuation_executor = Executor;
    return *this;
  }
  /// After the spawned function completes, the outer coroutine will be resumed
  /// on the provided executor.
  template <detail::TypeErasableExecutor Exec>
  aw_spawned_func& resume_on(Exec& Executor) {
    return resume_on(Executor.type_erased());
  }
  /// After the spawned function completes, the outer coroutine will be resumed
  /// on the provided executor.
  template <detail::TypeErasableExecutor Exec>
  aw_spawned_func& resume_on(Exec* Executor) {
    return resume_on(Executor->type_erased());
  }

  /// The wrapped function will run on the provided executor.
  inline aw_spawned_func& run_on(detail::type_erased_executor* e) {
    executor = e;
    return *this;
  }
  /// The wrapped function will run on the provided executor.
  template <detail::TypeErasableExecutor Exec>
  aw_spawned_func& run_on(Exec& Executor) {
    return run_on(Executor.type_erased());
  }
  /// The wrapped function will run on the provided executor.
  template <detail::TypeErasableExecutor Exec>
  aw_spawned_func& run_on(Exec* Executor) {
    return run_on(Executor->type_erased());
  }

  /// Sets the priority of the wrapped function. If co_awaited, the outer
  /// coroutine will also be resumed with this priority.
  inline aw_spawned_func& with_priority(size_t Priority) {
    prio = Priority;
    return *this;
  }
};

template <>
class [[nodiscard("You must use the aw_spawned_func<void> by one of: 1. "
                  "co_await or 2. detach().")]] aw_spawned_func<void> {
  detail::type_erased_executor* executor;
  detail::type_erased_executor* continuation_executor;
  std::function<void()> wrapped;
  size_t prio;
  bool did_await;

  friend class aw_spawned_func_impl<void, false>;

public:
  /// It is recommended to call `spawn()` instead of using this constructor
  /// directly.
  aw_spawned_func(std::function<void()>&& Func)
      : executor(detail::this_thread::executor),
        continuation_executor(detail::this_thread::executor),
        wrapped(std::move(Func)), prio(detail::this_thread::this_task.prio),
        did_await(false) {}

  aw_spawned_func_impl<void, false> operator co_await() {
    return aw_spawned_func_impl<void, false>(*this);
  }

  /// Submit the tasks to the executor immediately. They cannot be awaited
  /// afterward.
  void detach() {
    assert(!did_await);
#if WORK_ITEM_IS(CORO)
    executor->post(
      [](std::function<void()> Func) -> task<void> {
        Func();
        co_return;
      }(wrapped),
      prio
    );
#else
    executor->post(std::move(wrapped), prio);
#endif
    did_await = true;
  }

  ~aw_spawned_func() noexcept { assert(did_await); }

  aw_spawned_func(const aw_spawned_func&) = delete;
  aw_spawned_func& operator=(const aw_spawned_func&) = delete;
  aw_spawned_func(aw_spawned_func&& Other) {
    wrapped = std::move(Other.wrapped);
    prio = Other.prio;
    did_await = Other.did_await;
    Other.did_await = true; // prevent other from posting
  }
  aw_spawned_func& operator=(aw_spawned_func&& Other) {
    wrapped = std::move(Other.wrapped);
    prio = Other.prio;
    did_await = Other.did_await;
    Other.did_await = true; // prevent other from posting
    return *this;
  }

  /// When awaited, the outer coroutine will be resumed on the provided
  /// executor.
  inline aw_spawned_func& resume_on(detail::type_erased_executor* Executor) {
    continuation_executor = Executor;
    return *this;
  }

  /// When awaited, the outer coroutine will be resumed on the provided
  /// executor.
  template <detail::TypeErasableExecutor Exec>
  aw_spawned_func& resume_on(Exec& Executor) {
    return resume_on(Executor.type_erased());
  }

  /// When awaited, the outer coroutine will be resumed on the provided
  /// executor.
  template <detail::TypeErasableExecutor Exec>
  aw_spawned_func& resume_on(Exec* Executor) {
    return resume_on(Executor->type_erased());
  }

  /// The wrapped function will be submitted to the provided executor.
  inline aw_spawned_func& run_on(detail::type_erased_executor* Executor) {
    executor = Executor;
    return *this;
  }

  /// The wrapped function will be submitted to the provided executor.
  template <detail::TypeErasableExecutor Exec>
  aw_spawned_func& run_on(Exec& Executor) {
    return run_on(Executor.type_erased());
  }

  /// The wrapped function will be submitted to the provided executor.
  template <detail::TypeErasableExecutor Exec>
  aw_spawned_func& run_on(Exec* Executor) {
    return run_on(Executor->type_erased());
  }

  /// Sets the priority of the wrapped function. If co_awaited, the outer
  /// coroutine will also be resumed with this priority.
  inline aw_spawned_func& with_priority(size_t Priority) {
    prio = Priority;
    return *this;
  }
};

template <typename Result, bool RValue>
aw_spawned_func_impl<Result, RValue>::aw_spawned_func_impl(
  aw_spawned_func<Result>& Me
)
    : me(Me) {}

/// Always suspends.
template <typename Result, bool RValue>
inline bool aw_spawned_func_impl<Result, RValue>::await_ready() const noexcept {
  return false;
}

/// Suspends the outer coroutine, submits the wrapped task to the
/// executor, and waits for it to complete.
template <typename Result, bool RValue>
inline void aw_spawned_func_impl<Result, RValue>::await_suspend(
  std::coroutine_handle<> Outer
) noexcept {
  me.did_await = true;
#if WORK_ITEM_IS(CORO)
  auto t = [](aw_spawned_func<Result>* f) -> task<void> {
    f->result = f->wrapped();
    co_return;
  }(&me);
  auto& p = t.promise();
  p.continuation = Outer.address();
  p.continuation_executor = me.continuation_executor;
  me.executor->post(std::move(t), me.prio);
#else
  me.executor->post(
    [this, Outer]() {
      me.result = me.wrapped();
      if (continuation_executor == nullptr ||
          continuation_executor == detail::this_thread::executor) {
        Outer.resume();
      } else {
        me.continuation_executor->post(
          Outer, detail::this_thread::this_task.prio
        );
      }
    },
    me.prio
  );
#endif
}

/// Returns the value provided by the wrapped function.
template <typename Result, bool RValue>
inline Result& aw_spawned_func_impl<Result, RValue>::await_resume() noexcept
  requires(!RValue)
{
  return me.result;
}

/// Returns the value provided by the wrapped function.
template <typename Result, bool RValue>
inline Result&& aw_spawned_func_impl<Result, RValue>::await_resume() noexcept
  requires(RValue)
{
  // This appears to never be used - the 'this' parameter to
  // await_resume() is always an lvalue
  return std::move(me.result);
}

inline aw_spawned_func_impl<void, false>::aw_spawned_func_impl(
  aw_spawned_func<void>& Me
)
    : me(Me) {}

inline bool aw_spawned_func_impl<void, false>::await_ready() const noexcept {
  return false;
}

/// Suspends the outer coroutine, submits the wrapped task to the
/// executor, and waits for it to complete.
inline void
aw_spawned_func_impl<void, false>::await_suspend(std::coroutine_handle<> Outer
) noexcept {
  me.did_await = true;
#if WORK_ITEM_IS(CORO)
  auto t = [](aw_spawned_func<void>* f) -> task<void> {
    f->wrapped();
    co_return;
  }(&me);
  auto& p = t.promise();
  p.continuation = Outer.address();
  p.continuation_executor = me.continuation_executor;
  me.executor->post(std::move(t), me.prio);
#else
  me.executor->post(
    [this, Outer]() {
      wrapped();
      if (continuation_executor == nullptr ||
          continuation_executor == detail::this_thread::executor) {
        Outer.resume();
      } else {
        me.continuation_executor->post(
          Outer, detail::this_thread::this_task.prio
        );
      }
    },
    me.prio
  );
#endif
}

/// Does nothing.
inline void aw_spawned_func_impl<void, false>::await_resume() noexcept {}

} // namespace tmc
