#pragma once
#include "tmc/detail/aw_run_early.hpp"
#include "tmc/detail/concepts.hpp" // IWYU pragma: keep
#include "tmc/detail/thread_locals.hpp"
#include "tmc/task.hpp"
#include <cassert>
#include <coroutine>
namespace tmc {

template <typename Result, bool RValue> class aw_spawned_task_impl;

template <typename Result, bool RValue> class aw_spawned_task_impl {
  aw_spawned_task<Result>& me;
  friend aw_spawned_task<Result>;
  aw_spawned_task_impl(aw_spawned_task<Result>& Me);

public:
  /// Always suspends.
  constexpr bool await_ready() const noexcept;

  /// Suspends the outer coroutine, submits the wrapped task to the
  /// executor, and waits for it to complete.
  constexpr void await_suspend(std::coroutine_handle<> Outer) noexcept;

  /// Returns the value provided by the wrapped task.
  constexpr Result& await_resume() noexcept
    requires(!RValue);

  /// Returns the value provided by the wrapped task.
  constexpr Result&& await_resume() noexcept
    requires(RValue);
};

template <> class aw_spawned_task_impl<void, false> {
  aw_spawned_task<void>& me;
  friend aw_spawned_task<void>;

  constexpr aw_spawned_task_impl(aw_spawned_task<void>& Me);

public:
  /// Always suspends.
  constexpr bool await_ready() const noexcept;

  /// Suspends the outer coroutine, submits the wrapped task to the
  /// executor, and waits for it to complete.
  constexpr void await_suspend(std::coroutine_handle<> Outer) noexcept;

  /// Does nothing.
  constexpr void await_resume() noexcept;
};

// Primary template is forward-declared in "tmc/detail/aw_run_early.hpp".
template <typename Result>
class [[nodiscard("You must use the aw_spawned_task<Result> by one of: 1. "
                  "co_await 2. run_early()")]] aw_spawned_task {
  detail::type_erased_executor* executor;
  detail::type_erased_executor* continuation_executor;
  task<Result> wrapped;
  Result result;
  size_t prio;

  friend class aw_spawned_task_impl<Result, false>;
  friend class aw_spawned_task_impl<Result, true>;

public:
  /// It is recommended to call `spawn()` instead of using this constructor
  /// directly.
  aw_spawned_task(task<Result>&& Task)
      : executor(detail::this_thread::executor),
        continuation_executor(detail::this_thread::executor),
        wrapped(std::move(Task)), prio(detail::this_thread::this_task.prio) {}

  aw_spawned_task_impl<Result, false> operator co_await() & {
    return aw_spawned_task_impl<Result, false>(*this);
  }

  aw_spawned_task_impl<Result, true> operator co_await() && {
    return aw_spawned_task_impl<Result, true>(*this);
  }

  ~aw_spawned_task() noexcept {
    // If you spawn a task that returns a non-void type,
    // then you must co_await the return of spawn!
    assert(!wrapped);
  }
  aw_spawned_task(const aw_spawned_task&) = delete;
  aw_spawned_task& operator=(const aw_spawned_task&) = delete;
  aw_spawned_task(aw_spawned_task&& Other)
      : executor(std::move(Other.executor)), wrapped(std::move(Other.wrapped)),
        result(std::move(Other.result)), prio(Other.prio) {}
  aw_spawned_task& operator=(aw_spawned_task&& Other) {
    executor = std::move(Other.executor);
    wrapped = std::move(Other.wrapped);
    result = std::move(Other.result);
    prio = Other.prio;
    return *this;
  }

  /// When the spawned task completes, the awaiting coroutine will be resumed
  /// on the provided executor.
  inline aw_spawned_task& resume_on(detail::type_erased_executor* Executor) {
    continuation_executor = Executor;
    return *this;
  }
  /// When the spawned task completes, the awaiting coroutine will be resumed
  /// on the provided executor.
  template <detail::TypeErasableExecutor Exec>
  aw_spawned_task& resume_on(Exec& Executor) {
    return resume_on(Executor.type_erased());
  }
  /// When the spawned task completes, the awaiting coroutine will be resumed
  /// on the provided executor.
  template <detail::TypeErasableExecutor Exec>
  aw_spawned_task& resume_on(Exec* Executor) {
    return resume_on(Executor->type_erased());
  }

  /// The wrapped task will run on the provided executor.
  inline aw_spawned_task& run_on(detail::type_erased_executor* Executor) {
    executor = Executor;
    return *this;
  }
  /// The wrapped task will run on the provided executor.
  template <detail::TypeErasableExecutor Exec>
  aw_spawned_task& run_on(Exec& Executor) {
    return run_on(Executor.type_erased());
  }
  /// The wrapped task will run on the provided executor.
  template <detail::TypeErasableExecutor Exec>
  aw_spawned_task& run_on(Exec* Executor) {
    return run_on(Executor->type_erased());
  }

  /// Sets the priority of the wrapped task. If co_awaited, the outer
  /// coroutine will also be resumed with this priority.
  inline aw_spawned_task& with_priority(size_t Priority) {
    prio = Priority;
    return *this;
  }

  /// Submits the wrapped task immediately, without suspending the current
  /// coroutine. You must await the return type before destroying it.
  inline aw_run_early<Result, Result> run_early() && {
    return aw_run_early<Result, Result>(
      std::move(wrapped), prio, executor, continuation_executor
    );
  }
};

template <>
class [[nodiscard(
  "You must use the aw_spawned_task<void> by one of: 1. co_await 2. "
  "detach() 3. run_early()"
)]] aw_spawned_task<void> {
  detail::type_erased_executor* executor;
  detail::type_erased_executor* continuation_executor;
  task<void> wrapped;
  size_t prio;

  friend class aw_spawned_task_impl<void, false>;

public:
  /// It is recommended to call `spawn()` instead of using this constructor
  /// directly.
  aw_spawned_task(task<void>&& Task)
      : executor(detail::this_thread::executor),
        continuation_executor(detail::this_thread::executor),
        wrapped(std::move(Task)), prio(detail::this_thread::this_task.prio) {}

  aw_spawned_task_impl<void, false> operator co_await() {
    return aw_spawned_task_impl<void, false>(*this);
  }

  /// Submit the task to the executor immediately. It cannot be awaited
  /// afterward.
  void detach() {
    assert(wrapped);
    executor->post(std::move(wrapped), prio);
  }

  ~aw_spawned_task() noexcept {
    // If you spawn a task that returns a void type,
    // then you must co_await or detach the return of spawn!
    assert(!wrapped);
  }
  aw_spawned_task(const aw_spawned_task&) = delete;
  aw_spawned_task& operator=(const aw_spawned_task&) = delete;
  aw_spawned_task(aw_spawned_task&& Other)
      : executor(std::move(Other.executor)), wrapped(std::move(Other.wrapped)),
        prio(Other.prio) {}
  aw_spawned_task& operator=(aw_spawned_task&& Other) {
    executor = std::move(Other.executor);
    wrapped = std::move(Other.wrapped);
    prio = Other.prio;
    return *this;
  }

  /// When the spawned task completes, the awaiting coroutine will be resumed
  /// on the provided executor.
  inline aw_spawned_task& resume_on(detail::type_erased_executor* Executor) {
    continuation_executor = Executor;
    return *this;
  }
  /// When the spawned task completes, the awaiting coroutine will be resumed
  /// on the provided executor.
  template <detail::TypeErasableExecutor Exec>
  aw_spawned_task& resume_on(Exec& Executor) {
    return resume_on(Executor.type_erased());
  }
  /// When the spawned task completes, the awaiting coroutine will be resumed
  /// on the provided executor.
  template <detail::TypeErasableExecutor Exec>
  aw_spawned_task& resume_on(Exec* Executor) {
    return resume_on(Executor->type_erased());
  }

  /// The wrapped task will run on the provided executor.
  inline aw_spawned_task& run_on(detail::type_erased_executor* Executor) {
    executor = Executor;
    return *this;
  }
  /// The wrapped task will run on the provided executor.
  template <detail::TypeErasableExecutor Exec>
  aw_spawned_task& run_on(Exec& Executor) {
    return run_on(Executor.type_erased());
  }
  /// The wrapped task will run on the provided executor.
  template <detail::TypeErasableExecutor Exec>
  aw_spawned_task& run_on(Exec* Executor) {
    return run_on(Executor->type_erased());
  }

  /// Sets the priority of the wrapped task. If co_awaited, the outer
  /// coroutine will also be resumed with this priority.
  inline aw_spawned_task& with_priority(size_t Priority) {
    prio = Priority;
    return *this;
  }

  /// Submits the wrapped task immediately, without suspending the current
  /// coroutine. You must await the returned before destroying it.
  inline aw_run_early<void, void> run_early() && {
    return aw_run_early<void, void>(
      std::move(wrapped), prio, executor, continuation_executor
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

template <typename Result, bool RValue>
aw_spawned_task_impl<Result, RValue>::aw_spawned_task_impl(
  aw_spawned_task<Result>& Me
)
    : me(Me) {}

/// Always suspends.
template <typename Result, bool RValue>
constexpr bool
aw_spawned_task_impl<Result, RValue>::await_ready() const noexcept {
  return false;
}

/// Suspends the outer coroutine, submits the wrapped task to the
/// executor, and waits for it to complete.
template <typename Result, bool RValue>
constexpr void aw_spawned_task_impl<Result, RValue>::await_suspend(
  std::coroutine_handle<> Outer
) noexcept {
  assert(me.wrapped);
  auto& p = me.wrapped.promise();
  p.continuation = Outer.address();
  p.continuation_executor = me.continuation_executor;
  p.result_ptr = &me.result;
  me.executor->post(std::move(me.wrapped), me.prio);
}

/// Returns the value provided by the wrapped task.
template <typename Result, bool RValue>
constexpr Result& aw_spawned_task_impl<Result, RValue>::await_resume() noexcept
  requires(!RValue)
{
  return me.result;
}

/// Returns the value provided by the wrapped task.
template <typename Result, bool RValue>
constexpr Result&& aw_spawned_task_impl<Result, RValue>::await_resume() noexcept
  requires(RValue)
{
  // This appears to never be used - the 'this' parameter to
  // await_resume() is always an lvalue
  return std::move(me.result);
}

constexpr aw_spawned_task_impl<void, false>::aw_spawned_task_impl(
  aw_spawned_task<void>& Me
)
    : me(Me) {}

constexpr bool aw_spawned_task_impl<void, false>::await_ready() const noexcept {
  return false;
}

/// Suspends the outer coroutine, submits the wrapped task to the
/// executor, and waits for it to complete.
constexpr void
aw_spawned_task_impl<void, false>::await_suspend(std::coroutine_handle<> Outer
) noexcept {
  assert(me.wrapped);
  auto& p = me.wrapped.promise();
  p.continuation = Outer.address();
  p.continuation_executor = me.continuation_executor;
  me.executor->post(std::move(me.wrapped), me.prio);
}

/// Does nothing.
constexpr void aw_spawned_task_impl<void, false>::await_resume() noexcept {}

} // namespace tmc
