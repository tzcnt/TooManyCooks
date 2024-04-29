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
  inline Result&& await_resume() noexcept;
};

template <> class aw_spawned_func_impl<void> {
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
)]] aw_spawned_func
    : public detail::run_on_mixin<aw_spawned_func<Result>>,
      public detail::resume_on_mixin<aw_spawned_func<Result>>,
      public detail::with_priority_mixin<aw_spawned_func<Result>> {
  friend class detail::run_on_mixin<aw_spawned_func<Result>>;
  friend class detail::resume_on_mixin<aw_spawned_func<Result>>;
  friend class detail::with_priority_mixin<aw_spawned_func<Result>>;
  detail::type_erased_executor* executor;
  detail::type_erased_executor* continuation_executor;
  std::function<Result()> wrapped;
  Result result;
  size_t prio;
  bool did_await;

  friend class aw_spawned_func_impl<Result>;

public:
  /// It is recommended to call `spawn()` instead of using this constructor
  /// directly.
  aw_spawned_func(std::function<Result()>&& Func)
      : executor(detail::this_thread::executor),
        continuation_executor(detail::this_thread::executor),
        wrapped(std::move(Func)), prio(detail::this_thread::this_task.prio),
        did_await(false) {}

  aw_spawned_func_impl<Result> operator co_await() {
    return aw_spawned_func_impl<Result>(*this);
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
  detail::type_erased_executor* executor;
  detail::type_erased_executor* continuation_executor;
  std::function<void()> wrapped;
  size_t prio;
  bool did_await;

  friend class aw_spawned_func_impl<void>;

public:
  /// It is recommended to call `spawn()` instead of using this constructor
  /// directly.
  aw_spawned_func(std::function<void()>&& Func)
      : executor(detail::this_thread::executor),
        continuation_executor(detail::this_thread::executor),
        wrapped(std::move(Func)), prio(detail::this_thread::this_task.prio),
        did_await(false) {}

  aw_spawned_func_impl<void> operator co_await() {
    return aw_spawned_func_impl<void>(*this);
  }

  /// Submit the tasks to the executor immediately. They cannot be awaited
  /// afterward.
  void detach() {
    assert(!did_await);
#if TMC_WORK_ITEM_IS(CORO)
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
};

template <typename Result>
aw_spawned_func_impl<Result>::aw_spawned_func_impl(aw_spawned_func<Result>& Me)
    : me(Me) {}

/// Always suspends.
template <typename Result>
inline bool aw_spawned_func_impl<Result>::await_ready() const noexcept {
  return false;
}

/// Suspends the outer coroutine, submits the wrapped task to the
/// executor, and waits for it to complete.
template <typename Result>
inline void
aw_spawned_func_impl<Result>::await_suspend(std::coroutine_handle<> Outer
) noexcept {
  me.did_await = true;
#if TMC_WORK_ITEM_IS(CORO)
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
      if (me.continuation_executor == nullptr ||
          me.continuation_executor == detail::this_thread::executor) {
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
template <typename Result>
inline Result&& aw_spawned_func_impl<Result>::await_resume() noexcept {
  // This appears to never be used - the 'this' parameter to
  // await_resume() is always an lvalue
  return std::move(me.result);
}

inline aw_spawned_func_impl<void>::aw_spawned_func_impl(
  aw_spawned_func<void>& Me
)
    : me(Me) {}

inline bool aw_spawned_func_impl<void>::await_ready() const noexcept {
  return false;
}

/// Suspends the outer coroutine, submits the wrapped task to the
/// executor, and waits for it to complete.
inline void
aw_spawned_func_impl<void>::await_suspend(std::coroutine_handle<> Outer
) noexcept {
  me.did_await = true;
#if TMC_WORK_ITEM_IS(CORO)
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
      me.wrapped();
      if (me.continuation_executor == nullptr ||
          me.continuation_executor == detail::this_thread::executor) {
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
inline void aw_spawned_func_impl<void>::await_resume() noexcept {}

} // namespace tmc
