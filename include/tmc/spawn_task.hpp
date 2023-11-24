#pragma once
#include "tmc/detail/aw_run_early.hpp"
#include "tmc/detail/concepts.hpp"
#include "tmc/detail/thread_locals.hpp"
#include "tmc/task.hpp"
#include <cassert>
#include <coroutine>
#include <functional>
#include <mutex>
namespace tmc {

/// spawn() creates a task wrapper that allows you to customize a task, by
/// calling `run_on()`, `resume_on()`, `with_priority()`, and/or `run_early()`
/// before the task is spawned.
///
/// If `result_t` is non-void, the task will be spawned when the the wrapper is
/// co_await'ed:
/// `auto result = co_await spawn(task_result()).with_priority(1);`
///
/// If `result_t` is void, you can do the same thing:
/// `co_await spawn(task_void()).with_priority(1);`
///
/// If `result_t` is void, you also have the option to spawn it detached -
/// the task will be spawned when the wrapper temporary is destroyed:
/// `spawn(task_void()).with_priority(1);`
///
/// When `run_early()` is called, the task will be spawned immediately, and you
/// must co_await the returned awaitable later in this function. You cannot
/// simply destroy it, as the running task will have a pointer to it.
template <typename result_t> aw_spawned_task<result_t> spawn(task<result_t> t);
template <IsVoid result_t> aw_spawned_task<result_t> spawn(task<result_t> t) {
  return aw_spawned_task<result_t>(t);
}

template <IsNotVoid result_t>
aw_spawned_task<result_t> spawn(task<result_t> t) {
  return aw_spawned_task<result_t>(t);
}

// Primary template is forward-declared in "tmc/detail/aw_run_early.hpp".
template <IsNotVoid result_t>
class [[nodiscard("You must co_await the return of spawn(task<result_t>) "
                  "if result_t is not void.")]] aw_spawned_task<result_t> {
  using wrapped_t = task<result_t>;
  detail::type_erased_executor* executor;
  detail::type_erased_executor* continuation_executor;
  wrapped_t wrapped;
  result_t result;
  size_t prio;
  bool did_await;

public:
  /// It is recommended to call `spawn()` instead of using this constructor
  /// directly.
  aw_spawned_task(wrapped_t wrapped)
      : executor(detail::this_thread::executor),
        continuation_executor(detail::this_thread::executor), wrapped(wrapped),
        prio(detail::this_thread::this_task.prio), did_await(false) {}

  /// Always suspends.
  constexpr bool await_ready() const noexcept { return false; }

  /// Suspends the outer coroutine, submits the wrapped task to the
  /// executor, and waits for it to complete.
  constexpr void await_suspend(std::coroutine_handle<> outer) noexcept {
    assert(!did_await);
    did_await = true;
    auto& p = wrapped.promise();
    p.continuation = outer.address();
    p.continuation_executor = continuation_executor;
    p.result_ptr = &result;
    executor->post(std::coroutine_handle<>(wrapped), prio);
  }

  /// Returns the value provided by the wrapped task.
  constexpr result_t& await_resume() & noexcept { return result; }

  /// Returns the value provided by the wrapped task.
  constexpr result_t&& await_resume() && noexcept { return std::move(result); }

  ~aw_spawned_task() noexcept {
    // If you spawn a task that returns a non-void type,
    // then you must co_await the return of spawn!
    assert(did_await);
  }
  aw_spawned_task(const aw_spawned_task&) = delete;
  aw_spawned_task& operator=(const aw_spawned_task&) = delete;
  aw_spawned_task(aw_spawned_task&& other) {
    executor = std::move(other.executor);
    wrapped = std::move(other.wrapped);
    result = std::move(other.result);
    prio = other.prio;
    did_await = other.did_await;
    other.did_await = true; // prevent other from posting
  }
  aw_spawned_task& operator=(aw_spawned_task&& other) {
    executor = std::move(other.executor);
    wrapped = std::move(other.wrapped);
    result = std::move(other.result);
    prio = other.prio;
    did_await = other.did_await;
    other.did_await = true; // prevent other from posting
    return *this;
  }

  /// When the spawned task completes, the awaiting coroutine will be resumed
  /// on the provided executor.
  inline aw_spawned_task& resume_on(detail::type_erased_executor* e) {
    continuation_executor = e;
    return *this;
  }
  /// When the spawned task completes, the awaiting coroutine will be resumed
  /// on the provided executor.
  template <detail::TypeErasableExecutor Exec>
  aw_spawned_task& resume_on(Exec& executor) {
    return resume_on(executor.type_erased());
  }
  /// When the spawned task completes, the awaiting coroutine will be resumed
  /// on the provided executor.
  template <detail::TypeErasableExecutor Exec>
  aw_spawned_task& resume_on(Exec* executor) {
    return resume_on(executor->type_erased());
  }

  /// The wrapped task will run on the provided executor.
  inline aw_spawned_task& run_on(detail::type_erased_executor* e) {
    executor = e;
    return *this;
  }
  /// The wrapped task will run on the provided executor.
  template <detail::TypeErasableExecutor Exec>
  aw_spawned_task& run_on(Exec& executor) {
    return run_on(executor.type_erased());
  }
  /// The wrapped task will run on the provided executor.
  template <detail::TypeErasableExecutor Exec>
  aw_spawned_task& run_on(Exec* executor) {
    return run_on(executor->type_erased());
  }

  /// Sets the priority of the wrapped task. If co_awaited, the outer
  /// coroutine will also be resumed with this priority.
  inline aw_spawned_task& with_priority(size_t priority) {
    prio = priority;
    return *this;
  }

  /// Submits the wrapped task immediately, without suspending the current
  /// coroutine. You must await the return type before destroying it.
  ///
  /// This is not how you spawn a task in a detached state! For that, just call
  /// spawn() and discard the return value.
  inline aw_run_early<result_t, result_t> run_early() {
    did_await = true; // prevent this from posting afterward
    return aw_run_early<result_t, result_t>(
      wrapped, prio, executor, continuation_executor
    );
  }
};

template <IsVoid result_t> class aw_spawned_task<result_t> {
  using wrapped_t = task<result_t>;
  detail::type_erased_executor* executor;
  detail::type_erased_executor* continuation_executor;
  wrapped_t wrapped;
  size_t prio;
  bool did_await;

public:
  /// It is recommended to call `spawn()` instead of using this constructor
  /// directly.
  aw_spawned_task(wrapped_t wrapped)
      : executor(detail::this_thread::executor),
        continuation_executor(detail::this_thread::executor), wrapped(wrapped),
        prio(detail::this_thread::this_task.prio), did_await(false) {}

  /// Always suspends.
  constexpr bool await_ready() const noexcept { return false; }

  /// Suspends the outer coroutine, submits the wrapped task to the
  /// executor, and waits for it to complete.
  constexpr void await_suspend(std::coroutine_handle<> outer) noexcept {
    assert(!did_await);
    did_await = true;
    auto& p = wrapped.promise();
    p.continuation = outer.address();
    p.continuation_executor = continuation_executor;
    executor->post(std::coroutine_handle<>(wrapped), prio);
  }

  /// Does nothing.
  constexpr void await_resume() const noexcept {}

  /// For void result_t, if this was not co_await'ed, post it to the executor in
  /// the destructor. This allows spawn() to be invoked as a standalone
  /// function to create detached tasks.
  ~aw_spawned_task() noexcept {
    if (!did_await) {
      executor->post(std::coroutine_handle<>(wrapped), prio);
    }
  }
  aw_spawned_task(const aw_spawned_task&) = delete;
  aw_spawned_task& operator=(const aw_spawned_task&) = delete;
  aw_spawned_task(aw_spawned_task&& other) {
    executor = std::move(other.executor);
    wrapped = std::move(other.wrapped);
    prio = other.prio;
    did_await = other.did_await;
    other.did_await = true; // prevent other from posting
  }
  aw_spawned_task& operator=(aw_spawned_task&& other) {
    executor = std::move(other.executor);
    wrapped = std::move(other.wrapped);
    prio = other.prio;
    did_await = other.did_await;
    other.did_await = true; // prevent other from posting
    return *this;
  }

  /// When the spawned task completes, the awaiting coroutine will be resumed
  /// on the provided executor.
  inline aw_spawned_task& resume_on(detail::type_erased_executor* e) {
    continuation_executor = e;
    return *this;
  }
  /// When the spawned task completes, the awaiting coroutine will be resumed
  /// on the provided executor.
  template <detail::TypeErasableExecutor Exec>
  aw_spawned_task& resume_on(Exec& executor) {
    return resume_on(executor.type_erased());
  }
  /// When the spawned task completes, the awaiting coroutine will be resumed
  /// on the provided executor.
  template <detail::TypeErasableExecutor Exec>
  aw_spawned_task& resume_on(Exec* executor) {
    return resume_on(executor->type_erased());
  }

  /// The wrapped task will run on the provided executor.
  inline aw_spawned_task& run_on(detail::type_erased_executor* e) {
    executor = e;
    return *this;
  }
  /// The wrapped task will run on the provided executor.
  template <detail::TypeErasableExecutor Exec>
  aw_spawned_task& run_on(Exec& executor) {
    return run_on(executor.type_erased());
  }
  /// The wrapped task will run on the provided executor.
  template <detail::TypeErasableExecutor Exec>
  aw_spawned_task& run_on(Exec* executor) {
    return run_on(executor->type_erased());
  }

  /// Sets the priority of the wrapped task. If co_awaited, the outer
  /// coroutine will also be resumed with this priority.
  inline aw_spawned_task& with_priority(size_t priority) {
    prio = priority;
    return *this;
  }

  /// Submits the wrapped task immediately, without suspending the current
  /// coroutine. You must await the return type before destroying it.
  ///
  /// This is not how you spawn a task in a detached state! For that, just call
  /// spawn() and discard the return value.
  inline aw_run_early<result_t, result_t> run_early() {
    did_await = true; // prevent this from posting afterward
    return aw_run_early<result_t, result_t>(
      wrapped, prio, executor, continuation_executor
    );
  }
};

} // namespace tmc
