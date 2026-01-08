// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/awaitable_customizer.hpp"
#include "tmc/detail/compat.hpp"
#include "tmc/detail/concepts_awaitable.hpp" // IWYU pragma: keep
#include "tmc/detail/task_wrapper.hpp"       // IWYU pragma: keep
#include "tmc/ex_any.hpp"

#include <cassert>
#include <coroutine>
#include <new>
#include <type_traits>

#ifdef TMC_DEBUG_TASK_ALLOC_COUNT
#include <atomic>
#endif

#if TMC_HAS_EXCEPTIONS
#include <exception>
#endif

namespace tmc {
namespace detail {
#ifdef TMC_DEBUG_TASK_ALLOC_COUNT
inline std::atomic<size_t> g_task_alloc_count;
#endif

template <typename Result> struct task_promise;
} // namespace detail

#ifdef TMC_DEBUG_TASK_ALLOC_COUNT
namespace debug {
/// Returns the current value of the tmc::task allocation counter.
/// This is useful to determine if HALO is working; tasks that have their
/// allocations folded into the parent allocation by HALO do not increase this
/// counter.
inline size_t get_task_alloc_count() {
  return tmc::detail::g_task_alloc_count.load(std::memory_order_seq_cst);
}

/// Allows you to reset the tmc::task allocation counter, in order to
/// count the number of allocations in a specific program section.
inline void set_task_alloc_count(size_t Value) {
  tmc::detail::g_task_alloc_count.store(Value, std::memory_order_seq_cst);
}
} // namespace debug
#endif

template <typename Awaitable, typename Result> class aw_task;

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
struct [[nodiscard(
  "You must submit or co_await task for execution. Failure to "
  "do so will result in a memory leak."
)]] TMC_CORO_AWAIT_ELIDABLE task {
  using result_type = Result;
  using promise_type = tmc::detail::task_promise<Result>;
  std::coroutine_handle<promise_type> handle;

  /// Suspend the outer coroutine and run this task directly. The intermediate
  /// awaitable type `aw_task` cannot be used directly; the return type of the
  /// `co_await` expression will be `Result` or `void`.
  aw_task<task<Result>, Result> operator co_await() && noexcept {
    return aw_task<task<Result>, Result>(std::move(*this));
  }

  /// When this task completes, the awaiting coroutine will be resumed
  /// on the provided executor.
  [[nodiscard(
    "You must submit or co_await task for execution. Failure to "
    "do so will result in a memory leak."
  )]] inline task&
  resume_on(tmc::ex_any* Executor) & noexcept {
    // This overload is called by the other overloads.
    handle.promise().customizer.continuation_executor = Executor;
    return *this;
  }
  /// When this task completes, the awaiting coroutine will be resumed
  /// on the provided executor.
  template <typename Exec>
  [[nodiscard(
    "You must submit or co_await task for execution. Failure to "
    "do so will result in a memory leak."
  )]] task&
  resume_on(Exec&& Executor) & noexcept {
    return resume_on(
      tmc::detail::get_executor_traits<Exec>::type_erased(Executor)
    );
  }
  /// When this task completes, the awaiting coroutine will be resumed
  /// on the provided executor.
  template <typename Exec>
  [[nodiscard(
    "You must submit or co_await task for execution. Failure to "
    "do so will result in a memory leak."
  )]] task&
  resume_on(Exec* Executor) & noexcept {
    return resume_on(
      tmc::detail::get_executor_traits<Exec>::type_erased(*Executor)
    );
  }

  /// When this task completes, the awaiting coroutine will be resumed
  /// on the provided executor.
  [[nodiscard(
    "You must submit or co_await task for execution. Failure to "
    "do so will result in a memory leak."
  )]] inline task&&
  resume_on(tmc::ex_any* Executor) && noexcept {
    // This overload is called by the other overloads.
    handle.promise().customizer.continuation_executor = Executor;
    return std::move(*this);
  }
  /// When this task completes, the awaiting coroutine will be resumed
  /// on the provided executor.
  template <typename Exec>
  [[nodiscard(
    "You must submit or co_await task for execution. Failure to "
    "do so will result in a memory leak."
  )]] task&&
  resume_on(Exec&& Executor) && noexcept {
    handle.promise().customizer.continuation_executor =
      tmc::detail::get_executor_traits<Exec>::type_erased(Executor);
    return std::move(*this);
  }
  /// When this task completes, the awaiting coroutine will be resumed
  /// on the provided executor.
  template <typename Exec>
  [[nodiscard(
    "You must submit or co_await task for execution. Failure to "
    "do so will result in a memory leak."
  )]] task&&
  resume_on(Exec* Executor) && noexcept {
    handle.promise().customizer.continuation_executor =
      tmc::detail::get_executor_traits<Exec>::type_erased(*Executor);
    return std::move(*this);
  }

  inline task() noexcept : handle(nullptr) {}

#ifndef TMC_TRIVIAL_TASK
  /// Tasks are move-only
  task(std::coroutine_handle<promise_type>&& Other) noexcept {
    handle = Other;
    Other = nullptr;
  }
  task& operator=(std::coroutine_handle<promise_type>&& Other) noexcept {
    handle = Other;
    Other = nullptr;
    return *this;
  }

  task(task&& Other) noexcept {
    handle = Other.handle;
    Other.handle = nullptr;
  }

  task& operator=(task&& Other) noexcept {
    handle = Other.handle;
    Other.handle = nullptr;
    return *this;
  }

  /// Non-copyable
  task(const task& other) = delete;
  task& operator=(const task& other) = delete;

  /// When this task is destroyed, it should already have been deinitialized.
  /// Either because it was moved-from, or because the coroutine completed.
  ~task() { assert(!handle && "You must submit or co_await this."); }
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

  static task from_address(void* addr) noexcept {
    task t;
    t.handle = std::coroutine_handle<promise_type>::from_address(addr);
    return t;
  }

  static task from_promise(promise_type& prom) noexcept {
    task t;
    t.handle = std::coroutine_handle<promise_type>::from_promise(prom);
    return t;
  }

  void* address() const noexcept { return handle.address(); }

  // std::coroutine_handle::destroy() is const, but this isn't - it nulls the
  // pointer afterward
  void destroy() noexcept {
    handle.destroy();
#ifndef TMC_TRIVIAL_TASK
    handle = nullptr;
#endif
  }

  void resume() & noexcept { handle.resume(); }
  void operator()() & noexcept { handle.resume(); }

  void resume() && noexcept {
    handle.resume();
#ifndef TMC_TRIVIAL_TASK
    handle = nullptr;
#endif
  }
  void operator()() && noexcept {
    handle.resume();
#ifndef TMC_TRIVIAL_TASK
    handle = nullptr;
#endif
  }

  explicit operator bool() const noexcept { return handle.operator bool(); }

  auto& promise() const noexcept { return handle.promise(); }
};
namespace detail {

template <typename T>
concept HasAwaitableTraitsConcept = requires {
  // Check whether any function with this name exists
  &tmc::detail::get_awaitable_traits<T>::get_awaiter;
};

template <typename T> struct has_awaitable_traits {
  static constexpr bool value = false;
};
template <HasAwaitableTraitsConcept T> struct has_awaitable_traits<T> {
  static constexpr bool value = true;
};

template <typename Result> struct task_promise {
  awaitable_customizer<Result> customizer;

  task_promise() noexcept {}
  inline std::suspend_always initial_suspend() const noexcept { return {}; }
  inline mt1_continuation_resumer<task_promise> final_suspend() const noexcept {
    return {};
  }
  task<Result> get_return_object() noexcept {
    return {task<Result>::from_promise(*this)};
  }
  [[noreturn]] void unhandled_exception() noexcept { std::terminate(); }

  template <typename RV>
  void
  return_value(RV&& Value) noexcept(std::is_nothrow_move_constructible_v<RV>) {
    *customizer.result_ptr = static_cast<RV&&>(Value);
  }

  template <typename Awaitable>
  decltype(auto) await_transform(Awaitable&& awaitable) noexcept
    requires has_awaitable_traits<Awaitable>::value
  {
    // If you are looking at a compilation error on this line when awaiting
    // a TMC awaitable, you probably need to std::move() whatever you are
    // co_await'ing. co_await std::move(your_tmc_awaitable_variable_name)
    return tmc::detail::get_awaitable_traits<Awaitable>::get_awaiter(
      std::forward<Awaitable>(awaitable)
    );
  }

#ifndef TMC_NO_UNKNOWN_AWAITABLES
  template <typename Awaitable>
  decltype(auto) await_transform(
    Awaitable&& awaitable
  ) noexcept(std::is_nothrow_move_constructible_v<Awaitable>)
    requires(!has_awaitable_traits<Awaitable>::value)
  {
    // If you are awaiting a non-TMC awaitable, then you should consult the
    // documentation there to see why we can't deduce the awaiter type, or
    // specialize tmc::detail::awaitable_traits for it yourself.
    return tmc::detail::safe_wrap(std::forward<Awaitable>(awaitable));
  }
#endif

  // Round up the coroutine allocation to next 64 bytes.
  // This reduces false sharing with adjacent coroutines.
  static void* operator new(std::size_t n) noexcept {
    // This operator new is noexcept. This means that if the allocation
    // throws, std::terminate will be called.
    // I recommend using tcmalloc with TooManyCooks, as it will also directly
    // crash the program rather than throwing an exception:
    // https://github.com/google/tcmalloc/blob/master/docs/reference.md#operator-new--operator-new

#ifdef TMC_DEBUG_TASK_ALLOC_COUNT
    ++tmc::detail::g_task_alloc_count;
#endif

    // DEBUG - Print the size of the coroutine allocation.
    // std::printf("task_promise new %zu -> %zu\n", n, (n + 63) &
    // static_cast<size_t>(-64));
    n = (n + 63) & static_cast<size_t>(-64);
    return ::operator new(n);
  }

  // Aligned new/delete is necessary to support -fcoro-aligned-allocation
  static void* operator new(std::size_t n, std::align_val_t al) noexcept {
#ifdef TMC_DEBUG_TASK_ALLOC_COUNT
    ++tmc::detail::g_task_alloc_count;
#endif

    // std::printf("task_promise new %zu -> %zu\n", n, (n + 63) &
    // static_cast<size_t>(-64));
    n = (n + 63) & static_cast<size_t>(-64);
    return ::operator new(n, al);
  }

#if TMC_SIZED_DEALLOCATION
  static void operator delete(void* ptr, std::size_t n) noexcept {
    n = (n + 63) & static_cast<size_t>(-64);
    return ::operator delete(ptr, n);
  }
  static void
  operator delete(void* ptr, std::size_t n, std::align_val_t al) noexcept {
    n = (n + 63) & static_cast<size_t>(-64);
    return ::operator delete(ptr, n, al);
  }
#endif

#ifndef __clang__
  // GCC creates a TON of warnings if this is missing with the noexcept new
  static task<Result> get_return_object_on_allocation_failure() noexcept {
    return {};
  }
#endif
};

template <> struct task_promise<void> {
  awaitable_customizer<void> customizer;

  task_promise() noexcept {}
  inline std::suspend_always initial_suspend() const noexcept { return {}; }
  inline mt1_continuation_resumer<task_promise> final_suspend() const noexcept {
    return {};
  }
  task<void> get_return_object() noexcept {
    return {task<void>::from_promise(*this)};
  }
  [[noreturn]] void unhandled_exception() noexcept { std::terminate(); }

  void return_void() noexcept {}

  template <typename Awaitable>
  decltype(auto) await_transform(Awaitable&& awaitable) noexcept
    requires has_awaitable_traits<Awaitable>::value
  {
    // If you are looking at a compilation error on this line when awaiting
    // a TMC awaitable, you probably need to std::move() whatever you are
    // co_await'ing. co_await std::move(your_tmc_awaitable_variable_name)
    return tmc::detail::get_awaitable_traits<Awaitable>::get_awaiter(
      std::forward<Awaitable>(awaitable)
    );
  }

#ifndef TMC_NO_UNKNOWN_AWAITABLES
  template <typename Awaitable>
  decltype(auto) await_transform(
    Awaitable&& awaitable
  ) noexcept(std::is_nothrow_move_constructible_v<Awaitable>)
    requires(!has_awaitable_traits<Awaitable>::value)
  {
    // If you are awaiting a non-TMC awaitable, then you should consult the
    // documentation there to see why we can't deduce the awaiter type, or
    // specialize tmc::detail::awaitable_traits for it yourself.
    return tmc::detail::safe_wrap(std::forward<Awaitable>(awaitable));
  }
#endif

#ifdef TMC_DEBUG_TASK_ALLOC_COUNT
  // Round up the coroutine allocation to next 64 bytes.
  // This reduces false sharing with adjacent coroutines.
  static void* operator new(std::size_t n) noexcept {
    // This operator new is noexcept. This means that if the allocation
    // throws, std::terminate will be called.
    // I recommend using tcmalloc with TooManyCooks, as it will also directly
    // crash the program rather than throwing an exception:
    // https://github.com/google/tcmalloc/blob/master/docs/reference.md#operator-new--operator-new

    ++tmc::detail::g_task_alloc_count;

    // DEBUG - Print the size of the coroutine allocation.
    // std::printf("task_promise new %zu\n", n);
    return ::operator new(n);
  }

  // Aligned new/delete is necessary to support -fcoro-aligned-allocation
  static void* operator new(std::size_t n, std::align_val_t al) noexcept {
    ++tmc::detail::g_task_alloc_count;

    // std::printf("task_promise new %zu\n", n);
    return ::operator new(n, al);
  }

#if TMC_SIZED_DEALLOCATION
  static void operator delete(void* ptr, std::size_t n) noexcept {
    return ::operator delete(ptr, n);
  }
  static void
  operator delete(void* ptr, std::size_t n, std::align_val_t al) noexcept {
    return ::operator delete(ptr, n, al);
  }
#endif

#ifndef __clang__
  // GCC creates a TON of warnings if this is missing with the noexcept new
  static task<void> get_return_object_on_allocation_failure() noexcept {
    return {};
  }
#endif

#endif // TMC_DEBUG_TASK_ALLOC_COUNT
};
} // namespace detail

template <typename Awaitable, typename Result> class aw_task {
  Awaitable handle;
  tmc::detail::result_storage_t<Result> result;

  friend Awaitable;
  friend tmc::detail::awaitable_traits<Awaitable>;
  aw_task(Awaitable&& Handle) noexcept : handle(std::move(Handle)) {
    assert(
      handle.address() != nullptr &&
      "You may only submit or co_await this once."
    );
  }

public:
  inline constexpr bool await_ready() const noexcept { return false; }
  inline std::coroutine_handle<>
  await_suspend(std::coroutine_handle<> Outer) noexcept {
    tmc::detail::get_awaitable_traits<Awaitable>::set_continuation(
      handle, Outer.address()
    );
    tmc::detail::get_awaitable_traits<Awaitable>::set_result_ptr(
      handle, &result
    );
    return std::move(handle);
  }

  /// Returns the value provided by the awaited task.
  inline Result&& await_resume() noexcept {
    if constexpr (std::is_default_constructible_v<Result>) {
      return std::move(result);
    } else {
      return *std::move(result);
    }
  }

  // Not movable or copyable due to holding result storage
  aw_task(const aw_task& other) = delete;
  aw_task& operator=(const aw_task& other) = delete;
  aw_task(aw_task&& other) = delete;
  aw_task&& operator=(aw_task&& other) = delete;
};

template <typename Awaitable> class aw_task<Awaitable, void> {
  Awaitable handle;

  friend Awaitable;
  friend tmc::detail::awaitable_traits<Awaitable>;
  aw_task(Awaitable&& Handle) noexcept : handle(std::move(Handle)) {
    assert(
      handle.address() != nullptr &&
      "You may only submit or co_await this once."
    );
  }

public:
  inline constexpr bool await_ready() const noexcept { return false; }
  inline std::coroutine_handle<>
  await_suspend(std::coroutine_handle<> Outer) noexcept {
    tmc::detail::get_awaitable_traits<Awaitable>::set_continuation(
      handle, Outer.address()
    );
    return std::move(handle);
  }
  inline void await_resume() noexcept {}

  // Could be movable, but prefer to make it consistent with the others
  aw_task(const aw_task& other) = delete;
  aw_task& operator=(const aw_task& other) = delete;
  aw_task(aw_task&& other) = delete;
  aw_task&& operator=(aw_task&& other) = delete;
};

namespace detail {

template <typename Result> struct awaitable_traits<tmc::task<Result>> {
  using result_type = Result;
  using self_type = tmc::task<Result>;
  using awaiter_type = tmc::aw_task<self_type, Result>;

  // Values controlling the behavior when awaited directly in a tmc::task
  static awaiter_type get_awaiter(self_type&& Awaitable) noexcept {
    return awaiter_type(static_cast<self_type&&>(Awaitable));
  }

  // Values controlling the behavior when wrapped by a utility function
  // such as tmc::spawn_*()
  static constexpr configure_mode mode = TMC_TASK;

  static void set_result_ptr(
    self_type& Awaitable, tmc::detail::result_storage_t<Result>* ResultPtr
  ) noexcept {
    Awaitable.promise().customizer.result_ptr = ResultPtr;
  }

  static void
  set_continuation(self_type& Awaitable, void* Continuation) noexcept {
    Awaitable.promise().customizer.continuation = Continuation;
  }

  static void
  set_continuation_executor(self_type& Awaitable, void* ContExec) noexcept {
    Awaitable.promise().customizer.continuation_executor = ContExec;
  }

  static void set_done_count(self_type& Awaitable, void* DoneCount) noexcept {
    Awaitable.promise().customizer.done_count = DoneCount;
  }

  static void set_flags(self_type& Awaitable, size_t Flags) noexcept {
    Awaitable.promise().customizer.flags = Flags;
  }
};
} // namespace detail
} // namespace tmc
