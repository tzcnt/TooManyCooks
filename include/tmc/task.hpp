// Copyright (c) 2023-2024 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/concepts.hpp" // IWYU pragma: keep
#include "tmc/detail/thread_locals.hpp"

#include <atomic>
#include <cassert>
#include <coroutine>
#include <new>
#include <type_traits>

namespace tmc {
template <typename Result>
struct [[nodiscard("You must submit or co_await task for execution. Failure to "
                   "do so will result in a memory leak.")]] task;

namespace detail {
namespace task_flags {
constexpr inline uint64_t EACH = 1ULL << 63;
constexpr inline uint64_t OFFSET_MASK = (1ULL << 6) - 1;
} // namespace task_flags

/// Multipurpose awaitable type. Exposes fields that can be customized by most
/// TMC utility functions. Exposing this type allows various awaitables to be
/// compatible with the library and with each other.
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
///
/// `flags` is used to indicate other methods of coordinated resumption between
/// multiple awaitables.
struct awaitable_customizer_base {
  void* continuation;
  void* continuation_executor;
  void* done_count;
  uint64_t flags;

  static_assert(sizeof(void*) == sizeof(std::coroutine_handle<>));
  static_assert(alignof(void*) == alignof(std::coroutine_handle<>));
  static_assert(std::is_trivially_copyable_v<std::coroutine_handle<>>);
  static_assert(std::is_trivially_destructible_v<std::coroutine_handle<>>);

  awaitable_customizer_base()
      : continuation{nullptr}, continuation_executor{this_thread::executor},
        done_count{nullptr}, flags{0} {}

  // Either returns the awaiting coroutine (continuation) to be resumed
  // directly, or submits that awaiting coroutine to the continuation executor
  // to be resumed. This should be called exactly once, after the awaitable is
  // complete and any results are ready.
  TMC_FORCE_INLINE inline std::coroutine_handle<>
  resume_continuation() noexcept {
    std::coroutine_handle<> finalContinuation = nullptr;
    tmc::detail::type_erased_executor* continuationExecutor = nullptr;
    if (done_count == nullptr) {
      // being awaited alone, or detached
      // continuation is a std::coroutine_handle<>
      // continuation_executor is a tmc::detail::type_erased_executor*
      continuationExecutor =
        static_cast<tmc::detail::type_erased_executor*>(continuation_executor);
      finalContinuation = std::coroutine_handle<>::from_address(continuation);
    } else {
      // being awaited as part of a group
      bool should_resume;
      if (flags & task_flags::EACH) {
        // Each only supports 63 tasks. High bit of flags indicates whether the
        // awaiting task is ready to resume, or is already resumed. Each of the
        // low 63 bits are unique to a child task. We will set our unique low
        // bit, as well as try to set the high bit. If high bit was already
        // set, someone else is running the awaiting task already.
        should_resume = 0 == (task_flags::EACH &
                              static_cast<std::atomic<uint64_t>*>(done_count)
                                ->fetch_or(
                                  task_flags::EACH |
                                    (1ULL << (flags & task_flags::OFFSET_MASK)),
                                  std::memory_order_acq_rel
                                ));
      } else {
        // task is part of a spawn_many group, or run_early
        // continuation is a std::coroutine_handle<>*
        // continuation_executor is a tmc::detail::type_erased_executor**
        should_resume = static_cast<std::atomic<int64_t>*>(done_count)
                          ->fetch_sub(1, std::memory_order_acq_rel) == 0;
      }
      if (should_resume) {
        continuationExecutor =
          *static_cast<tmc::detail::type_erased_executor**>(
            continuation_executor
          );
        finalContinuation =
          *(static_cast<std::coroutine_handle<>*>(continuation));
      }
    }

    // Common submission and continuation logic
    if (continuationExecutor != nullptr &&
        !this_thread::exec_is(continuationExecutor)) {
      // post_checked is redundant with the prior check at the moment
      tmc::detail::post_checked(
        continuationExecutor, std::move(finalContinuation),
        this_thread::this_task.prio
      );
      finalContinuation = nullptr;
    }
    if (finalContinuation == nullptr) {
      finalContinuation = std::noop_coroutine();
    }
    return finalContinuation;
  }
};

template <typename Result>
struct awaitable_customizer : awaitable_customizer_base {
  Result* result_ptr;
  awaitable_customizer() : awaitable_customizer_base{}, result_ptr{nullptr} {}

  using result_type = Result;
};

template <> struct awaitable_customizer<void> : awaitable_customizer_base {
  awaitable_customizer() : awaitable_customizer_base{} {}

  using result_type = void;
};

template <typename Result> struct task_promise;

// final_suspend type for tmc::task
// a wrapper around awaitable_customizer
template <typename Result> struct mt1_continuation_resumer {
  inline bool await_ready() const noexcept { return false; }

  inline void await_resume() const noexcept {}

  TMC_FORCE_INLINE inline std::coroutine_handle<>
  await_suspend(std::coroutine_handle<task_promise<Result>> Handle
  ) const noexcept {
    auto& p = Handle.promise();
    auto continuation = p.customizer.resume_continuation();
    Handle.destroy();
    return continuation;
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
  std::coroutine_handle<tmc::detail::task_promise<Result>> handle;
  using result_type = Result;
  using promise_type = tmc::detail::task_promise<Result>;

  /// Suspend the outer coroutine and run this task directly. The intermediate
  /// awaitable type `aw_task` cannot be used directly; the return type of the
  /// `co_await` expression will be `Result` or `void`.
  aw_task<Result> operator co_await() && {
    return aw_task<Result>(std::move(*this));
  }

  /// When this task completes, the awaiting coroutine will be resumed
  /// on the provided executor.
  [[nodiscard("You must submit or co_await task for execution. Failure to "
              "do so will result in a memory leak.")]] inline task&
  resume_on(tmc::detail::type_erased_executor* Executor) & {
    handle.promise().customizer.continuation_executor = Executor;
    return *this;
  }
  /// When this task completes, the awaiting coroutine will be resumed
  /// on the provided executor.
  template <tmc::detail::TypeErasableExecutor Exec>
  [[nodiscard("You must submit or co_await task for execution. Failure to "
              "do so will result in a memory leak.")]] task&
  resume_on(Exec& Executor) & {
    return resume_on(Executor.type_erased());
  }
  /// When this task completes, the awaiting coroutine will be resumed
  /// on the provided executor.
  template <tmc::detail::TypeErasableExecutor Exec>
  [[nodiscard("You must submit or co_await task for execution. Failure to "
              "do so will result in a memory leak.")]] task&
  resume_on(Exec* Executor) & {
    return resume_on(Executor->type_erased());
  }

  /// When this task completes, the awaiting coroutine will be resumed
  /// on the provided executor.
  [[nodiscard("You must submit or co_await task for execution. Failure to "
              "do so will result in a memory leak.")]] inline task&&
  resume_on(tmc::detail::type_erased_executor* Executor) && {
    handle.promise().customizer.continuation_executor = Executor;
    return *this;
  }
  /// When this task completes, the awaiting coroutine will be resumed
  /// on the provided executor.
  template <tmc::detail::TypeErasableExecutor Exec>
  [[nodiscard("You must submit or co_await task for execution. Failure to "
              "do so will result in a memory leak.")]] task&&
  resume_on(Exec& Executor) && {
    return resume_on(Executor.type_erased());
  }
  /// When this task completes, the awaiting coroutine will be resumed
  /// on the provided executor.
  template <tmc::detail::TypeErasableExecutor Exec>
  [[nodiscard("You must submit or co_await task for execution. Failure to "
              "do so will result in a memory leak.")]] task&&
  resume_on(Exec* Executor) && {
    return resume_on(Executor->type_erased());
  }

  inline task() noexcept : handle(nullptr) {}

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

  static task from_address(void* addr) noexcept {
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

  inline void* address() const noexcept { return handle.address(); }

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
  awaitable_customizer<Result> customizer;

  task_promise() {}
  inline std::suspend_always initial_suspend() const noexcept { return {}; }
  inline mt1_continuation_resumer<Result> final_suspend() const noexcept {
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
    *customizer.result_ptr = static_cast<Result&&>(Value);
  }

  void return_value(Result const& Value)
    requires(!std::is_reference_v<Result>)
  {
    *customizer.result_ptr = Value;
  }

#ifdef TMC_CUSTOM_CORO_ALLOC
  // Round up the coroutine allocation to next 64 bytes.
  // This reduces false sharing with adjacent coroutines.
  static void* operator new(std::size_t n) noexcept {
    // This operator new as noexcept. This means that if the allocation
    // throws, std::terminate will be called.
    // I recommend using tcmalloc with TooManyCooks, as it will also directly
    // crash the program rather than throwing an exception:
    // https://github.com/google/tcmalloc/blob/master/docs/reference.md#operator-new--operator-new

    // DEBUG - Print the size of the coroutine allocation.
    // std::printf("task_promise new %zu -> %zu\n", n, (n + 63) & -64);
    n = (n + 63) & -64;
    return ::operator new(n);
  }

  static void* operator new(std::size_t n, std::align_val_t al) noexcept {
    // Don't try to round up the allocation size if there is also a required
    // alignment. If we end up with size > alignment, that could cause issues.
    return ::operator new(n, al);
  }

#ifndef __clang__
  // GCC creates a TON of warnings if this is missing with the noexcept new
  static task<Result> get_return_object_on_allocation_failure() { return {}; }
#endif
#endif
};

template <> struct task_promise<void> {
  awaitable_customizer<void> customizer;

  task_promise() {}
  inline std::suspend_always initial_suspend() const noexcept { return {}; }
  inline mt1_continuation_resumer<void> final_suspend() const noexcept {
    return {};
  }
  task<void> get_return_object() noexcept {
    return {task<void>::from_promise(*this)};
  }
  [[noreturn]] void unhandled_exception() {
    throw;
    // exc = std::current_exception();
  }

  void return_void() {}
};

/// For internal usage only! To modify promises without taking ownership.
template <typename Result>
using unsafe_task = std::coroutine_handle<task_promise<Result>>;

template <typename Awaitable> struct unknown_awaitable_traits {
  // Try to guess at the awaiter type based on the expected function signatures.
  template <typename T> static decltype(auto) guess_awaiter(T&& value) {
    if constexpr (requires { static_cast<T&&>(value).operator co_await(); }) {
      return static_cast<T&&>(value).operator co_await();
    } else if constexpr (requires {
                           operator co_await(static_cast<T&&>(value));
                         }) {
      return operator co_await(static_cast<T&&>(value));
    } else {
      return static_cast<T&&>(value);
    }
  }

  using awaiter_type = decltype(guess_awaiter(std::declval<Awaitable>()));

  using result_type =
    std::remove_reference_t<decltype(std::declval<awaiter_type>().await_resume()
    )>;
};

enum awaitable_mode { COROUTINE, ASYNC_INITIATE, UNKNOWN };

// You must specialize this for any awaitable that you want to submit directly
// to a customization function (tmc::spawn_*()). If you don't want to specialize
// this, you can instead use tmc::external::safe_await() to wrap your awaitable
// into a task.
template <typename Awaitable> struct awaitable_traits {
  static constexpr awaitable_mode mode = UNKNOWN;

  // Try to guess at the result type based on the expected function signatures.
  // Awaiting is context-dependent, so this is not guaranteed to be correct. If
  // this doesn't behave as expected, you should specialize awaitable_traits
  // instead.
  using result_type =
    tmc::detail::unknown_awaitable_traits<Awaitable>::result_type;
};

//// Details on how to specialize awaitable_traits:
// template <typename Awaitable> struct awaitable_traits {
// {

/* You must declare `static constexpr awaitable_mode mode;` */
//// If you declare this, when initiating the async process, the awaitable
//// will be submitted to the TMC executor to be resumed.
//// It may also be resumed directly using symmetric transfer.
//// requires {std::coroutine_handle<> c = declval<YourAwaitable>();}
// static constexpr awaitable_mode mode = COROUTINE;

//// Otherwise, you must define this function, which will be called to
//// initiate the async process. The current TMC executor and priority will
//// be passed in, but they are not required to be used.
// static constexpr awaitable_mode mode = ASYNC_INITIATE;
// static void async_initiate(
//   Awaitable&& YourAwaitable, tmc::detail::type_erased_executor* Executor,
//   size_t Priority
// ) {}

/* You must declare ALL of the following types and functions. */

//// Define the result type of `co_await YourAwaitable;`
//// Storage will be allocated for this result type, and it will be assigned
//// via set_result_ptr(), So it should not be a reference type.
// using result_type = /* result type of `co_await YourAwaitable;` */;

// static void set_result_ptr(Awaitable& YourAwaitable, result_type*
// ResultPtr);

// static void set_continuation(Awaitable& YourAwaitable, void* Continuation);

// static void
// set_continuation_executor(Awaitable& YourAwaitable, void* ContExec);

// static void set_done_count(Awaitable& YourAwaitable, void* DoneCount);

// static void set_flags(Awaitable& YourAwaitable, uint64_t Flags);
// };

template <typename Result> struct awaitable_traits<task<Result>> {
  static constexpr awaitable_mode mode = COROUTINE;

  using result_type = Result;

  static void set_result_ptr(task<Result>& Awaitable, Result* ResultPtr) {
    Awaitable.promise().customizer.result_ptr = ResultPtr;
  }

  static void set_continuation(task<Result>& Awaitable, void* Continuation) {
    Awaitable.promise().customizer.continuation = Continuation;
  }

  static void
  set_continuation_executor(task<Result>& Awaitable, void* ContExec) {
    Awaitable.promise().customizer.continuation_executor = ContExec;
  }

  static void set_done_count(task<Result>& Awaitable, void* DoneCount) {
    Awaitable.promise().customizer.done_count = DoneCount;
  }

  static void set_flags(task<Result>& Awaitable, uint64_t Flags) {
    Awaitable.promise().customizer.flags = Flags;
  }
};

template <typename Result>
struct awaitable_traits<tmc::detail::unsafe_task<Result>> {
  static constexpr awaitable_mode mode = COROUTINE;

  using result_type = Result;

  static void set_result_ptr(
    tmc::detail::unsafe_task<Result>& Awaitable, Result* ResultPtr
  ) {
    Awaitable.promise().customizer.result_ptr = ResultPtr;
  }

  static void set_continuation(
    tmc::detail::unsafe_task<Result>& Awaitable, void* Continuation
  ) {
    Awaitable.promise().customizer.continuation = Continuation;
  }

  static void set_continuation_executor(
    tmc::detail::unsafe_task<Result>& Awaitable, void* ContExec
  ) {
    Awaitable.promise().customizer.continuation_executor = ContExec;
  }

  static void
  set_done_count(tmc::detail::unsafe_task<Result>& Awaitable, void* DoneCount) {
    Awaitable.promise().customizer.done_count = DoneCount;
  }

  static void
  set_flags(tmc::detail::unsafe_task<Result>& Awaitable, uint64_t Flags) {
    Awaitable.promise().customizer.flags = Flags;
  }
};

} // namespace detail
} // namespace tmc

template <typename Result, typename... Args>
struct std::coroutine_traits<tmc::detail::unsafe_task<Result>, Args...> {
  using promise_type = tmc::detail::task_promise<Result>;
};

namespace tmc {
namespace detail {

template <class T, template <class...> class U>
constexpr inline bool is_instance_of_v = std::false_type{};

template <template <class...> class U, class... Vs>
constexpr inline bool is_instance_of_v<U<Vs...>, U> = std::true_type{};

struct not_found {};

/// begin task_result_t<T>
template <typename T>
concept HasTaskResult = requires { typename T::result_type; };
template <typename T> struct task_result_t_impl {
  using type = not_found;
};
template <HasTaskResult T> struct task_result_t_impl<T> {
  using type = T::result_type;
};
template <typename T>
using task_result_t = typename task_result_t_impl<T>::type;
/// end task_result_t<T>

/// begin func_result_t<T>
template <typename T>
concept HasFuncResult = requires { typename std::invoke_result_t<T>; };
template <typename T> struct func_result_t_impl {
  using type = not_found;
};
template <HasFuncResult T> struct func_result_t_impl<T> {
  using type = std::invoke_result_t<T>;
};
template <typename T>
using func_result_t = typename func_result_t_impl<T>::type;
/// end func_result_t<T>

// Can be converted to a `tmc::task<T::result_type>`
template <typename T>
concept is_task_v = std::is_convertible_v<T, task<task_result_t<T>>>;

// Can be converted to a `tmc::task<void>`
template <typename T>
concept is_task_void_v =
  std::is_convertible_v<T, task<void>> && std::is_void_v<task_result_t<T>>;

// Can be converted to a `tmc::task<T::result_type>` where `T::result_type` !=
// void
template <typename T>
concept is_task_nonvoid_v = std::is_convertible_v<T, task<task_result_t<T>>> &&
                            !std::is_void_v<task_result_t<T>>;

// Can be converted to a `tmc::task<Result>`
template <typename T, typename Result>
concept is_task_result_v = std::is_convertible_v<T, task<Result>>;

// A functor with `operator()()` that isn't a `tmc::task`
template <typename T>
concept is_func_v =
  !is_task_v<T> && !std::is_same_v<func_result_t<T>, not_found>;

// A functor with `void operator()()` that isn't a `tmc::task`
template <typename T>
concept is_func_void_v = !is_task_v<T> && std::is_void_v<func_result_t<T>>;

// A functor with `Result operator()()` that isn't a `tmc::task`, where Result
// != void
template <typename T>
concept is_func_nonvoid_v =
  !is_task_v<T> && !std::is_void_v<func_result_t<T>> &&
  !std::is_same_v<func_result_t<T>, not_found>;

// A functor with `Result operator()()` that isn't a `tmc::task`
template <typename T, typename Result>
concept is_func_result_v =
  !is_task_v<T> && std::is_same_v<func_result_t<T>, Result>;

/// Makes a task<Result> from a task<Result> or a Result(void)
/// functor.

template <typename Original, typename Result = Original::result_type>
  requires(is_task_result_v<Original, Result>)
task<Result> into_task(Original Task) {
  return Task;
}

template <typename Original, typename Result = std::invoke_result_t<Original>>
task<Result> into_task(Original FuncResult)
  requires(!std::is_void_v<Result> && is_func_result_v<Original, Result>)
{
  co_return FuncResult();
}

template <typename Original>
  requires(tmc::detail::is_func_void_v<Original>)
task<void> into_task(Original FuncVoid) {
  FuncVoid();
  co_return;
}

inline work_item into_work_item(task<void>&& Task) {
  return std::coroutine_handle<>(static_cast<task<void>&&>(Task));
}

template <typename Original>
  requires(tmc::detail::is_func_void_v<Original>)
work_item into_work_item(Original&& FuncVoid) {
#if TMC_WORK_ITEM_IS(CORO)
  return std::coroutine_handle<>([](Original f) -> task<void> {
    f();
    co_return;
  }(static_cast<Original&&>(FuncVoid)));
#else
  return FuncVoid;
#endif
}

} // namespace detail

template <typename Result> class aw_task {
  task<Result> handle;
  Result result;

  friend struct task<Result>;
  aw_task(task<Result>&& Handle) : handle(std::move(Handle)) {}

public:
  inline bool await_ready() const noexcept { return handle.done(); }
  TMC_FORCE_INLINE inline std::coroutine_handle<>
  await_suspend(std::coroutine_handle<> Outer) noexcept {
    tmc::detail::awaitable_traits<task<Result>>::set_continuation(
      handle, Outer.address()
    );
    tmc::detail::awaitable_traits<task<Result>>::set_result_ptr(
      handle, &result
    );
    return std::move(handle);
  }

  /// Returns the value provided by the awaited task.
  inline Result&& await_resume() noexcept { return std::move(result); }
};

template <> class aw_task<void> {
  task<void> handle;

  friend struct task<void>;
  inline aw_task(task<void>&& Handle) : handle(std::move(Handle)) {}

public:
  inline bool await_ready() const noexcept { return handle.done(); }
  TMC_FORCE_INLINE inline std::coroutine_handle<>
  await_suspend(std::coroutine_handle<> Outer) noexcept {
    tmc::detail::awaitable_traits<task<void>>::set_continuation(
      handle, Outer.address()
    );
    return std::move(handle);
  }
  inline void await_resume() noexcept {}
};

/// A wrapper to convert any awaitable to a task so that it may be used
/// with TMC utilities.
template <
  typename Awaitable, typename Result = typename tmc::detail::awaitable_traits<
                        Awaitable>::result_type>
[[nodiscard("You must await the return type of to_task()")]] tmc::task<Result>
to_task(Awaitable awaitable) {
  if constexpr (std::is_void_v<Result>) {
    co_await std::move(awaitable);
    co_return;
  } else {
    co_return co_await std::move(awaitable);
  }
}

namespace detail {
template <typename Awaitable>
TMC_FORCE_INLINE inline void initiate_one(
  Awaitable&& Item, tmc::detail::type_erased_executor* Executor, size_t Priority
) {
  if constexpr (tmc::detail::awaitable_traits<Awaitable>::mode == COROUTINE) {
    // Submitting to the TMC executor queue includes a release store,
    // so no atomic_thread_fence is needed.
    tmc::detail::post_checked(Executor, std::move(Item), Priority);
  } else if constexpr (tmc::detail::awaitable_traits<Awaitable>::mode ==
                       ASYNC_INITIATE) {
    std::atomic_thread_fence(std::memory_order_release);
    tmc::detail::awaitable_traits<Awaitable>::async_initiate(
      std::move(Item), Executor, Priority
    );
  } else {
    tmc::detail::post_checked(
      Executor, tmc::to_task(std::move(Item)), Priority
    );
  }
}
} // namespace detail

/// Submits `Work` for execution on `Executor` at priority `Priority`. Tasks or
/// functors that return values cannot be submitted this way; see
/// `post_waitable` instead.
template <typename E, typename TaskOrFunc>
void post(E& Executor, TaskOrFunc&& Work, size_t Priority)
  requires(tmc::detail::is_task_void_v<TaskOrFunc> || tmc::detail::is_func_void_v<TaskOrFunc>)
{
  if constexpr (std::is_convertible_v<TaskOrFunc, work_item>) {
    Executor.post(work_item(static_cast<TaskOrFunc&&>(Work)), Priority);
  } else {
    Executor.post(
      tmc::detail::into_work_item(static_cast<TaskOrFunc&&>(Work)), Priority
    );
  }
}

} // namespace tmc
