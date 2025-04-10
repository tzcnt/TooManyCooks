// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/aw_resume_on.hpp"
#include "tmc/detail/awaitable_customizer.hpp"
#include "tmc/detail/compat.hpp"
#include "tmc/detail/concepts.hpp"

#include <coroutine>
#include <exception>

namespace tmc {
namespace detail {

template <typename Result> struct wrapper_task_promise;
template <typename Result> class aw_wrapper_task;

// Same as task, but doesn't use await_transform.
// Used to safely wrap unknown awaitables.
template <typename Result> struct wrapper_task {
  using promise_type = tmc::detail::wrapper_task_promise<Result>;
  using result_type = Result;
  std::coroutine_handle<promise_type> handle;

  aw_wrapper_task<Result> operator co_await() && noexcept {
    return aw_wrapper_task<Result>(std::move(*this));
  }

  inline wrapper_task() noexcept : handle(nullptr) {}

  /// Conversion to a std::coroutine_handle<> is move-only
  operator std::coroutine_handle<>() && noexcept {
    auto addr = handle.address();
    return std::coroutine_handle<>::from_address(addr);
  }

  /// Conversion to a std::coroutine_handle<> is move-only
  operator std::coroutine_handle<promise_type>() && noexcept {
    auto addr = handle.address();
    return std::coroutine_handle<promise_type>::from_address(addr);
  }

  static wrapper_task from_address(void* addr) noexcept {
    wrapper_task t;
    t.handle = std::coroutine_handle<promise_type>::from_address(addr);
    return t;
  }

  static wrapper_task from_promise(promise_type& prom) noexcept {
    wrapper_task t;
    t.handle = std::coroutine_handle<promise_type>::from_promise(prom);
    return t;
  }

  bool done() const noexcept { return handle.done(); }

  inline void* address() const noexcept { return handle.address(); }

  void destroy() noexcept { handle.destroy(); }

  void resume() const noexcept { handle.resume(); }
  void operator()() const noexcept { handle.resume(); }

  operator bool() const noexcept { return handle.operator bool(); }

  auto& promise() const noexcept { return handle.promise(); }
};

// wrapper_task_promise supports rethrowing exceptions that may occur from
// unknown awaitables.
template <typename Result> struct wrapper_task_promise {
  awaitable_customizer<Result> customizer;
#if TMC_HAS_EXCEPTIONS
  std::exception_ptr* exc = nullptr;
#endif

  wrapper_task_promise() noexcept {}
  inline std::suspend_always initial_suspend() const noexcept { return {}; }
  inline mt1_continuation_resumer<wrapper_task_promise>
  final_suspend() const noexcept {
    return {};
  }
  wrapper_task<Result> get_return_object() noexcept {
    return {wrapper_task<Result>::from_promise(*this)};
  }

#if TMC_HAS_EXCEPTIONS
  void unhandled_exception() noexcept {
    if (exc == nullptr) {
      std::rethrow_exception(std::current_exception());
    }
    *exc = std::current_exception();
  }
#else
  [[noreturn]] void unhandled_exception() noexcept { std::terminate(); }
#endif

  template <typename RV>
  void return_value(RV&& Value
  ) noexcept(std::is_nothrow_move_constructible_v<RV>) {
    *customizer.result_ptr = static_cast<RV&&>(Value);
  }
};

template <> struct wrapper_task_promise<void> {
  awaitable_customizer<void> customizer;
#if TMC_HAS_EXCEPTIONS
  std::exception_ptr* exc = nullptr;
#endif

  wrapper_task_promise() noexcept {}
  inline std::suspend_always initial_suspend() const noexcept { return {}; }
  inline mt1_continuation_resumer<wrapper_task_promise>
  final_suspend() const noexcept {
    return {};
  }
  wrapper_task<void> get_return_object() noexcept {
    return {wrapper_task<void>::from_promise(*this)};
  }

#if TMC_HAS_EXCEPTIONS
  void unhandled_exception() noexcept {
    if (exc == nullptr) {
      std::rethrow_exception(std::current_exception());
    }
    *exc = std::current_exception();
  }
#else
  [[noreturn]] void unhandled_exception() noexcept { std::terminate(); }
#endif

  void return_void() noexcept {}
};

template <typename Result>
struct awaitable_traits<tmc::detail::wrapper_task<Result>> {

  using result_type = Result;
  using self_type = tmc::detail::wrapper_task<Result>;
  using awaiter_type = tmc::detail::aw_wrapper_task<Result>;

  // Values controlling the behavior when awaited directly in a tmc::task
  static awaiter_type get_awaiter(self_type& Awaitable) noexcept {
    return awaiter_type(Awaitable);
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

template <typename Result> class aw_wrapper_task {
  using Awaitable = tmc::detail::wrapper_task<Result>;
  Awaitable handle;
#if TMC_HAS_EXCEPTIONS
  std::exception_ptr exc;
#endif
  tmc::detail::result_storage_t<Result> result;

  friend Awaitable;
  friend tmc::detail::awaitable_traits<Awaitable>;
  aw_wrapper_task(Awaitable&& Handle) noexcept : handle(std::move(Handle)) {
#if TMC_HAS_EXCEPTIONS
    handle.promise().exc = &exc;
#endif
  }

public:
  inline bool await_ready() const noexcept { return handle.done(); }
  TMC_FORCE_INLINE inline std::coroutine_handle<>
  await_suspend(std::coroutine_handle<> Outer) noexcept {
    tmc::detail::awaitable_traits<Awaitable>::set_continuation(
      handle, Outer.address()
    );
    tmc::detail::awaitable_traits<Awaitable>::set_result_ptr(handle, &result);
    return std::move(handle);
  }

  /// Returns the value provided by the awaited task.
  inline Result&& await_resume() {
#if TMC_HAS_EXCEPTIONS
    if (exc != nullptr) {
      std::rethrow_exception(exc);
    }
#endif
    if constexpr (std::is_default_constructible_v<Result>) {
      return std::move(result);
    } else {
      return *std::move(result);
    }
  }

  // Not movable or copyable due to holding exc and result storage
  aw_wrapper_task(const aw_wrapper_task& other) = delete;
  aw_wrapper_task& operator=(const aw_wrapper_task& other) = delete;
  aw_wrapper_task(aw_wrapper_task&& other) = delete;
  aw_wrapper_task&& operator=(aw_wrapper_task&& other) = delete;
};

template <> class aw_wrapper_task<void> {
  using Awaitable = tmc::detail::wrapper_task<void>;
  Awaitable handle;
#if TMC_HAS_EXCEPTIONS
  std::exception_ptr exc;
#endif

  friend Awaitable;
  friend tmc::detail::awaitable_traits<Awaitable>;
  inline aw_wrapper_task(Awaitable&& Handle) noexcept
      : handle(std::move(Handle)) {
#if TMC_HAS_EXCEPTIONS
    handle.promise().exc = &exc;
#endif
  }

public:
  inline bool await_ready() const noexcept { return handle.done(); }
  TMC_FORCE_INLINE inline std::coroutine_handle<>
  await_suspend(std::coroutine_handle<> Outer) noexcept {
    tmc::detail::awaitable_traits<Awaitable>::set_continuation(
      handle, Outer.address()
    );
    return std::move(handle);
  }
  inline void await_resume() {
#if TMC_HAS_EXCEPTIONS
    if (exc != nullptr) {
      std::rethrow_exception(exc);
    }
#endif
  }

  // Not movable or copyable due to holding exc
  aw_wrapper_task(const aw_wrapper_task& other) = delete;
  aw_wrapper_task& operator=(const aw_wrapper_task& other) = delete;
  aw_wrapper_task(aw_wrapper_task&& other) = delete;
  aw_wrapper_task&& operator=(aw_wrapper_task&& other) = delete;
};

/// A wrapper to convert any awaitable to a task so that it may be used
/// with TMC utilities. This wrapper task type doesn't have await_transform;
/// it IS the await_transform. It ensures that, after awaiting the unknown
/// awaitable, we are restored to the original TMC executor and priority.
template <
  typename Awaitable,
  typename Result = tmc::detail::awaitable_result_t<Awaitable>>
[[nodiscard("You must await the return type of safe_wrap()"
)]] tmc::detail::wrapper_task<Result>
safe_wrap(Awaitable&& awaitable
) noexcept(std::is_nothrow_move_constructible_v<Awaitable>) {
  return [](
           Awaitable Aw, tmc::aw_resume_on TakeMeHome
         ) -> tmc::detail::wrapper_task<Result> {
    if constexpr (std::is_void_v<Result>) {
      co_await std::move(Aw);
      co_await TakeMeHome;
      co_return;
    } else {
      auto result = co_await std::move(Aw);
      co_await TakeMeHome;
      co_return result;
    }
  }(std::forward<Awaitable>(awaitable),
           tmc::resume_on(tmc::detail::this_thread::executor));
}
} // namespace detail
} // namespace tmc