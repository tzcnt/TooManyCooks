#pragma once
#include "tmc/detail/aw_run_early.hpp"
#include "tmc/detail/concepts.hpp" // IWYU pragma: keep
#include "tmc/detail/thread_locals.hpp"
#include "tmc/task.hpp"
#include <cassert>
#include <coroutine>
namespace tmc {

// Primary template is forward-declared in "tmc/detail/aw_run_early.hpp".
template <typename Result>
class [[nodiscard("You must use the aw_spawned_task<Result> by one of: 1. "
                  "co_await 2. run_early()")]] aw_spawned_task {
  detail::type_erased_executor* executor;
  detail::type_erased_executor* continuation_executor;
  task<Result> wrapped;
  size_t prio;

public:
  /// It is recommended to call `spawn()` instead of using this constructor
  /// directly.
  aw_spawned_task(task<Result>&& Task)
      : executor(detail::this_thread::executor),
        continuation_executor(detail::this_thread::executor),
        wrapped(std::move(Task)), prio(detail::this_thread::this_task.prio) {}

  /// Always suspends.
  constexpr bool await_ready() const noexcept { return false; }

  /// Suspends the outer coroutine, submits the wrapped task to the
  /// executor, and waits for it to complete.
  constexpr void await_suspend(std::coroutine_handle<> Outer) noexcept {
    assert(wrapped);
    auto& p = wrapped.promise();
    p.continuation = Outer.address();
    p.continuation_executor = continuation_executor;
    executor->post(std::move(wrapped), prio);
  }

  /// Returns the value provided by the wrapped task.
  constexpr Result await_resume() noexcept {
    auto r = wrapped.promise().result;
    // works in release mode with TMC_TRIVIAL_TASK only
    // in debug mode, wrapped will have been null'ed already
    wrapped.destroy();
    wrapped = nullptr;
    return r;
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

public:
  /// It is recommended to call `spawn()` instead of using this constructor
  /// directly.
  aw_spawned_task(task<void>&& Task)
      : executor(detail::this_thread::executor),
        continuation_executor(detail::this_thread::executor),
        wrapped(std::move(Task)), prio(detail::this_thread::this_task.prio) {}

  /// Always suspends.
  constexpr bool await_ready() const noexcept { return false; }

  /// Suspends the outer coroutine, submits the wrapped task to the
  /// executor, and waits for it to complete.
  void await_suspend(std::coroutine_handle<> Outer) noexcept {
    assert(wrapped);
    auto& p = wrapped.promise();
    p.continuation = Outer.address();
    p.continuation_executor = continuation_executor;
    executor->post(std::move(wrapped), prio);
  }

  /// Does nothing.
  constexpr void await_resume() const noexcept {}

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
