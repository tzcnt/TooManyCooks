#pragma once
#include "tmc/detail/concepts.hpp" // IWYU pragma: keep
#include "tmc/detail/thread_locals.hpp"
#include <atomic>
#include <coroutine>
#include <type_traits>

namespace tmc {
template <typename Result> struct task;

namespace detail {

template <typename Result> struct task_promise;

/// "many-to-one" multipurpose final_suspend type for tmc::task.
///
/// `done_count` is used as an atomic barrier to synchronize with other tasks in
/// the same spawn group (in the case of spawn_many()), or the awaiting task (in
/// the case of run_early()). In other scenarios, `done_count` is unused,and is
/// expected to be nullptr.
///
/// If `done_count` is nullptr, `continuation` and `continuation_executor` are
/// used directly.
///
/// If `done_count` is not nullptr, `continuation` and `continuation_executor`
/// are indirected. This allows them to be changed simultaneously for many tasks
/// in the same group.
template <typename Result> struct mt1_continuation_resumer {
  static_assert(sizeof(void*) == sizeof(std::coroutine_handle<>));
  static_assert(alignof(void*) == alignof(std::coroutine_handle<>));
  static_assert(std::is_trivially_copyable_v<std::coroutine_handle<>>);
  static_assert(std::is_trivially_destructible_v<std::coroutine_handle<>>);
  constexpr bool await_ready() const noexcept { return false; }
  constexpr void await_resume() const noexcept {}

  constexpr std::coroutine_handle<>
  await_suspend(std::coroutine_handle<task_promise<Result>> Handle
  ) const noexcept {
    auto& p = Handle.promise();
    void* rawContinuation = p.continuation;
    if (p.done_count == nullptr) {
      // solo task, lazy execution
      // continuation is a std::coroutine_handle<>
      // continuation_executor is a detail::type_erased_executor*
      std::coroutine_handle<> continuation =
        std::coroutine_handle<>::from_address(rawContinuation);
      std::coroutine_handle<> next;
      if (continuation) {
        if (this_thread::executor == p.continuation_executor) {
          next = continuation;
        } else {
          static_cast<detail::type_erased_executor*>(p.continuation_executor)
            ->post(std::move(continuation), this_thread::this_task.prio);
          next = std::noop_coroutine();
        }
      } else {
        next = std::noop_coroutine();
      }
      Handle.destroy();
      return next;
    } else { // p.done_count != nullptr
      // many task and/or eager execution
      // task is part of a spawn_many group, or eagerly executed
      // continuation is a std::coroutine_handle<>*
      // continuation_executor is a detail::type_erased_executor**

      std::coroutine_handle<> next;
      if (p.done_count->fetch_sub(1, std::memory_order_acq_rel) == 0) {
        std::coroutine_handle<> continuation =
          *(static_cast<std::coroutine_handle<>*>(rawContinuation));
        if (continuation) {
          detail::type_erased_executor* continuationExecutor =
            *static_cast<detail::type_erased_executor**>(p.continuation_executor
            );
          if (this_thread::executor == continuationExecutor) {
            next = continuation;
          } else {
            continuationExecutor->post(
              std::move(continuation), this_thread::this_task.prio
            );
            next = std::noop_coroutine();
          }
        } else {
          next = std::noop_coroutine();
        }
      } else {
        next = std::noop_coroutine();
      }
      Handle.destroy();
      return next;
    }
  }
};

template <IsNotVoid Result> struct task_promise<Result> {
  task_promise()
      : continuation{nullptr}, continuation_executor{this_thread::executor},
        done_count{nullptr}, result_ptr{nullptr} {}
  constexpr std::suspend_always initial_suspend() const noexcept { return {}; }
  constexpr mt1_continuation_resumer<Result> final_suspend() const noexcept {
    return {};
  }
  task<Result> get_return_object() noexcept {
    return {task<Result>::from_promise(*this)};
  }
  void unhandled_exception() {
    throw;
    // exc = std::current_exception();
  }

  void return_value(Result&& Value) { *result_ptr = std::move(Value); }
  void return_value(const Result& Value) { *result_ptr = Value; }

  void* continuation;
  void* continuation_executor;
  std::atomic<int64_t>* done_count;
  Result* result_ptr;
  // std::exception_ptr exc;
};

template <IsVoid Result> struct task_promise<Result> {
  task_promise()
      : continuation{nullptr}, continuation_executor{this_thread::executor},
        done_count{nullptr} {}
  constexpr std::suspend_always initial_suspend() const noexcept { return {}; }
  constexpr mt1_continuation_resumer<Result> final_suspend() const noexcept {
    return {};
  }
  task<Result> get_return_object() noexcept {
    return {task<Result>::from_promise(*this)};
  }
  void unhandled_exception() {
    throw;
    // exc = std::current_exception();
  }

  void return_void() {}

  void* continuation;
  void* continuation_executor;
  std::atomic<int64_t>* done_count;
  // std::exception_ptr exc;
};

} // namespace detail

template <typename Result> class aw_task;

/// The main coroutine type used by TooManyCooks. `task` is a lazy / cold
/// coroutine and will not begin running immediately.
/// To start running a `task`, you can:
///
/// Use `co_await` directly on the task to run it and await the results.
///
/// Call `tmc::spawn()` to create a task wrapper that can be configured before
/// `co_await` ing the results.
///
/// Call `tmc::spawn_many()` to submit and await multiple tasks at once. This
/// task group can be configured before `co_await` ing the results.
///
/// Call `tmc::post()` / `tmc::post_waitable()` to submit this task for
/// execution to an async executor from external (non-async) calling code.
template <typename Result>
struct task : std::coroutine_handle<detail::task_promise<Result>> {
  using result_type = Result;
  using promise_type = detail::task_promise<Result>;
  aw_task<Result> operator co_await() { return aw_task<Result>(*this); }

  /// When this task completes, the awaiting coroutine will be resumed
  /// on the provided executor.
  inline task& resume_on(detail::type_erased_executor* Executor) {
    std::coroutine_handle<detail::task_promise<Result>>::promise()
      .continuation_executor = Executor;
    return *this;
  }
  /// When this task completes, the awaiting coroutine will be resumed
  /// on the provided executor.
  template <detail::TypeErasableExecutor Exec> task& resume_on(Exec& Executor) {
    return resume_on(Executor.type_erased());
  }
  /// When this task completes, the awaiting coroutine will be resumed
  /// on the provided executor.
  template <detail::TypeErasableExecutor Exec> task& resume_on(Exec* Executor) {
    return resume_on(Executor->type_erased());
  }
};

template <IsNotVoid Result> class aw_task<Result> {
  task<Result> handle;
  Result result;

  friend struct task<Result>;
  constexpr aw_task(const task<Result>& Handle) : handle(Handle) {}

public:
  constexpr bool await_ready() const noexcept { return handle.done(); }
  constexpr std::coroutine_handle<> await_suspend(std::coroutine_handle<> Outer
  ) noexcept {
    auto& p = handle.promise();
    p.continuation = Outer.address();
    p.result_ptr = &result;
    return handle;
  }
  constexpr Result& await_resume() & noexcept { return result; }
  constexpr Result&& await_resume() && noexcept { return std::move(result); }
};

template <IsVoid Result> class aw_task<Result> {
  task<Result> inner;

  friend struct task<Result>;
  constexpr aw_task(const task<Result>& Handle) : inner(Handle) {}

public:
  constexpr bool await_ready() const noexcept { return inner.done(); }
  constexpr std::coroutine_handle<> await_suspend(std::coroutine_handle<> Outer
  ) const noexcept {
    auto& p = inner.promise();
    p.continuation = Outer.address();
    return inner;
  }
  constexpr void await_resume() const noexcept {}
};

/// Submits `Coro` for execution on `Executor` at priority `Priority`.
template <typename E, typename T>
void post(E& Executor, T&& Coro, size_t Priority)
  requires(std::is_convertible_v<T, std::coroutine_handle<>>)
{
  Executor.post(std::coroutine_handle<>(std::forward<T>(Coro)), Priority);
}
/// Invokes `FuncReturnsCoro()` to get coroutine `coro`. Submits `coro` for
/// execution on `Executor` at priority `Priority`.
template <typename E, typename T>
void post(E& Executor, T&& FuncReturnsCoro, size_t Priority)
  requires(!std::is_convertible_v<T, std::coroutine_handle<>> && std::is_convertible_v<std::invoke_result_t<T>, std::coroutine_handle<>>)
{
  Executor.post(std::coroutine_handle<>(FuncReturnsCoro()), Priority);
}
#if WORK_ITEM_IS(CORO)

/// Submits void-returning `Func` for execution on `Executor` at priority
/// `Priority`. Functions that return values cannot be submitted this way; see
/// `post_waitable` instead.
template <typename E, typename T>
void post(E& Executor, T&& Func, size_t Priority)
  requires(!std::is_convertible_v<T, std::coroutine_handle<>> && std::is_void_v<std::invoke_result_t<T>>)
{
  Executor.post(
    std::coroutine_handle<>([](T t) -> task<void> {
      t();
      co_return;
    }(std::forward<T>(Func))),
    Priority
  );
}
#else
/// Submits void-returning `Func` for execution on `Executor` at priority
/// `Priority`. Functions that return values cannot be submitted this way; see
/// `post_waitable` instead.
template <typename E, typename T>
void post(E& Executor, T&& Func, size_t Priority)
  requires(!std::is_convertible_v<T, std::coroutine_handle<>> && std::is_void_v<std::invoke_result_t<T>>)
{
  Executor.post(std::forward<T>(Func), Priority);
}
#endif

/// Reads `Count` items from `WorkItemIterator` and submits them for execution
/// on `Executor` at priority `Priority`. `Count` must be non-zero.
/// `WorkItemIterator` must be an iterator type that implements `operator*()`
/// and `It& operator++()`.
template <typename E, typename Iter>
void post_bulk(
  E& Executor, Iter WorkItemIterator, size_t Priority, size_t Count
) {
  Executor.post_bulk(WorkItemIterator, Priority, Count);
}
} // namespace tmc
