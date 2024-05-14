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
  inline bool await_ready() const noexcept { return false; }
  inline void await_resume() const noexcept {}

  inline std::coroutine_handle<>
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
        detail::type_erased_executor* continuationExecutor =
          static_cast<detail::type_erased_executor*>(p.continuation_executor);
        if (p.continuation_executor == nullptr ||
            this_thread::exec_is(continuationExecutor)) {
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
          if (continuationExecutor == nullptr ||
              this_thread::exec_is(continuationExecutor)) {
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
  [[nodiscard("You must submit or co_await task for execution. Failure to "
              "do so will result in a memory leak.")]] inline task&
  resume_on(detail::type_erased_executor* Executor) & {
    handle.promise().continuation_executor = Executor;
    return *this;
  }
  /// When this task completes, the awaiting coroutine will be resumed
  /// on the provided executor.
  template <detail::TypeErasableExecutor Exec>
  [[nodiscard("You must submit or co_await task for execution. Failure to "
              "do so will result in a memory leak.")]] task&
  resume_on(Exec& Executor) & {
    return resume_on(Executor.type_erased());
  }
  /// When this task completes, the awaiting coroutine will be resumed
  /// on the provided executor.
  template <detail::TypeErasableExecutor Exec>
  [[nodiscard("You must submit or co_await task for execution. Failure to "
              "do so will result in a memory leak.")]] task&
  resume_on(Exec* Executor) & {
    return resume_on(Executor->type_erased());
  }

  /// When this task completes, the awaiting coroutine will be resumed
  /// on the provided executor.
  [[nodiscard("You must submit or co_await task for execution. Failure to "
              "do so will result in a memory leak.")]] inline task&&
  resume_on(detail::type_erased_executor* Executor) && {
    handle.promise().continuation_executor = Executor;
    return *this;
  }
  /// When this task completes, the awaiting coroutine will be resumed
  /// on the provided executor.
  template <detail::TypeErasableExecutor Exec>
  [[nodiscard("You must submit or co_await task for execution. Failure to "
              "do so will result in a memory leak.")]] task&&
  resume_on(Exec& Executor) && {
    return resume_on(Executor.type_erased());
  }
  /// When this task completes, the awaiting coroutine will be resumed
  /// on the provided executor.
  template <detail::TypeErasableExecutor Exec>
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
  task_promise()
      : continuation{nullptr}, continuation_executor{this_thread::executor},
        done_count{nullptr}, result_ptr{nullptr} {}
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
    *result_ptr = static_cast<Result&&>(Value);
  }

  void return_value(Result const& Value)
    requires(!std::is_reference_v<Result>)
  {
    *result_ptr = Value;
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

  void* continuation;
  void* continuation_executor;
  std::atomic<int64_t>* done_count;
  // std::exception_ptr exc;
};

/// For internal usage only! To modify promises without taking ownership.
template <typename Result>
using unsafe_task = std::coroutine_handle<task_promise<Result>>;
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
  requires(detail::is_func_void_v<Original>)
task<void> into_task(Original FuncVoid) {
  FuncVoid();
  co_return;
}

inline work_item into_work_item(task<void>&& Task) {
  return std::coroutine_handle<>(static_cast<task<void>&&>(Task));
}

template <typename Original>
  requires(detail::is_func_void_v<Original>)
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
  inline std::coroutine_handle<> await_suspend(std::coroutine_handle<> Outer
  ) noexcept {
    auto& p = handle.promise();
    p.continuation = Outer.address();
    p.result_ptr = &result;
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
  inline std::coroutine_handle<> await_suspend(std::coroutine_handle<> Outer
  ) noexcept {
    auto& p = handle.promise();
    p.continuation = Outer.address();
    return std::move(handle);
  }
  inline void await_resume() noexcept {}
};

/// Submits `Work` for execution on `Executor` at priority `Priority`. Tasks or
/// functors that return values cannot be submitted this way; see
/// `post_waitable` instead.
template <typename E, typename TaskOrFunc>
void post(E& Executor, TaskOrFunc&& Work, size_t Priority)
  requires(detail::is_task_void_v<TaskOrFunc> || detail::is_func_void_v<TaskOrFunc>)
{
  if constexpr (std::is_convertible_v<TaskOrFunc, work_item>) {
    Executor.post(work_item(static_cast<TaskOrFunc&&>(Work)), Priority);
  } else {
    Executor.post(
      detail::into_work_item(static_cast<TaskOrFunc&&>(Work)), Priority
    );
  }
}

} // namespace tmc
