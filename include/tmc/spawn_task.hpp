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
  Result result;
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
    p.result_ptr = &result;
    executor->post(std::move(wrapped), prio);
  }

  /// Returns the value provided by the wrapped task.
  constexpr Result& await_resume() & noexcept { return result; }

  /// Returns the value provided by the wrapped task.
  constexpr Result&& await_resume() && noexcept {
    // This appears to never be used - the 'this' parameter to
    // await_resume() is always an lvalue
    return std::move(result);
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
  ///
  /// This cannot be used to spawn the task in a detached state.
  inline aw_run_early<Result, Result> run_early() {
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

  /// Submits the wrapped task immediately, without suspending the current
  /// coroutine. You must await the return type before destroying it.
  ///
  /// This is not how you spawn a task in a detached state! For that, just call
  /// spawn() and discard the return value.
  inline aw_run_early<void, void> run_early() {
    return aw_run_early<void, void>(
      std::move(wrapped), prio, executor, continuation_executor
    );
  }
};

/// spawn() creates a task wrapper that allows you to customize a task, by
/// calling `run_on()`, `resume_on()`, `with_priority()`, and/or `run_early()`
/// before the task is spawned.
///
/// If `Result` is non-void, the task will be spawned when the the wrapper is
/// co_await'ed:
/// `auto result = co_await spawn(task_result()).with_priority(1);`
///
/// If `Result` is void, you can do the same thing:
/// `co_await spawn(task_void()).with_priority(1);`
///
/// If `Result` is void, you also have the option to spawn it detached -
/// the task will be spawned when the wrapper temporary is destroyed:
/// `spawn(task_void()).with_priority(1);`
///
/// When `run_early()` is called, the task will be spawned immediately, and you
/// must co_await the returned awaitable later in this function. You cannot
/// simply destroy it, as the running task will have a pointer to it.
template <typename Result> aw_spawned_task<Result> spawn(task<Result>&& Task) {
  return aw_spawned_task<Result>(std::move(Task));
}

} // namespace tmc
