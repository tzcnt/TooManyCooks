// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/current.hpp"
#include "tmc/detail/awaitable_customizer.hpp"
#include "tmc/detail/compat.hpp"
#include "tmc/detail/concepts_awaitable.hpp" // IWYU pragma: keep
#include "tmc/detail/concepts_work_item.hpp"
#include "tmc/detail/result_each.hpp"
#include "tmc/detail/task_unsafe.hpp"
#include "tmc/detail/thread_locals.hpp"
#include "tmc/ex_any.hpp"
#include "tmc/work_item.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <coroutine>
#include <cstddef>
#include <iterator>
#include <type_traits>
#include <vector>

namespace tmc {

/// A standalone, reusable result-multiplexer over a homogeneous set of awaitable
/// slots. Like `tmc::spawn_many()` it initiates a set of awaitables that all
/// produce the same `Result` type, but rather than returning all results at
/// once, each result is made available as it becomes ready: `co_await`ing the
/// group returns the index of a single ready slot. Unlike `spawn_many()`,
/// passing the awaitables up front is optional.
///
/// `mux_many` is constructed via the free function `tmc::mux_many()`, which
/// mirrors the overloads of `tmc::spawn_many()`. There are two families of
/// construction:
///
/// 1. With awaitables (an iterator, an iterator + count, a begin/end range, or a
/// range object). This eagerly initiates the awaitables, exactly like
/// `tmc::spawn_many(...)`, but their results are consumed one at a time as they
/// become ready:
/// ```
/// auto mux = tmc::mux_many(tasks.begin(), tasks.end());
/// for (size_t i = co_await mux; i != mux.end(); i = co_await mux) {
///   use(mux[i]);
/// }
/// ```
///
/// 2. Empty (the `Result` type, and optionally `Count`, must be provided
/// explicitly). This only allocates the result storage; no awaitables are
/// initiated. You start individual slots with `fork()` whenever you like,
/// optionally choosing a specific executor and priority for each one:
/// ```
/// auto mux = tmc::mux_many<int, 2>();      // fixed-size (std::array)
/// auto mux = tmc::mux_many<int>(capacity); // runtime-sized (std::vector)
/// mux.fork(0, make_int_task());
/// mux.fork(1, make_int_task());
/// for (size_t i = co_await mux; i != mux.end(); i = co_await mux) { ... }
/// ```
///
/// In both cases, after a slot's result has been consumed by `co_await`, you may
/// call `fork()` to launch a fresh awaitable into that slot. The replacement
/// awaitable must produce the same `Result` type. This is what allows a
/// `mux_many` to maintain a fixed level of concurrency: as each result is
/// consumed, replace it with new work.
///
/// `Result` is the result type produced by every awaitable in the group.
/// void-returning awaitables are supported (no result storage is allocated, and
/// `operator[]` is a no-op). Non-default-constructible results are wrapped in a
/// std::optional.
///
/// `Count` determines the result storage strategy:
/// - If `Count` is non-zero, a fixed-size `std::array<Result, Count>` is used.
/// - If `Count` is zero, a `std::vector<Result>` is used (sized to the number of
///   eagerly-initiated awaitables, or to the runtime capacity for the empty
///   constructor).
template <typename Result, size_t Count>
class aw_mux_many : private tmc::detail::AwaitTagNoGroupCoAwaitLvalue {
  // Tasks are synchronized via an atomic bitmask with only 63 (or 31, on 32-bit)
  // slots for tasks.
  static_assert(
    Count < TMC_PLATFORM_BITS,
    "mux_many supports at most 63 awaitables (31 on 32-bit platforms)."
  );

  // The count of submitted-but-not-yet-consumed results.
  ptrdiff_t remaining_count;
  std::coroutine_handle<> continuation;
  // The executor and priority used to initiate the eager constructor's
  // awaitables. fork() takes its own executor/priority arguments.
  tmc::ex_any* executor;
  tmc::ex_any* continuation_executor;
  size_t prio;
  // The number of slots (the valid index range for fork()). For the eager
  // constructors this is the number of awaitables actually initiated; for the
  // empty constructors it is the requested capacity.
  size_t task_count;
#ifndef NDEBUG
  size_t pending_or_ready_slots;
#endif
  // the atomic synchronization variable that coordinates between this and
  // awaitable_customizer (task's final_suspend)
  std::atomic<size_t> sync_flags;

  struct empty {};
  using ResultStorage = tmc::detail::result_storage_t<Result>;
  using ResultArray = std::conditional_t<
    std::is_void_v<Result>, empty,
    std::conditional_t<
      Count == 0, std::vector<ResultStorage>, std::array<ResultStorage, Count>>>;
  TMC_NO_UNIQUE_ADDRESS ResultArray result_arr;

  // Prepares the work item but does not initiate it.
  template <typename T>
  TMC_FORCE_INLINE inline void
  prepare_work(T& Task, size_t Idx, size_t ContinuationPrio) {
    tmc::detail::get_awaitable_traits<T>::set_continuation(Task, &continuation);
    tmc::detail::get_awaitable_traits<T>::set_continuation_executor(
      Task, &continuation_executor
    );
    tmc::detail::get_awaitable_traits<T>::set_done_count(Task, &sync_flags);
    // Overwriting flags clobbers the continuation priority that
    // awaitable_customizer_base captured by default. The EACH path packs the
    // EACH bit and this slot's task number into flags, so the continuation
    // priority (ContinuationPrio - the priority the awaiting coroutine resumes
    // at, read back as flags & PRIORITY_MASK) must be folded back into the low
    // bits here.
    tmc::detail::get_awaitable_traits<T>::set_flags(
      Task, tmc::detail::task_flags::EACH |
              (Idx << tmc::detail::task_flags::TASKNUM_LOW_OFF) | ContinuationPrio
    );
    if constexpr (!std::is_void_v<Result>) {
      tmc::detail::get_awaitable_traits<T>::set_result_ptr(Task, &result_arr[Idx]);
    }
  }

  void set_done_count(size_t NumTasks) {
    remaining_count = static_cast<ptrdiff_t>(NumTasks);
    task_count = NumTasks;
#ifndef NDEBUG
    pending_or_ready_slots = (TMC_ONE_BIT << NumTasks) - 1;
#endif
    sync_flags.store(tmc::detail::task_flags::EACH, std::memory_order_release);
  }

public:
  /// Creates the result storage but does not initiate any awaitables. The
  /// `Result` type and a non-zero `Count` must be provided explicitly. Use
  /// `fork()` to initiate work into individual slots. It is recommended to
  /// call `tmc::mux_many<Result, Count>()` instead of this constructor directly.
  aw_mux_many()
      : executor{tmc::detail::this_thread::executor()},
        continuation_executor{tmc::detail::this_thread::executor()},
        prio{tmc::detail::this_thread::this_task().prio} {
    set_done_count(0);
    task_count = Count;
  }

  /// Creates runtime-sized result storage but does not initiate any awaitables.
  /// The `Result` type must be provided explicitly and `Count` must be zero. Use
  /// `fork()` to initiate work into individual slots. It is recommended to
  /// call `tmc::mux_many<Result>(RuntimeMaxCount)` instead of this constructor
  /// directly.
  aw_mux_many(size_t RuntimeMaxCount)
    requires(Count == 0)
      : executor{tmc::detail::this_thread::executor()},
        continuation_executor{tmc::detail::this_thread::executor()},
        prio{tmc::detail::this_thread::this_task().prio} {
    size_t capacity = RuntimeMaxCount;
    if (capacity > TMC_PLATFORM_BITS - 1) {
      capacity = TMC_PLATFORM_BITS - 1;
    }
    if constexpr (!std::is_void_v<Result>) {
      result_arr.resize(capacity);
    }
    set_done_count(0);
    task_count = capacity;
  }

  /// Eagerly initiates awaitables from `[Iter, Iter + TaskCount)` (or
  /// `[Iter, Iter + Count)` if `Count` is non-zero). It is recommended to call
  /// `tmc::mux_many()` instead of this constructor directly.
  template <typename TaskIter>
  inline aw_mux_many(TaskIter Iter, size_t TaskCount)
      : executor{tmc::detail::this_thread::executor()},
        continuation_executor{tmc::detail::this_thread::executor()},
        prio{tmc::detail::this_thread::this_task().prio} {
    using Awaitable = std::remove_cvref_t<std::iter_value_t<TaskIter>>;
    constexpr auto mode = tmc::detail::get_awaitable_traits<Awaitable>::mode;
    static_assert(
      mode != tmc::detail::UNKNOWN, "This doesn't appear to be an awaitable."
    );

    using WorkItem =
      std::conditional_t<mode == tmc::detail::ASYNC_INITIATE, Awaitable, work_item>;
    using WorkItemArray =
      std::conditional_t<Count == 0, std::vector<WorkItem>, std::array<WorkItem, Count>>;

    size_t size;
    if constexpr (Count != 0) {
      size = Count;
    } else {
      size = TaskCount;
      if (size > TMC_PLATFORM_BITS - 1) {
        size = TMC_PLATFORM_BITS - 1;
      }
      if constexpr (!std::is_void_v<Result>) {
        result_arr.resize(size);
      }
    }
    size_t continuationPriority = tmc::detail::this_thread::this_task().prio;

    if constexpr (mode == tmc::detail::ASYNC_INITIATE) {
      // ASYNC_INITIATE types may possibly not be stored in a vector or array
      // (no default/copy constructor), so initiate them individually.
      set_done_count(size);
      for (size_t i = 0; i < size; ++i) {
        TMC_DISABLE_WARNING_PESSIMIZING_MOVE_BEGIN
        auto t = std::move(*Iter);
        TMC_DISABLE_WARNING_PESSIMIZING_MOVE_END
        prepare_work(t, i, continuationPriority);
        tmc::detail::get_awaitable_traits<Awaitable>::async_initiate(
          std::move(t), executor, prio
        );
        ++Iter;
      }
    } else { // mode != ASYNC_INITIATE
      // Batch other types of awaitables into a work_item array/vector
      // and submit them in bulk.
      WorkItemArray taskArr;
      if constexpr (Count == 0) {
        taskArr.resize(size);
      }

      for (size_t i = 0; i < size; ++i) {
        TMC_DISABLE_WARNING_PESSIMIZING_MOVE_BEGIN
        auto t = tmc::detail::into_known<false>(std::move(*Iter));
        TMC_DISABLE_WARNING_PESSIMIZING_MOVE_END
        prepare_work(t, i, continuationPriority);
        taskArr[i] = tmc::detail::into_initiate(std::move(t));
        ++Iter;
      }

      set_done_count(size);
      if (size != 0) {
        tmc::detail::post_bulk_checked(executor, taskArr.data(), size, prio);
      }
    }
  }

  /// Eagerly initiates awaitables from `[Begin, min(Begin + MaxCount, End))`. It
  /// is recommended to call `tmc::mux_many()` instead of this constructor
  /// directly.
  template <typename TaskIter>
  inline aw_mux_many(TaskIter Begin, TaskIter End, size_t MaxCount)
    requires(requires(TaskIter a, TaskIter b) {
              ++a;
              *a;
              a != b;
            })
      : executor{tmc::detail::this_thread::executor()},
        continuation_executor{tmc::detail::this_thread::executor()},
        prio{tmc::detail::this_thread::this_task().prio} {
    size_t size;
    if constexpr (Count != 0) {
      size = Count;
    } else {
      size = MaxCount;
      if (size > TMC_PLATFORM_BITS - 1) {
        size = TMC_PLATFORM_BITS - 1;
      }
      if constexpr (requires(TaskIter a, TaskIter b) { a - b; }) {
        // Caller didn't specify capacity to preallocate, but we can calculate.
        size_t iterSize = static_cast<size_t>(End - Begin);
        if (iterSize < size) {
          size = iterSize;
        }
        if constexpr (!std::is_void_v<Result>) {
          result_arr.resize(size);
        }
      }
    }

    using Awaitable = std::remove_cvref_t<std::iter_value_t<TaskIter>>;
    constexpr auto mode = tmc::detail::get_awaitable_traits<Awaitable>::mode;
    static_assert(
      mode != tmc::detail::UNKNOWN, "This doesn't appear to be an awaitable."
    );

    using WorkItem =
      std::conditional_t<mode == tmc::detail::ASYNC_INITIATE, Awaitable, work_item>;
    using WorkItemArray =
      std::conditional_t<Count == 0, std::vector<WorkItem>, std::array<WorkItem, Count>>;

    size_t continuationPriority = tmc::detail::this_thread::this_task().prio;

    size_t taskCount = 0;
    if constexpr (Count != 0 || requires(TaskIter a, TaskIter b) { a - b; }) {
      if constexpr (mode == tmc::detail::ASYNC_INITIATE &&
                    requires(TaskIter a, TaskIter b) { a - b; }) {
        // ASYNC_INITIATE types may possibly not be stored in a vector or
        // array (no default/copy constructor). Try to sidestep this by
        // initiating them individually. For this block we also need to be able
        // to calculate the actual size beforehand.
        size_t actualSize = static_cast<size_t>(End - Begin);
        if (size < actualSize) {
          actualSize = size;
        }
        set_done_count(actualSize);
        while (Begin != End && taskCount < actualSize) {
          TMC_DISABLE_WARNING_PESSIMIZING_MOVE_BEGIN
          auto t = std::move(*Begin);
          TMC_DISABLE_WARNING_PESSIMIZING_MOVE_END
          prepare_work(t, taskCount, continuationPriority);
          tmc::detail::get_awaitable_traits<Awaitable>::async_initiate(
            std::move(t), executor, prio
          );
          ++Begin;
          ++taskCount;
        }
      } else { // mode != ASYNC_INITIATE || uncountable
        WorkItemArray taskArr;
        if constexpr (Count == 0) {
          taskArr.resize(size);
        }
        // Iterator could produce less than Count tasks, so count them.
        // Iterator could produce more than Count tasks - stop after taking
        // Count.
        while (Begin != End && taskCount < size) {
          TMC_DISABLE_WARNING_PESSIMIZING_MOVE_BEGIN
          auto t = tmc::detail::into_known<false>(std::move(*Begin));
          TMC_DISABLE_WARNING_PESSIMIZING_MOVE_END
          prepare_work(t, taskCount, continuationPriority);
          taskArr[taskCount] = tmc::detail::into_initiate(std::move(t));
          ++Begin;
          ++taskCount;
        }

        if (taskCount == 0) {
          set_done_count(0);
          return;
        }
        if constexpr (mode == tmc::detail::ASYNC_INITIATE) {
          set_done_count(taskCount);
          for (size_t i = 0; i < taskCount; ++i) {
            tmc::detail::get_awaitable_traits<Awaitable>::async_initiate(
              std::move(taskArr[i]), executor, prio
            );
          }
        } else {
          set_done_count(taskCount);
          tmc::detail::post_bulk_checked(executor, taskArr.data(), taskCount, prio);
        }
      }
    } else {
      // We have no idea how many awaitables there will be.
      // This introduces some complexity - we need to count all of the
      // awaitables before we can set done_count, and we need to appropriately
      // size the result vector before we can configure each awaitable's
      // result_ptr. This means that the awaitables must be collected into a
      // vector so that they can be configured afterward. If the awaitable
      // type is not copy-constructible, this will not compile.
      if constexpr (mode == tmc::detail::TMC_TASK ||
                    mode == tmc::detail::ASYNC_INITIATE || mode == tmc::detail::WRAPPER) {
        // These types can be processed using a single vector.
        WorkItemArray taskArr;
        while (Begin != End && taskCount < size) {
          TMC_DISABLE_WARNING_PESSIMIZING_MOVE_BEGIN
          taskArr.emplace_back(
            tmc::detail::into_initiate(tmc::detail::into_known<false>(std::move(*Begin)))
          );
          TMC_DISABLE_WARNING_PESSIMIZING_MOVE_END
          ++Begin;
          ++taskCount;
        }

        // We couldn't bind result_ptr before we determined how many tasks
        // there are, because reallocation would invalidate those pointers.
        // Now bind them.
        // This also injects a 2nd pass into the void-result case, but it
        // makes it simpler to maintain.
        if constexpr (!std::is_void_v<Result>) {
          result_arr.resize(taskCount);
        }
        for (size_t i = 0; i < taskCount; ++i) {
          if constexpr (mode == tmc::detail::ASYNC_INITIATE) {
            prepare_work(taskArr[i], i, continuationPriority);
          } else { // TMC_TASK or WRAPPER
            auto t = tmc::detail::task_unsafe<Result>::from_address(
              TMC_WORK_ITEM_AS_STD_CORO(taskArr[i]).address()
            );
            prepare_work(t, i, continuationPriority);
          }
        }

        if (taskCount == 0) {
          set_done_count(0);
          return;
        }
        if constexpr (mode == tmc::detail::ASYNC_INITIATE) {
          set_done_count(taskCount);
          for (size_t i = 0; i < taskCount; ++i) {
            tmc::detail::get_awaitable_traits<Awaitable>::async_initiate(
              std::move(taskArr[i]), executor, prio
            );
          }
        } else {
          set_done_count(taskCount);
          tmc::detail::post_bulk_checked(executor, taskArr.data(), taskCount, prio);
        }
      } else if constexpr (mode == tmc::detail::COROUTINE) {
        // These types must be stored in a separate vector that preserves the
        // original type, then configured, then transformed into work_item and
        // submitted in batches.
        std::vector<Awaitable> originalCoroArr;
        while (Begin != End && taskCount < size) {
          TMC_DISABLE_WARNING_PESSIMIZING_MOVE_BEGIN
          originalCoroArr.emplace_back(std::move(*Begin));
          TMC_DISABLE_WARNING_PESSIMIZING_MOVE_END
          ++Begin;
          ++taskCount;
        }

        if (taskCount == 0) {
          set_done_count(0);
          return;
        }

        // We couldn't bind result_ptr before we determined how many tasks
        // there are, because reallocation would invalidate those pointers.
        // Now bind them.
        // This also injects a 2nd pass into the void-result case, but it
        // makes it simpler to maintain.
        if constexpr (!std::is_void_v<Result>) {
          result_arr.resize(taskCount);
        }
        for (size_t i = 0; i < taskCount; ++i) {
          prepare_work(originalCoroArr[i], i, continuationPriority);
        }

        set_done_count(taskCount);

        std::array<tmc::work_item, 64> workItemArr;
        size_t totalCount = 0;
        while (totalCount < taskCount) {
          size_t submitCount = 0;
          while (submitCount < workItemArr.size() && totalCount < taskCount) {
            workItemArr[submitCount] =
              tmc::detail::into_initiate(std::move(originalCoroArr[totalCount]));
            ++totalCount;
            ++submitCount;
          }
          tmc::detail::post_bulk_checked(executor, workItemArr.data(), submitCount, prio);
        }
      }
    }
  }

  /// Suspends if there are no ready results.
  inline bool await_ready() const noexcept {
    return tmc::detail::result_each_await_ready();
  }

  /// Suspends the outer coroutine until a result becomes ready (or resumes
  /// immediately if one is already ready).
  inline bool await_suspend(std::coroutine_handle<> Outer) noexcept {
    return tmc::detail::result_each_await_suspend(
      remaining_count, Outer, continuation, continuation_executor, sync_flags
    );
  }

  /// Returns the index of a single ready slot. The result indexes correspond to
  /// the indexes of the originally submitted awaitables, and the values can be
  /// accessed using `operator[]`. Results may become ready in any order, but
  /// each consumed (or forked) slot index will be returned exactly once per
  /// submission. When no submitted results remain, the index returned will be
  /// equal to the value of `end()`.
  TMC_AWAIT_RESUME inline size_t await_resume() noexcept {
    auto slot = tmc::detail::result_each_await_resume(remaining_count, sync_flags);
#ifndef NDEBUG
    if (slot != end()) {
      pending_or_ready_slots &= ~(TMC_ONE_BIT << slot);
    }
#endif
    return slot;
  }

  /// This type must be awaited as an lvalue (it is awaited repeatedly and is not
  /// movable). The awaiter is the group itself.
  aw_mux_many& operator co_await() & noexcept { return *this; }

  /// Provides a sentinel value that can be compared against the value returned
  /// from co_await.
  inline size_t end() noexcept { return 64; }

  /// Gets the ready result at the given index.
  inline std::add_lvalue_reference_t<ResultStorage> operator[](size_t idx) noexcept
    requires(!std::is_void_v<Result>)
  {
    assert(idx < result_arr.size());
    return result_arr[idx];
  }

  /// Provided for convenience only - to expose the same API as the non-void
  /// version. Does nothing.
  inline void operator[]([[maybe_unused]] size_t idx) noexcept
    requires(std::is_void_v<Result>)
  {}

  /// Starts a new awaitable in the given slot and initiates it immediately on
  /// the specified executor and priority. The slot must be empty - either it was
  /// never started (when constructed with an empty constructor), or its previous
  /// result has already been consumed by `co_await`. Read the current result
  /// before calling this, since the next completion will overwrite the slot. The
  /// replacement awaitable must produce the same `Result` type as the group.
  ///
  /// `Executor` defaults to the current executor.
  /// `Priority` defaults to the current priority.
  ///
  /// Only the awaitable's dispatch is customized; regardless of where each slot
  /// runs, the awaiting coroutine always resumes on the executor that was
  /// current when this `mux_many` was constructed. The eager constructors do not
  /// offer this per-awaitable customization - they initiate every awaitable on
  /// the executor and priority current at construction. To customize each
  /// awaitable, use an empty constructor and `fork()` each slot.
  ///
  /// This method is not thread-safe.
  template <typename T, typename Exec = tmc::ex_any*>
  inline void fork(
    size_t idx, T&& Task, Exec&& Executor = tmc::current_executor(),
    size_t Priority = tmc::current_priority()
  ) {
    if (idx >= task_count) [[unlikely]] {
      assert(false && "fork() index out of range.");
      return;
    }
#ifndef NDEBUG
    auto slotBit = TMC_ONE_BIT << idx;
    assert(
      0 == (pending_or_ready_slots & slotBit) &&
      "You may only fork a slot after its previous result has been awaited."
    );
#endif

    decltype(auto) known = tmc::detail::into_known<false>(static_cast<T&&>(Task));
    using KnownAwaitable = std::remove_reference_t<decltype(known)>;
    static_assert(
      std::is_same_v<
        typename tmc::detail::get_awaitable_traits<KnownAwaitable>::result_type, Result>,
      "Replacement awaitable must have the same result type as the mux_many "
      "group."
    );

    tmc::ex_any* exec = tmc::detail::get_executor_traits<Exec>::type_erased(Executor);
    // The continuation priority is the priority the awaiting (consumer)
    // coroutine resumes at - the current priority, the same value
    // awaitable_customizer_base captures by default. It is independent of this
    // awaitable's dispatch priority (the Priority argument), so it is taken from
    // the current task, not from Priority.
    auto continuationPriority = tmc::detail::this_thread::this_task().prio;
#ifndef NDEBUG
    pending_or_ready_slots |= slotBit;
#endif
    ++remaining_count;

    constexpr auto mode = tmc::detail::get_awaitable_traits<KnownAwaitable>::mode;
    if constexpr (mode == tmc::detail::ASYNC_INITIATE) {
      prepare_work(known, idx, continuationPriority);
      // Forward known with its original value category.
      tmc::detail::get_awaitable_traits<KnownAwaitable>::async_initiate(
        static_cast<decltype(known)&&>(known), exec, Priority
      );
    } else {
      prepare_work(known, idx, continuationPriority);
      auto item = tmc::detail::into_initiate(static_cast<decltype(known)&&>(known));
      tmc::detail::post_checked(exec, std::move(item), Priority);
    }
  }

  // This must be awaited and all child tasks completed before destruction.
#ifndef NDEBUG
  ~aw_mux_many() noexcept {
    assert(remaining_count == 0 && "You must submit or co_await this.");
  }
#endif

  // Not movable or copyable due to awaitables being initiated with pointers to
  // this.
  aw_mux_many& operator=(const aw_mux_many& other) = delete;
  aw_mux_many(const aw_mux_many& other) = delete;
  aw_mux_many& operator=(aw_mux_many&& other) = delete;
  aw_mux_many(aw_mux_many&& other) = delete;
};

/// The single-argument form of mux_many() has two overloads.
/// If `Count` is non-zero (this overload), a fixed-size `std::array<Result,
/// Count>` will be allocated to return results in. The other overload
/// (Count == 0) supports range-types.
///
/// `AwaitableIter` must be an iterator type that implements `operator*()` and
/// `AwaitableIter& operator++()`.
///
/// Eagerly initiates items in range [Begin, Begin + Count).
///
/// Note: You must ensure the iterator remains in scope until this has been
/// `co_await` ed.
template <
  size_t Count = 0, typename AwaitableIter,
  typename Awaitable = std::iter_value_t<AwaitableIter>,
  typename Result = tmc::detail::awaitable_result_t<Awaitable>>
aw_mux_many<Result, Count> mux_many(AwaitableIter&& Begin)
  requires(Count != 0)
{
  return aw_mux_many<Result, Count>(std::forward<AwaitableIter>(Begin), 0);
}

/// The single-argument form of mux_many() has two overloads.
/// If `Count` is zero (this overload), the single argument is treated as a
/// range. The other overload (Count != 0) supports fixed-size awaitable groups.
///
/// `AwaitableRange` must implement `begin()` and `end()` methods which return an
/// iterator type.
///
/// Eagerly initiates items in range [Range.begin(), Range.end()).
///
/// Note: You must ensure the range remains in scope until this has been
/// `co_await` ed.
template <
  size_t Count = 0, typename AwaitableRange,
  typename AwaitableIter = tmc::detail::range_iter<AwaitableRange>::type,
  typename Awaitable = std::iter_value_t<AwaitableIter>,
  typename Result = tmc::detail::awaitable_result_t<Awaitable>>
aw_mux_many<Result, 0> mux_many(AwaitableRange&& Range)
  requires(Count == 0)
{
  return aw_mux_many<Result, 0>(Range.begin(), Range.end(), TMC_ALL_ONES);
}

/// For use when the number of items is a runtime parameter.
///
/// `AwaitableIter` must be an iterator type that implements `operator*()` and
/// `AwaitableIter& operator++()`.
///
/// Eagerly initiates items in range [Begin, Begin + TaskCount).
///
/// Note: You must ensure the iterator remains in scope until this has been
/// `co_await` ed.
template <
  typename AwaitableIter, typename Awaitable = std::iter_value_t<AwaitableIter>,
  typename Result = tmc::detail::awaitable_result_t<Awaitable>>
aw_mux_many<Result, 0> mux_many(AwaitableIter&& Begin, size_t TaskCount) {
  return aw_mux_many<Result, 0>(std::forward<AwaitableIter>(Begin), TaskCount);
}

/// For use when the number of items may be variable.
///
/// `AwaitableIter` must be an iterator type that implements `operator*()`,
/// `AwaitableIter& operator++()`, and `operator==(AwaitableIter const& rhs)`.
///
/// - If `MaxCount` is non-zero, a fixed-size `std::array<Result, MaxCount>` is
/// allocated; up to `MaxCount` tasks are consumed from the iterator.
/// - If `MaxCount` is zero/not provided, a right-sized `std::vector<Result>` is
/// allocated.
///
/// Eagerly initiates items in range [Begin, min(Begin + MaxCount, End)).
///
/// Note: You must ensure the iterators remain in scope until this has been
/// `co_await` ed.
template <
  size_t MaxCount = 0, typename AwaitableIter,
  typename Awaitable = std::iter_value_t<AwaitableIter>,
  typename Result = tmc::detail::awaitable_result_t<Awaitable>>
aw_mux_many<Result, MaxCount> mux_many(AwaitableIter&& Begin, AwaitableIter&& End) {
  return aw_mux_many<Result, MaxCount>(
    std::forward<AwaitableIter>(Begin), std::forward<AwaitableIter>(End), TMC_ALL_ONES
  );
}

/// For use when the number of items may be variable.
///
/// `AwaitableIter` must be an iterator type that implements `operator*()`,
/// `AwaitableIter& operator++()`, and `operator==(AwaitableIter const& rhs)`.
///
/// Up to `MaxCount` tasks are consumed from the iterator; a right-sized
/// `std::vector<Result>` is allocated.
///
/// Eagerly initiates items in range [Begin, min(Begin + MaxCount, End)).
///
/// Note: You must ensure the iterators remain in scope until this has been
/// `co_await` ed.
template <
  typename AwaitableIter, typename Awaitable = std::iter_value_t<AwaitableIter>,
  typename Result = tmc::detail::awaitable_result_t<Awaitable>>
aw_mux_many<Result, 0>
mux_many(AwaitableIter&& Begin, AwaitableIter&& End, size_t MaxCount) {
  return aw_mux_many<Result, 0>(
    std::forward<AwaitableIter>(Begin), std::forward<AwaitableIter>(End), MaxCount
  );
}

/// Creates an empty `mux_many` with fixed-size result storage. No awaitables are
/// initiated; use `fork()` to launch work into individual slots.
///
/// `Result` is the result type produced by every awaitable. `Count` must be
/// non-zero (it determines the `std::array<Result, Count>` storage size). For a
/// runtime-sized group, use the `mux_many<Result>(RuntimeMaxCount)` overload.
template <typename Result, size_t Count = 0>
  requires(Count != 0)
aw_mux_many<Result, Count> mux_many() {
  return aw_mux_many<Result, Count>();
}

/// Creates an empty `mux_many` with runtime-sized result storage. No awaitables
/// are initiated; use `fork()` to launch work into individual slots.
///
/// `Result` is the result type produced by every awaitable. A
/// `std::vector<Result>` of size `RuntimeMaxCount` is pre-allocated to store the
/// results (capped at 63 / 31 slots).
template <typename Result> aw_mux_many<Result, 0> mux_many(size_t RuntimeMaxCount) {
  return aw_mux_many<Result, 0>(RuntimeMaxCount);
}

} // namespace tmc
