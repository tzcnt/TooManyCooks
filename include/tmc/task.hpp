#pragma once
#include "tmc/detail/concepts.hpp"
#include "tmc/detail/thread_locals.hpp"
#include <atomic>
#include <coroutine>
#include <exception>
#include <functional>
#include <type_traits>

namespace tmc {
template <typename result_t> struct task;

namespace detail {

template <typename result_t> struct task_promise;

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
template <typename result_t> struct mt1_continuation_resumer {
  static_assert(sizeof(void*) == sizeof(std::coroutine_handle<>));
  static_assert(alignof(void*) == alignof(std::coroutine_handle<>));
  static_assert(std::is_trivially_copyable_v<std::coroutine_handle<>>);
  static_assert(std::is_trivially_destructible_v<std::coroutine_handle<>>);
  constexpr bool await_ready() const noexcept { return false; }
  constexpr void await_resume() const noexcept {}

  constexpr std::coroutine_handle<>
  await_suspend(std::coroutine_handle<task_promise<result_t>> handle
  ) const noexcept {
    auto& p = handle.promise();
    void* raw_continuation = p.continuation;
    if (p.done_count == nullptr) {
      // solo task, lazy execution
      // continuation is a std::coroutine_handle<>
      // continuation_executor is a detail::type_erased_executor*
      std::coroutine_handle<> continuation =
        std::coroutine_handle<>::from_address(raw_continuation);
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
      handle.destroy();
      return next;
    } else { // p.done_count != nullptr
      // many task and/or eager execution
      // task is part of a spawn_many group, or eagerly executed
      // continuation is a std::coroutine_handle<>*
      // continuation_executor is a detail::type_erased_executor**

      std::coroutine_handle<> next;
      if (p.done_count->fetch_sub(1, std::memory_order_acq_rel) == 0) {
        std::coroutine_handle<> continuation =
          *(static_cast<std::coroutine_handle<>*>(raw_continuation));
        if (continuation) {
          detail::type_erased_executor* continuation_executor =
            *static_cast<detail::type_erased_executor**>(p.continuation_executor
            );
          if (this_thread::executor == continuation_executor) {
            next = continuation;
          } else {
            continuation_executor->post(
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
      handle.destroy();
      return next;
    }
  }
};

template <IsNotVoid result_t> struct task_promise<result_t> {
  task_promise()
      : continuation{nullptr}, continuation_executor{this_thread::executor},
        done_count{nullptr}, result_ptr{nullptr} {}
  constexpr std::suspend_always initial_suspend() const noexcept { return {}; }
  constexpr mt1_continuation_resumer<result_t> final_suspend() const noexcept {
    return {};
  }
  task<result_t> get_return_object() noexcept {
    return {task<result_t>::from_promise(*this)};
  }
  void unhandled_exception() {
    throw;
    // exc = std::current_exception();
  }

  void return_value(result_t&& value) { *result_ptr = std::move(value); }
  void return_value(const result_t& value) { *result_ptr = value; }

  void* continuation;
  void* continuation_executor;
  std::atomic<int64_t>* done_count;
  result_t* result_ptr;
  // std::exception_ptr exc;
};

template <IsVoid result_t> struct task_promise<result_t> {
  task_promise()
      : continuation{nullptr}, continuation_executor{this_thread::executor},
        done_count{nullptr} {}
  constexpr std::suspend_always initial_suspend() const noexcept { return {}; }
  constexpr mt1_continuation_resumer<result_t> final_suspend() const noexcept {
    return {};
  }
  task<result_t> get_return_object() noexcept {
    return {task<result_t>::from_promise(*this)};
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

template <typename result_t> class aw_task;

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
template <typename result_t>
struct task : std::coroutine_handle<detail::task_promise<result_t>> {
  using result_type = result_t;
  using promise_type = detail::task_promise<result_t>;
  aw_task<result_t> operator co_await() { return aw_task<result_t>(*this); }

  /// When this task completes, the awaiting coroutine will be resumed
  /// on the provided executor.
  inline task& resume_on(detail::type_erased_executor* e) {
    std::coroutine_handle<detail::task_promise<result_t>>::promise()
      .continuation_executor = e;
    return *this;
  }
  /// When this task completes, the awaiting coroutine will be resumed
  /// on the provided executor.
  template <detail::TypeErasableExecutor Exec> task& resume_on(Exec& executor) {
    return resume_on(executor.type_erased());
  }
  /// When this task completes, the awaiting coroutine will be resumed
  /// on the provided executor.
  template <detail::TypeErasableExecutor Exec> task& resume_on(Exec* executor) {
    return resume_on(executor->type_erased());
  }
};

template <IsNotVoid result_t> class aw_task<result_t> {
  task<result_t> handle;
  result_t result;

  friend struct task<result_t>;
  constexpr aw_task(const task<result_t>& handle_in) : handle(handle_in) {}

public:
  constexpr bool await_ready() const noexcept { return handle.done(); }
  constexpr std::coroutine_handle<> await_suspend(std::coroutine_handle<> outer
  ) noexcept {
    auto& p = handle.promise();
    p.continuation = outer.address();
    p.result_ptr = &result;
    return handle;
  }
  constexpr result_t await_resume() const noexcept { return result; }
};

template <IsVoid result_t> class aw_task<result_t> {
  task<result_t> inner;

  friend struct task<result_t>;
  constexpr aw_task(const task<result_t>& handle_in) : inner(handle_in) {}

public:
  constexpr bool await_ready() const noexcept { return inner.done(); }
  constexpr std::coroutine_handle<> await_suspend(std::coroutine_handle<> outer
  ) const noexcept {
    auto& p = inner.promise();
    p.continuation = outer.address();
    return inner;
  }
  constexpr void await_resume() const noexcept {}
};

/// Submits `coro` for execution on `ex` at priority `priority`.
template <typename E, typename T>
void post(E& ex, T&& coro, size_t priority)
  requires(std::is_convertible_v<T, std::coroutine_handle<>>)
{
  ex.post(std::coroutine_handle<>(std::forward<T>(coro)), priority);
}
/// Invokes `func()` to get coroutine `coro`. Submits `coro` for execution on
/// `ex` at priority `priority`.
template <typename E, typename T>
void post(E& ex, T&& func, size_t priority)
  requires(!std::is_convertible_v<T, std::coroutine_handle<>> && std::is_convertible_v<std::invoke_result_t<T>, std::coroutine_handle<>>)
{
  ex.post(std::coroutine_handle<>(func()), priority);
}
#if WORK_ITEM_IS(CORO)

/// Submits void-returning `func` for execution on `ex` at priority `priority`.
/// Functions that return values cannot be submitted this way; see
/// `post_waitable` instead.
template <typename E, typename T>
void post(E& ex, T&& func, size_t priority)
  requires(!std::is_convertible_v<T, std::coroutine_handle<>> && std::is_void_v<std::invoke_result_t<T>>)
{
  ex.post(
    std::coroutine_handle<>([](T t) -> task<void> {
      t();
      co_return;
    }(std::forward<T>(func))),
    priority
  );
}
#else
/// Submits void-returning `func` for execution on `ex` at priority `priority`.
/// Functions that return values cannot be submitted this way; see
/// `post_waitable` instead.
template <typename E, typename T>
void post(E& ex, T&& item, size_t priority)
  requires(!std::is_convertible_v<T, std::coroutine_handle<>> && std::is_void_v<std::invoke_result_t<T>>)
{
  ex.post(std::forward<T>(item), priority);
}
#endif

/// Reads `count` items from `it` and submits them for execution on `ex` at
/// priority `priority`.
/// `count` must be non-zero.
/// `It` must be an iterator type that implements `operator*()` and
/// `It& operator++()`.
template <typename E, typename Iter>
void post_bulk(E& ex, Iter it, size_t priority, size_t count) {
  ex.post_bulk(it, priority, count);
}
} // namespace tmc
