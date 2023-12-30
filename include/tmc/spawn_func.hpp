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

/// Wraps a function into a new task. You can customize this task by
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
template <typename Result, typename... Arguments>
aw_spawned_func<Result>
spawn(std::function<Result(Arguments...)> Func, Arguments... Args) {
  return aw_spawned_func<Result>(std::bind(Func, Args...));
}

template <IsNotVoid Result>
class [[nodiscard(
  "You must co_await the return of spawn(std::function<Result(Args...)>) "
  "if Result is not void."
)]] aw_spawned_func<Result> {
  detail::type_erased_executor* executor;
  std::function<Result()> wrapped;
  Result result;
  size_t prio;
  bool did_await;

public:
  /// It is recommended to call `spawn()` instead of using this constructor
  /// directly.
  aw_spawned_func(std::function<Result()>&& Func)
      : executor(detail::this_thread::executor), wrapped(std::move(Func)),
        prio(detail::this_thread::this_task.prio), did_await(false) {}

  /// Always suspends.
  constexpr bool await_ready() const noexcept { return false; }

  /// Suspends the outer coroutine, submits the wrapped function to the
  /// executor, and waits for it to complete.
  constexpr void await_suspend(std::coroutine_handle<> Outer) noexcept {
    did_await = true;
#if WORK_ITEM_IS(CORO)
    auto t = [](aw_spawned_func* me) -> task<void> {
      me->result = me->wrapped();
      co_return;
    }(this);
    auto& p = t.promise();
    p.continuation = Outer.address();
    executor->post(t, prio);
#else
    executor->post(
      [this, Outer, continuation_executor = detail::this_thread::executor]() {
        result = wrapped();
        if (continuation_executor == detail::this_thread::executor) {
          Outer.resume();
        } else {
          continuation_executor->post(
            Outer, detail::this_thread::this_task.prio
          );
        }
      },
      prio
    );
#endif
  }

  /// Returns the value provided by the wrapped function.
  constexpr Result& await_resume() & noexcept { return result; }

  /// Returns the value provided by the wrapped function.
  constexpr Result&& await_resume() && noexcept { return std::move(result); }

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
    wrapped.promise().continuation_executor = Executor;
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

template <IsVoid Result> class aw_spawned_func<Result> {
  detail::type_erased_executor* executor;
  std::function<Result()> wrapped;
  size_t prio;
  bool did_await;

public:
  /// It is recommended to call `spawn()` instead of using this constructor
  /// directly.
  aw_spawned_func(std::function<Result()>&& Func)
      : executor(detail::this_thread::executor), wrapped(std::move(Func)),
        prio(detail::this_thread::this_task.prio), did_await(false) {}

  /// Always suspends.
  constexpr bool await_ready() const noexcept { return false; }

  /// Suspends the outer coroutine, submits the wrapped function to the
  /// executor, and waits for it to complete.
  constexpr void await_suspend(std::coroutine_handle<> Outer) noexcept {
    did_await = true;
#if WORK_ITEM_IS(CORO)
    auto t = [](aw_spawned_func* me) -> task<void> {
      me->wrapped();
      co_return;
    }(this);
    auto& p = t.promise();
    p.continuation = Outer.address();
    executor->post(t, prio);
#else
    executor->post(
      [this, Outer, continuation_executor = detail::this_thread::executor]() {
        wrapped();
        if (continuation_executor == detail::this_thread::executor) {
          Outer.resume();
        } else {
          continuation_executor->post(
            Outer, detail::this_thread::this_task.prio
          );
        }
      },
      prio
    );
#endif
  }

  /// Does nothing.
  constexpr void await_resume() const noexcept {}

  /// For void Result, if this was not co_await'ed, post it to the executor in
  /// the destructor. This allows spawn() to be invoked as a standalone
  /// function to create detached tasks.
  ~aw_spawned_func() noexcept {
    if (!did_await) {
#if WORK_ITEM_IS(CORO)
      executor->post(
        [](std::function<Result()> Func) -> task<void> {
          Func();
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
    wrapped.promise().continuation_executor = Executor;
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

} // namespace tmc
