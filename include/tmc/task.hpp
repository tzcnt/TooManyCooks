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
        if (continuationExecutor == nullptr ||
            this_thread::exec_is(continuationExecutor)) {
          next = continuation;
        } else {
          // post_checked is redundant with the prior check at the moment
          detail::post_checked(
            continuationExecutor, std::move(continuation),
            this_thread::this_task.prio
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
            // post_checked is redundant with the prior check at the moment
            detail::post_checked(
              continuationExecutor, std::move(continuation),
              this_thread::this_task.prio
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

constexpr inline int64_t FREE_BLOCK_FLAG = 0x8000000000000000ULL;

// constexpr inline int64_t ALLOC_MODE_STACK = 0;
// constexpr inline int64_t ALLOC_MODE_SOLO = 1;

struct per_alloc_block {
  per_alloc_block* prev_block;
  // std::atomic<per_alloc_block*> next_block;
  std::atomic<int64_t> space_after;
};
struct group_alloc_header {
  // std::atomic<int64_t> mode;
  per_alloc_block* prev_group;
};

// template <typename A> struct awaitable_wrapper {
//   A awaitable;
//   std::coroutine_handle<> outer;
//   template <typename U>
//   awaitable_wrapper(U&& Awaitable) : awaitable(std::forward<U>(Awaitable)) {}
//   bool await_ready() { return awaitable.await_ready(); }

//   auto await_suspend(std::coroutine_handle<> Outer) {
//     outer = Outer;
//     return awaitable.await_suspend(Outer);
//   }

//   auto await_resume() {
//     // this is wrong if I'm part of a spawn_many group, or if
//     // another thread took the alloc beyond me
//     // tasks really do need to point to a group header with an atomic
//     offset...
//     // group header should have a MODE - solo, stackful, or group
//     // and atomic offset describing the end of the group
//     detail::this_thread::alloc_block =
//       (reinterpret_cast<per_alloc_block*>(outer.address()) - 1)->next_block;
//     return awaitable.await_resume();
//   }
// };

// Can be used only inside of operator new or delete
inline per_alloc_block* block_after_frame(void* CoroFrame, size_t FrameSize) {
  auto block = reinterpret_cast<per_alloc_block*>(
    reinterpret_cast<char*>(CoroFrame) + FrameSize
  );
  return block;
}

// Can be used only inside of operator new or delete
inline per_alloc_block* block_before_frame(void* CoroFrame, size_t FrameSize) {
  auto prevBlock = reinterpret_cast<per_alloc_block*>(CoroFrame) - 1;
  return prevBlock;
}

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

  // template <typename A> auto await_transform(A&& Awaitable) {
  //   return awaitable_wrapper<A>(std::forward<A>(Awaitable));
  // }

#ifdef TMC_CUSTOM_CORO_ALLOC
  static void* new_alloc_group(
    // int64_t Mode,
    per_alloc_block* PrevGroup, size_t TotalSize, size_t EachSize
  ) {
    group_alloc_header* header = static_cast<group_alloc_header*>(
      detail::this_thread::cache_alloc(TotalSize)
    );
    // header->mode.store(Mode, std::memory_order_relaxed);
    header->prev_group = PrevGroup;

    auto block = reinterpret_cast<per_alloc_block*>(header + 1);
    const auto sizePreBlock =
      TotalSize - sizeof(group_alloc_header) - sizeof(per_alloc_block);
    block->prev_block = nullptr;
    block->space_after.store(
      sizePreBlock | FREE_BLOCK_FLAG, std::memory_order_release
    );

    auto after_block = reinterpret_cast<per_alloc_block*>(
      reinterpret_cast<char*>(block) + EachSize
    );
    const auto sizePostBlock = sizePreBlock - EachSize;
    after_block->prev_block = block;
    // block->next_block.store(after_block, std::memory_order_relaxed);
    // after_block->next_block.store(nullptr, std::memory_order_relaxed);
    after_block->space_after.store(sizePostBlock, std::memory_order_release);

    detail::this_thread::alloc_block = after_block;

    auto coro = static_cast<void*>(block + 1);
    return coro;
  }

  static inline per_alloc_block*
  sweep_before_alloc(per_alloc_block* block, size_t EachSize) {
    auto spaceAfter = block->space_after.load(std::memory_order_acquire);
    bool blockFree = (spaceAfter & FREE_BLOCK_FLAG) != 0;
    while (blockFree) {
      if (block->prev_block != nullptr) {
        block = block->prev_block;
        spaceAfter = block->space_after.load(std::memory_order_acquire);
        blockFree = (spaceAfter & FREE_BLOCK_FLAG) != 0;
      } else {
        group_alloc_header* header =
          reinterpret_cast<group_alloc_header*>(block) - 1;
        auto pgb = header->prev_group;
        if (pgb != nullptr) {
          // Don't worry about stack splitting for now. Cache should handle it.
          // If it becomes a problem, can implement rolling tracker of real
          // allocations vs bump allocations using a 64-bit int. 1 = real alloc,
          // 0 = bump alloc. Shift left on every allocation. A similar thing
          // could track cache hit rate, and increase size of cache (1->2->8)
          // based on heuristics.
          auto pgbSa = pgb->space_after.load(std::memory_order_acquire);
          blockFree = (pgbSa & FREE_BLOCK_FLAG) != 0;
          bool pgbEnough = EachSize <= (pgbSa & ~(1ULL << 63));
          if (blockFree || pgbEnough) {
            // TODO free the prev group block without freeing the current block
            // if !pgbEnough - later... don't worry about stack splitting now
            detail::this_thread::cache_free(
              static_cast<void*>(header), (spaceAfter & ~(1ULL << 63)) +
                                            sizeof(group_alloc_header) +
                                            sizeof(per_alloc_block)
            );
            block = pgb;
            spaceAfter = pgbSa;
          }
        } else {
          // Only stackful allocs should get here.
          // Solo or Group allocs should free their own memory.
          // assert(header->mode == ALLOC_MODE_STACK);
          break;
        }
      }
    }
    return block;
  }

  // This operator new is noexcept. This means that if the allocation
  // throws, std::terminate will be called.
  // I recommend using tcmalloc with TooManyCooks, as it will also directly
  // crash the program rather than throwing an exception:
  // https://github.com/google/tcmalloc/blob/master/docs/reference.md#operator-new--operator-new

  static void* operator new(size_t n) noexcept {
    // Round up the coroutine allocation to next 64 bytes.
    // This reduces false sharing with adjacent coroutines.
    size_t eachSize = ((sizeof(per_alloc_block) + n + 63) & -64);
    // per_alloc_block* block;
    //  if (auto b = detail::this_thread::alloc_block; b != nullptr) [[likely]]
    //  {
    //    per_alloc_block* block = static_cast<per_alloc_block*>(b);
    //    block = sweep_before_alloc(block, eachSize);
    //    auto spaceAfter = block->space_after.load(std::memory_order_relaxed);
    //  }
    //  auto allocCount = detail::this_thread::alloc_count;
    //  if (allocCount > 0) [[unlikely]] {
    //    auto tasksSize = eachSize * allocCount;
    //    auto block =
    //      static_cast<per_alloc_block*>(detail::this_thread::alloc_block);
    //    if (block != nullptr && tasksSize <= block->space_after.load()) {
    //      // jump down to the eachSize block below
    //      return coro;
    //    }
    //    // A size hint was provided
    //    size_t totalSize = sizeof(group_alloc_header) +
    //    sizeof(per_alloc_block)
    //    +
    //                       eachSize * detail::this_thread::alloc_count;
    //    detail::this_thread::alloc_count = 0;
    //    return new_alloc_group(ALLOC_MODE_STACK, block, totalSize, eachSize);
    //    // std::printf(
    //    //   "group leader %zu -> each: %zu group: %zu\n", n, each_size,
    //    //   total_size
    //    // );
    //  } else
    if (auto b = detail::this_thread::alloc_block; b != nullptr) [[likely]] {
      per_alloc_block* block = static_cast<per_alloc_block*>(b);
      block = sweep_before_alloc(block, eachSize);
      auto spaceAfter =
        block->space_after.load(std::memory_order_relaxed) & ~FREE_BLOCK_FLAG;

      if (eachSize <= spaceAfter) {
        auto afterBlock = reinterpret_cast<per_alloc_block*>(
          reinterpret_cast<char*>(block) + eachSize
        );
        const auto sizePostBlock = spaceAfter - eachSize;
        afterBlock->prev_block = block;
        // block->next_block.store(afterBlock, std::memory_order_relaxed);
        // afterBlock->next_block.store(nullptr, std::memory_order_relaxed);
        afterBlock->space_after.store(sizePostBlock, std::memory_order_release);
        detail::this_thread::alloc_block = afterBlock;

        auto coro = static_cast<void*>(block + 1);
        return coro;

        // std::printf(
        //   "group sibling %zu -> each: %zu group: %zu\n", n, each_size,
        //   group_cap
        // );
      } else {
        size_t totalSize = sizeof(group_alloc_header) + eachSize;
        if (totalSize < 4096) {
          totalSize = 4096;
        }
        return new_alloc_group(block, totalSize, eachSize);
      }
    } else {
      // Handle allocations from non-TMC threads
      size_t totalSize = sizeof(group_alloc_header) + eachSize;
      // if (total_size < 4096) {
      //   total_size = 4096;
      // }
      return new_alloc_group(nullptr, totalSize, eachSize);
      // std::printf("standalone new %zu -> %zu\n", n, each_size);
    }
  }

  // static void* operator new(size_t n, std::align_val_t al) noexcept {
  //   // Don't try to round up the allocation size if there is also a required
  //   // alignment. If we end up with size > alignment, that could cause
  //   issues. return ::operator new(n, al);
  // }

#ifndef __clang__
  // GCC creates a TON of warnings if this is missing with the noexcept new
  static task<Result> get_return_object_on_allocation_failure() { return {}; }
#endif

  static void operator delete(void* frame, std::size_t n) noexcept {
    size_t eachSize = ((sizeof(per_alloc_block) + n + 63) & -64);
    auto prevBlock = reinterpret_cast<per_alloc_block*>(frame) - 1;
    auto block = reinterpret_cast<per_alloc_block*>(
      reinterpret_cast<char*>(prevBlock) + eachSize
    );

    // RMW instead of fetch_or; nobody else can modify this value
    auto spaceAfter = block->space_after.load(std::memory_order_relaxed);
    auto freed = spaceAfter |= FREE_BLOCK_FLAG;
    block->space_after.store(freed, std::memory_order_release);

    // auto prevBlock = reinterpret_cast<per_alloc_block*>(frame) - 1;
    // // prevBlock->next_block.store(nullptr, std::memory_order_relaxed);
    // //  TODO how to handle out-of-order frees?
    // if (prevBlock->prev_block == nullptr) {
    //   group_alloc_header* header =
    //     reinterpret_cast<group_alloc_header*>(prevBlock) - 1;
    //   if (header->mode != ALLOC_MODE_STACK) {
    //     detail::this_thread::cache_free(
    //       static_cast<void*>(header), prevBlock->space_after +
    //                                     sizeof(group_alloc_header) +
    //                                     sizeof(per_alloc_block)
    //     );
    //   }
    // }
  }

#endif // TMC_CUSTOM_CORO_ALLOC

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
  TMC_FORCE_INLINE inline std::coroutine_handle<>
  await_suspend(std::coroutine_handle<> Outer) noexcept {
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
