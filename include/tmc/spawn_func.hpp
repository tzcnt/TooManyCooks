#pragma once
#include "tmc/detail/concepts.hpp"
#include "tmc/detail/thread_locals.hpp"
#include "tmc/task.hpp"
#include <cassert>
#include <coroutine>
#include <functional>
#include <mutex>

namespace tmc {

/// The customizable task wrapper / awaitable type returned by
/// `tmc::spawn(std::function)`.
template <typename result_t> class aw_spawned_func;

/// Wraps a function into a new task. You can customize this task by
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
template <typename result_t, typename... Args>
aw_spawned_func<result_t>
spawn(std::function<result_t(Args...)> func, Args... args) {
  return aw_spawned_func<result_t>(std::bind(func, args...));
}

template <IsNotVoid result_t>
class [[nodiscard(
  "You must co_await the return of spawn(std::function<result_t(Args...)>) "
  "if result_t is not void."
)]] aw_spawned_func<result_t> {
  using wrapped_t = std::function<result_t()>;
  detail::type_erased_executor* executor;
  wrapped_t wrapped;
  result_t result;
  size_t prio;
  bool did_await;

public:
  /// It is recommended to call `spawn()` instead of using this constructor
  /// directly.
  aw_spawned_func(wrapped_t&& wrapped)
      : executor(detail::this_thread::executor), wrapped(std::move(wrapped)),
        prio(detail::this_thread::this_task.prio), did_await(false) {}

  /// Always suspends.
  constexpr bool await_ready() const noexcept { return false; }

  /// Suspends the outer coroutine, submits the wrapped function to the
  /// executor, and waits for it to complete.
  constexpr void await_suspend(std::coroutine_handle<> outer) noexcept {
    did_await = true;
#if WORK_ITEM_IS(CORO)
    auto t = [](aw_spawned_func* me) -> task<void> {
      me->result = me->wrapped();
      co_return;
    }(this);
    auto& p = t.promise();
    p.continuation = outer.address();
    executor->post(t, prio);
#else
    executor->post(
      [this, outer, continuation_executor = detail::this_thread::executor]() {
        result = wrapped();
        if (continuation_executor == detail::this_thread::executor) {
          outer.resume();
        } else {
          continuation_executor->post(
            outer, detail::this_thread::this_task.prio
          );
        }
      },
      prio
    );
#endif
  }

  /// Returns the value provided by the wrapped function.
  constexpr result_t& await_resume() & noexcept { return result; }

  /// Returns the value provided by the wrapped function.
  constexpr result_t&& await_resume() && noexcept { return std::move(result); }

  ~aw_spawned_func() noexcept {
    // If you spawn a function that returns a non-void type,
    // then you must co_await the return of spawn!
    assert(did_await);
  }
  aw_spawned_func(const aw_spawned_func&) = delete;
  aw_spawned_func& operator=(const aw_spawned_func&) = delete;
  aw_spawned_func(aw_spawned_func&& other) {
    wrapped = std::move(other.wrapped);
    result = std::move(other.result);
    prio = other.prio;
    did_await = other.did_await;
    other.did_await = true; // prevent other from posting
  }
  aw_spawned_func& operator=(aw_spawned_func&& other) {
    wrapped = std::move(other.wrapped);
    result = std::move(other.result);
    prio = other.prio;
    did_await = other.did_await;
    other.did_await = true; // prevent other from posting
    return *this;
  }

  /// After the spawned function completes, the outer coroutine will be resumed
  /// on the provided executor.
  inline aw_spawned_func& resume_on(detail::type_erased_executor* e) {
    wrapped.promise().continuation_executor = e;
    return *this;
  }
  /// After the spawned function completes, the outer coroutine will be resumed
  /// on the provided executor.
  template <detail::TypeErasableExecutor Exec>
  aw_spawned_func& resume_on(Exec& executor) {
    return resume_on(executor.type_erased());
  }
  /// After the spawned function completes, the outer coroutine will be resumed
  /// on the provided executor.
  template <detail::TypeErasableExecutor Exec>
  aw_spawned_func& resume_on(Exec* executor) {
    return resume_on(executor->type_erased());
  }

  /// The wrapped function will run on the provided executor.
  inline aw_spawned_func& run_on(detail::type_erased_executor* e) {
    executor = e;
    return *this;
  }
  /// The wrapped function will run on the provided executor.
  template <detail::TypeErasableExecutor Exec>
  aw_spawned_func& run_on(Exec& executor) {
    return run_on(executor.type_erased());
  }
  /// The wrapped function will run on the provided executor.
  template <detail::TypeErasableExecutor Exec>
  aw_spawned_func& run_on(Exec* executor) {
    return run_on(executor->type_erased());
  }

  /// Sets the priority of the wrapped function. If co_awaited, the outer
  /// coroutine will also be resumed with this priority.
  inline aw_spawned_func& with_priority(size_t priority) {
    prio = priority;
    return *this;
  }
};

template <IsVoid result_t> class aw_spawned_func<result_t> {
  using wrapped_t = std::function<result_t()>;
  detail::type_erased_executor* executor;
  wrapped_t wrapped;
  size_t prio;
  bool did_await;

public:
  /// It is recommended to call `spawn()` instead of using this constructor
  /// directly.
  aw_spawned_func(wrapped_t&& wrapped)
      : executor(detail::this_thread::executor), wrapped(std::move(wrapped)),
        prio(detail::this_thread::this_task.prio), did_await(false) {}

  /// Always suspends.
  constexpr bool await_ready() const noexcept { return false; }

  /// Suspends the outer coroutine, submits the wrapped function to the
  /// executor, and waits for it to complete.
  constexpr void await_suspend(std::coroutine_handle<> outer) noexcept {
    did_await = true;
#if WORK_ITEM_IS(CORO)
    auto t = [](aw_spawned_func* me) -> task<void> {
      me->wrapped();
      co_return;
    }(this);
    auto& p = t.promise();
    p.continuation = outer.address();
    executor->post(t, prio);
#else
    executor->post(
      [this, outer, continuation_executor = detail::this_thread::executor]() {
        wrapped();
        if (continuation_executor == detail::this_thread::executor) {
          outer.resume();
        } else {
          continuation_executor->post(
            outer, detail::this_thread::this_task.prio
          );
        }
      },
      prio
    );
#endif
  }

  /// Does nothing.
  constexpr void await_resume() const noexcept {}

  /// For void result_t, if this was not co_await'ed, post it to the executor in
  /// the destructor. This allows spawn() to be invoked as a standalone
  /// function to create detached tasks.
  ~aw_spawned_func() noexcept {
    if (!did_await) {
#if WORK_ITEM_IS(CORO)
      executor->post(
        [](wrapped_t func) -> task<void> {
          func();
          co_return;
        }(wrapped),
        prio
      );
#else
      executor->post(std::move(wrapped), prio);
#endif
    }
  }

  aw_spawned_func(const aw_spawned_func&) = delete;
  aw_spawned_func& operator=(const aw_spawned_func&) = delete;
  aw_spawned_func(aw_spawned_func&& other) {
    wrapped = std::move(other.wrapped);
    prio = other.prio;
    did_await = other.did_await;
    other.did_await = true; // prevent other from posting
  }
  aw_spawned_func& operator=(aw_spawned_func&& other) {
    wrapped = std::move(other.wrapped);
    prio = other.prio;
    did_await = other.did_await;
    other.did_await = true; // prevent other from posting
    return *this;
  }

  /// When awaited, the outer coroutine will be resumed on the provided
  /// executor.
  inline aw_spawned_func& resume_on(detail::type_erased_executor* e) {
    wrapped.promise().continuation_executor = e;
    return *this;
  }

  /// When awaited, the outer coroutine will be resumed on the provided
  /// executor.
  template <detail::TypeErasableExecutor Exec>
  aw_spawned_func& resume_on(Exec& executor) {
    return resume_on(executor.type_erased());
  }

  /// When awaited, the outer coroutine will be resumed on the provided
  /// executor.
  template <detail::TypeErasableExecutor Exec>
  aw_spawned_func& resume_on(Exec* executor) {
    return resume_on(executor->type_erased());
  }

  /// The wrapped function will be submitted to the provided executor.
  inline aw_spawned_func& run_on(detail::type_erased_executor* e) {
    executor = e;
    return *this;
  }

  /// The wrapped function will be submitted to the provided executor.
  template <detail::TypeErasableExecutor Exec>
  aw_spawned_func& run_on(Exec& executor) {
    return run_on(executor.type_erased());
  }

  /// The wrapped function will be submitted to the provided executor.
  template <detail::TypeErasableExecutor Exec>
  aw_spawned_func& run_on(Exec* executor) {
    return run_on(executor->type_erased());
  }

  /// Sets the priority of the wrapped function. If co_awaited, the outer
  /// coroutine will also be resumed with this priority.
  inline aw_spawned_func& with_priority(size_t priority) {
    prio = priority;
    return *this;
  }
};

} // namespace tmc
