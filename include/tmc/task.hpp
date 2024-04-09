#pragma once
#include "tmc/detail/concepts.hpp" // IWYU pragma: keep
#include "tmc/detail/thread_locals.hpp"
#include <atomic>
#include <cassert>
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
        if (p.continuation_executor == nullptr || p.continuation_executor == this_thread::executor) {
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
          if (continuationExecutor == nullptr || continuationExecutor == this_thread::executor) {
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
template <typename Result> struct task {
  std::coroutine_handle<detail::task_promise<Result>> handle;
  using result_type = Result;
  using promise_type = detail::task_promise<Result>;

  /// Suspend the outer coroutine and run this task directly. The intermediate
  /// awaitable type `aw_task` cannot be used directly; the return type of the
  /// `co_await` expression will be `Result` or `void`.
  aw_task<Result> operator co_await() && {
    return aw_task<Result>(std::move(*this));
  }

  /// When this task completes, the awaiting coroutine will be resumed
  /// on the provided executor.
  inline task& resume_on(detail::type_erased_executor* Executor) {
    handle.promise().continuation_executor = Executor;
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

  /// This is useless since it can't be invoked by mt1_continuation_resumer
  /// final_suspend. It gets passed a std::coroutine_handle<promise_type> by
  /// value...
  // void finish() {
  //   std::coroutine_handle<promise_type>::destroy();
  //   std::coroutine_handle<promise_type>::operator=(nullptr);
  // }

  // This can be nulled out in move constructor, or in await_resume of aw_task,
  // aw_spawned_task, etc. But what about detached tasks? Maybe they are OK
  // since they aren't "task" in the queue, they are just a functor... So as
  // long as they are moved-from when submitting, it's valid.

  constexpr task() noexcept : handle(nullptr) {}

#ifndef TMC_TRIVIAL_TASK
  /// Tasks are move-only
  task(std::coroutine_handle<promise_type>&& other) noexcept {
    handle = other;
    other = nullptr;
  }
  task& operator=(std::coroutine_handle<promise_type>&& other) noexcept {
    handle = other;
    other = nullptr;
    return *this;
  }

  task(task&& other) noexcept {
    handle = other.handle;
    other.handle = nullptr;
  }

  task& operator=(task&& other) noexcept {
    handle = other.handle;
    other.handle = nullptr;
    return *this;
  }

  /// Non-copyable
  task(const task& other) = delete;
  task& operator=(const task& other) = delete;

  /// When this task is destroyed, it should already have been deinitialized.
  /// Either because it was moved-from, or because the coroutine completed.
  ~task() { assert(!handle); }
#endif

  /// Conversion to a std::coroutine_handle<> is move-only
  operator std::coroutine_handle<>() && noexcept {
    auto addr = handle.address();
#ifndef TMC_TRIVIAL_TASK
    handle = nullptr;
#endif
    return std::coroutine_handle<>::from_address(addr);
  }

  /// Conversion to a std::coroutine_handle<> is move-only
  operator std::coroutine_handle<promise_type>() && noexcept {
    auto addr = handle.address();
#ifndef TMC_TRIVIAL_TASK
    handle = nullptr;
#endif
    return std::coroutine_handle<promise_type>::from_address(addr);
  }

  static constexpr task from_address(void* addr) noexcept {
    task t;
    t.handle = std::coroutine_handle<promise_type>::from_address(addr);
    return t;
  }

  static task from_promise(promise_type& prom) {
    task t;
    t.handle = std::coroutine_handle<promise_type>::from_promise(prom);
    return t;
  }

  bool done() const noexcept { return handle.done(); }

  constexpr void* address() const noexcept { return handle.address(); }

  // std::coroutine_handle::destroy() is const, but this isn't - it nulls the
  // pointer afterward
  void destroy() noexcept {
    handle.destroy();
#ifndef TMC_TRIVIAL_TASK
    handle = nullptr;
#endif
  }

  void resume() const { handle.resume(); }
  void operator()() const { handle.resume(); }

  operator bool() const noexcept { return handle.operator bool(); }

  auto& promise() const { return handle.promise(); }
};
namespace detail {

template <typename Result> struct task_promise {
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

  void return_value(Result&& Value) {
    *result_ptr = static_cast<Result&&>(Value);
  }

  void return_value(Result const& Value)
    requires(!std::is_reference_v<Result>)
  {
    *result_ptr = Value;
  }

  void* continuation;
  void* continuation_executor;
  std::atomic<int64_t>* done_count;
  Result* result_ptr;
  // std::exception_ptr exc;
};

template <> struct task_promise<void> {
  task_promise()
      : continuation{nullptr}, continuation_executor{this_thread::executor},
        done_count{nullptr} {}
  constexpr std::suspend_always initial_suspend() const noexcept { return {}; }
  constexpr mt1_continuation_resumer<void> final_suspend() const noexcept {
    return {};
  }
  task<void> get_return_object() noexcept {
    return {task<void>::from_promise(*this)};
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

template <typename Result> class aw_task {
  task<Result> handle;
  Result result;

  friend struct task<Result>;
  aw_task(task<Result>&& Handle) : handle(std::move(Handle)) {}

public:
  constexpr bool await_ready() const noexcept { return handle.done(); }
  constexpr std::coroutine_handle<> await_suspend(std::coroutine_handle<> Outer
  ) noexcept {
    auto& p = handle.promise();
    p.continuation = Outer.address();
    p.result_ptr = &result;
    return std::move(handle);
  }

  /// Returns the value provided by the awaited task.
  constexpr Result& await_resume() & noexcept { return result; }

  /// Returns the value provided by the awaited task.
  constexpr Result&& await_resume() && noexcept {
    // This appears to never be used - the 'this' parameter to
    // await_resume() is always an lvalue
    return std::move(result);
  }
};

template <> class aw_task<void> {
  task<void> handle;

  friend struct task<void>;
  inline aw_task(task<void>&& Handle) : handle(std::move(Handle)) {}

public:
  constexpr bool await_ready() const noexcept { return handle.done(); }
  constexpr std::coroutine_handle<> await_suspend(std::coroutine_handle<> Outer
  ) noexcept {
    auto& p = handle.promise();
    p.continuation = Outer.address();
    return std::move(handle);
  }
  constexpr void await_resume() noexcept {}
};

/// Submits `Coro` for execution on `Executor` at priority `Priority`.
template <typename E, typename C>
void post(E& Executor, C&& Coro, size_t Priority)
  requires(std::is_convertible_v<C, std::coroutine_handle<>>)
{
  Executor.post(std::coroutine_handle<>(std::forward<C>(Coro)), Priority);
}

#if WORK_ITEM_IS(CORO)
/// Submits void-returning `Func` for execution on `Executor` at priority
/// `Priority`. Functions that return values cannot be submitted this way; see
/// `post_waitable` instead.
template <typename E, typename F>
void post(E& Executor, F&& Func, size_t Priority)
  requires(!std::is_convertible_v<F, std::coroutine_handle<>> && std::is_void_v<std::invoke_result_t<F>>)
{
  Executor.post(
    std::coroutine_handle<>([](F t) -> task<void> {
      t();
      co_return;
    }(std::forward<F>(Func))),
    Priority
  );
}
#else
/// Submits void-returning `Func` for execution on `Executor` at priority
/// `Priority`. Functions that return values cannot be submitted this way; see
/// `post_waitable` instead.
template <typename E, typename F>
void post(E& Executor, F&& Func, size_t Priority)
  requires(!std::is_convertible_v<F, std::coroutine_handle<>> && std::is_void_v<std::invoke_result_t<F>>)
{
  Executor.post(std::forward<F>(Func), Priority);
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
