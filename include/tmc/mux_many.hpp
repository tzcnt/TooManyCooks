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
/// There are two families of construction:
///
/// 1. With awaitables (an iterator + count, a begin/end range, a begin/end range
/// + max count, or a range object). The `Result` type is deduced from the
/// awaitables via CTAD, and a right-sized `std::vector<Result>` is used. This
/// eagerly initiates the awaitables, exactly like `tmc::spawn_many(...)`, but
/// their results are consumed one at a time as they become ready:
/// ```
/// auto mux = tmc::mux_many(tasks.begin(), tasks.end());
/// for (size_t i = co_await mux; i != mux.end(); i = co_await mux) {
///   use(mux[i]);
/// }
/// ```
/// To store the results in a fixed-size `std::array<Result, Count>` instead, name
/// both template arguments explicitly - CTAD cannot deduce only `Result` while
/// you fix `Count`:
/// ```
/// auto mux = tmc::mux_many<int, N>(tasks.begin());              // exactly N
/// auto mux = tmc::mux_many<int, N>(tasks.begin(), tasks.end()); // up to N
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
template <typename Result, size_t Count = 0>
class mux_many : private tmc::detail::AwaitTagNoGroupCoAwaitLvalue {
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
  /// `Result` type and a non-zero `Count` must be provided explicitly, e.g.
  /// `tmc::mux_many<Result, Count>()`. Use `fork()` to initiate work into
  /// individual slots.
  mux_many()
      : executor{tmc::detail::this_thread::executor()},
        continuation_executor{tmc::detail::this_thread::executor()},
        prio{tmc::detail::this_thread::this_task().prio} {
    set_done_count(0);
    task_count = Count;
  }

  /// Creates runtime-sized result storage but does not initiate any awaitables.
  /// The `Result` type must be provided explicitly and `Count` must be zero, e.g.
  /// `tmc::mux_many<Result>(RuntimeMaxCount)`. Use `fork()` to initiate work into
  /// individual slots.
  mux_many(size_t RuntimeMaxCount)
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
  /// `[Iter, Iter + Count)` if `Count` is non-zero).
  template <typename TaskIter>
  inline mux_many(TaskIter Iter, size_t TaskCount)
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

  /// Eagerly initiates awaitables from `[Begin, min(Begin + MaxCount, End))`.
  template <typename TaskIter>
  inline mux_many(TaskIter Begin, TaskIter End, size_t MaxCount)
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

  /// Eagerly initiates exactly `Count` awaitables from `[Begin, Begin + Count)`.
  /// Only available when `Count` is non-zero (fixed-size `std::array` storage);
  /// name both template arguments explicitly, e.g.
  /// `tmc::mux_many<Result, Count>(Begin)`.
  template <typename AwaitableIter>
    requires(Count != 0)
  inline mux_many(AwaitableIter&& Begin)
      : mux_many(std::forward<AwaitableIter>(Begin), 0) {}

  /// Eagerly initiates awaitables from `[Begin, End)`. When `Count` is zero the
  /// `Result` type is deduced via CTAD and a right-sized `std::vector` is used;
  /// name both template arguments explicitly to use fixed-size `std::array`
  /// storage of up to `Count` awaitables.
  template <typename AwaitableIter>
    requires(requires(AwaitableIter a, AwaitableIter b) {
              ++a;
              *a;
              a != b;
            })
  inline mux_many(AwaitableIter&& Begin, AwaitableIter&& End)
      : mux_many(
          std::forward<AwaitableIter>(Begin), std::forward<AwaitableIter>(End),
          TMC_ALL_ONES
        ) {}

  /// Eagerly initiates awaitables from `[Range.begin(), Range.end())`. The
  /// `Result` type is deduced via CTAD and a right-sized `std::vector` is used.
  /// Only available when `Count` is zero.
  template <typename AwaitableRange>
    requires(Count == 0 && requires(std::remove_reference_t<AwaitableRange>& r) {
              r.begin();
              r.end();
            })
  inline mux_many(AwaitableRange&& Range)
      : mux_many(Range.begin(), Range.end(), TMC_ALL_ONES) {}

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
  mux_many& operator co_await() & noexcept { return *this; }

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
  /// result has already been consumed by `co_await`. The replacement awaitable must
  /// produce the same `Result` type as the group.
  ///
  /// `fork()` destroys the previous result in the given slot before initiating the
  /// replacement. If you need to use the result after this, you should move it out before
  /// calling `fork()`.
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

    // Destroy the previously-consumed result before initiating the replacement. This
    // prevents issues with results that own a resource which the replacement awaitable
    // needs to re-acquire, such as zero-copy queue scopes.
    if constexpr (!std::is_void_v<Result>) {
      result_arr[idx] = ResultStorage{};
    }

    tmc::ex_any* exec = tmc::detail::get_executor_traits<Exec>::type_erased(Executor);
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

  /// This is a dummy awaitable. Don't store this in a variable.
  /// For HALO to work, you must `co_await mux.fork_clang()` immediately.
  class TMC_CORO_AWAIT_ELIDABLE mux_many_fork_clang : tmc::detail::AwaitTagNoGroupAsIs {
  public:
    mux_many_fork_clang() {}

    /// Never suspends.
    bool await_ready() const noexcept { return true; }

    /// Does nothing.
    void await_suspend(std::coroutine_handle<>) noexcept {}

    /// Does nothing.
    void await_resume() noexcept {}
  };

  /// Similar to `fork()` but allows the forked task's allocation to be elided
  /// by combining it into the parent's allocation (HALO). This works by using
  /// specific attributes that are only available on Clang 20+. You can safely
  /// call this function on other compilers, but no HALO-specific optimizations
  /// will be applied.
  ///
  /// This method is not thread-safe.
  ///
  /// WARNING: Don't allow coroutines passed into this to cross a loop boundary,
  /// or Clang will try to reuse the same allocation for multiple active
  /// coroutines.
  ///
  /// IMPORTANT: This returns a dummy awaitable. For HALO to work, you should
  /// not store the dummy awaitable. Instead, `co_await` this expression
  /// immediately. Proper usage:
  /// ```
  /// auto mux = tmc::mux_many<int, 2>();
  /// co_await mux.fork_clang(0, task(0));
  /// co_await mux.fork_clang(1, task(1));
  /// for (size_t i = co_await mux; i != mux.end(); i = co_await mux) { ... }
  /// ```
  template <typename T, typename Exec = tmc::ex_any*>
  [[nodiscard("You must co_await fork_clang() immediately for HALO to be possible.")]]
  mux_many_fork_clang fork_clang(
    size_t idx, TMC_CORO_AWAIT_ELIDABLE_ARGUMENT T&& Task,
    Exec&& Executor = tmc::current_executor(), size_t Priority = tmc::current_priority()
  ) {
    fork(idx, static_cast<T&&>(Task), static_cast<Exec&&>(Executor), Priority);
    return mux_many_fork_clang{};
  }

  // This must be awaited and all child tasks completed before destruction.
#ifndef NDEBUG
  ~mux_many() noexcept {
    assert(remaining_count == 0 && "You must submit or co_await this.");
  }
#endif

  // Not movable or copyable due to awaitables being initiated with pointers to
  // this.
  mux_many& operator=(const mux_many& other) = delete;
  mux_many(const mux_many& other) = delete;
  mux_many& operator=(mux_many&& other) = delete;
  mux_many(mux_many&& other) = delete;
};

// Deduction guides for the eager construction forms. Each deduces the `Result`
// type from the awaitables and selects vector storage (`Count == 0`); a
// right-sized `std::vector<Result>` is allocated. To store results in a
// fixed-size `std::array<Result, Count>` instead, name both template arguments
// explicitly - CTAD cannot deduce only `Result` while you fix `Count`, so there
// is no guide for the explicit-`Count` forms.
//
// The eager constructors are member templates whose own parameters do not appear
// in the class's `Result`/`Count` parameter list, so their implicit deduction
// guides cannot deduce those parameters and are discarded - these written guides
// are the only viable ones.
//
// Note: You must ensure the iterators/range remain in scope until the group has
// been `co_await` ed.

/// Eagerly initiates items in range [Begin, Begin + TaskCount).
template <
  typename AwaitableIter, typename Awaitable = std::iter_value_t<AwaitableIter>,
  typename Result = tmc::detail::awaitable_result_t<Awaitable>>
mux_many(AwaitableIter&&, size_t) -> mux_many<Result, 0>;

/// Eagerly initiates items in range [Begin, End).
template <
  typename AwaitableIter, typename Awaitable = std::iter_value_t<AwaitableIter>,
  typename Result = tmc::detail::awaitable_result_t<Awaitable>>
mux_many(AwaitableIter&&, AwaitableIter&&) -> mux_many<Result, 0>;

/// Eagerly initiates items in range [Begin, min(Begin + MaxCount, End)).
template <
  typename AwaitableIter, typename Awaitable = std::iter_value_t<AwaitableIter>,
  typename Result = tmc::detail::awaitable_result_t<Awaitable>>
mux_many(AwaitableIter&&, AwaitableIter&&, size_t) -> mux_many<Result, 0>;

/// Eagerly initiates items in range [Range.begin(), Range.end()). The
/// `range_iter` default template argument keeps this guide from matching a
/// non-range single argument (e.g. the runtime-capacity `size_t` constructor).
template <
  typename AwaitableRange,
  typename AwaitableIter = tmc::detail::range_iter<AwaitableRange>::type,
  typename Awaitable = std::iter_value_t<AwaitableIter>,
  typename Result = tmc::detail::awaitable_result_t<Awaitable>>
mux_many(AwaitableRange&&) -> mux_many<Result, 0>;

} // namespace tmc
