// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/awaitable_customizer.hpp"
#include "tmc/detail/compat.hpp"
#include "tmc/detail/concepts_awaitable.hpp" // IWYU pragma: keep
#include "tmc/detail/concepts_work_item.hpp"
#include "tmc/detail/mixins.hpp"
#include "tmc/detail/result_each.hpp"
#include "tmc/detail/task_unsafe.hpp"
#include "tmc/detail/thread_locals.hpp"
#include "tmc/ex_any.hpp"
#include "tmc/task.hpp"
#include "tmc/work_item.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <coroutine>
#include <iterator>
#include <type_traits>
#include <vector>

namespace tmc {
/// The customizable task wrapper / awaitable type returned by
/// `tmc::spawn_many()` / `tmc::spawn_func_many()`.
template <
  typename Result, size_t Count, typename IterBegin, typename IterEnd,
  bool IsFunc>
class aw_spawn_many;

/// The single-argument form of spawn_many() has two overloads.
/// If `Count` is non-zero (this overload), a fixed-size `std::array<T, Count>`
/// will be allocated to return results in. The other overload (Count == 0)
/// supports range-types.
///
/// `AwaitableIter` must be an iterator type that implements `operator*()` and
/// `AwaitableIter& operator++()`.
///
/// `Awaitable` must be a type that can be awaited
/// (implements `operator co_await()` or `await_ready/suspend/resume()`).
///
/// Submits items in range [Begin, Begin + Count) to the executor.
///
/// Note: You must ensure the iterator remains in scope until this has
/// been `co_await` ed. A temporary iterator is OK only if this
/// is `co_await` ed immediately.
template <
  size_t Count = 0, typename AwaitableIter,
  typename Awaitable = std::iter_value_t<AwaitableIter>,
  typename Result = tmc::detail::awaitable_result_t<Awaitable>>
aw_spawn_many<Result, Count, AwaitableIter, size_t, false>
spawn_many(AwaitableIter&& Begin)
  requires(Count != 0)
{
  return aw_spawn_many<Result, Count, AwaitableIter, size_t, false>(
    std::forward<AwaitableIter>(Begin), 0, 0
  );
}

/// The single-argument form of spawn_many() has two overloads.
/// If `Count` is zero (this overload), the single argument is treated as a
/// range. The other overload (Count != 0) supports fixed-size awaitable groups.
///
/// `AwaitableRange` must implement `begin()` and `end()` methods which return
/// an iterator type. The iterator type must implement `operator*()`,
/// `AwaitableIter& operator++()`, and `operator==(AwaitableIter const& rhs)`.
///
/// `Awaitable` must be a type that can be awaited (implements `operator
/// co_await()` or `await_ready/suspend/resume()`)
///
/// Submits items in range [Range.begin(), Range.end()) to the executor.
///
/// Note: You must ensure the range remains in scope until this has
/// been `co_await` ed. A temporary range is OK only if this
/// is `co_await` ed immediately.
template <
  size_t Count = 0, typename AwaitableRange,
  typename AwaitableIter = tmc::detail::range_iter<AwaitableRange>::type,
  typename Awaitable = std::iter_value_t<AwaitableIter>,
  typename Result = tmc::detail::awaitable_result_t<Awaitable>>
aw_spawn_many<Result, 0, AwaitableIter, AwaitableIter, false>
spawn_many(AwaitableRange&& Range)
  requires(Count == 0)
{
  return aw_spawn_many<Result, 0, AwaitableIter, AwaitableIter, false>(
    Range.begin(), Range.end(), TMC_ALL_ONES
  );
}

/// For use when the number of items to spawn is a runtime parameter.
///
/// `AwaitableIter` must be an iterator type that implements `operator*()` and
/// `AwaitableIter& operator++()`.
/// `TaskCount` must be non-zero.
///
/// `Awaitable` must be a type that can be awaited (implements `operator
/// co_await()` or `await_ready/suspend/resume()`)
///
/// Submits items in range [Begin, Begin + TaskCount) to the executor.
///
/// Note: You must ensure the iterator remains in scope until this has
/// been `co_await` ed. A temporary iterator is OK only if this
/// is `co_await` ed immediately.
template <
  typename AwaitableIter, typename Awaitable = std::iter_value_t<AwaitableIter>,
  typename Result = tmc::detail::awaitable_result_t<Awaitable>>
aw_spawn_many<Result, 0, AwaitableIter, size_t, false>
spawn_many(AwaitableIter&& Begin, size_t TaskCount) {
  return aw_spawn_many<Result, 0, AwaitableIter, size_t, false>(
    std::forward<AwaitableIter>(Begin), TaskCount, 0
  );
}

/// For use when the number of items to spawn may be variable.
///
/// `AwaitableIter` must be an iterator type that implements `operator*()`,
/// `AwaitableIter& operator++()`, and `operator==(AwaitableIter const& rhs)`.
///
/// `Awaitable` must be a type that can be awaited (implements `operator
/// co_await()` or `await_ready/suspend/resume()`)
///
/// - If `MaxCount` is non-zero, the return type will be a `std::array<Result,
/// MaxCount>`. Up to `MaxCount` tasks will be consumed from the
/// iterator. If the iterator produces less than `MaxCount` tasks, elements in
/// the return array beyond the number of results actually produced by the
/// iterator will be default-initialized.
/// Submits items in range [Begin, min(Begin + MaxCount, End)) to the executor.
///
/// - If `MaxCount` is zero/not provided, the return type will be a right-sized
/// `std::vector<Result>` with size and capacity equal to the number of tasks
/// produced by the iterator.
///
/// Submits items in range [Begin, End) to the executor.
///
/// Note: You must ensure the iterators remain in scope until this has
/// been `co_await` ed.
template <
  size_t MaxCount = 0, typename AwaitableIter,
  typename Awaitable = std::iter_value_t<AwaitableIter>,
  typename Result = tmc::detail::awaitable_result_t<Awaitable>>
aw_spawn_many<Result, MaxCount, AwaitableIter, AwaitableIter, false>
spawn_many(AwaitableIter&& Begin, AwaitableIter&& End) {
  return aw_spawn_many<Result, MaxCount, AwaitableIter, AwaitableIter, false>(
    std::forward<AwaitableIter>(Begin), std::forward<AwaitableIter>(End),
    TMC_ALL_ONES
  );
}

/// For use when the number of items to spawn may be variable.
///
/// `AwaitableIter` must be an iterator type that implements `operator*()`,
/// `AwaitableIter& operator++()`, and `operator==(AwaitableIter const& rhs)`.
///
/// `Awaitable` must be a type that can be awaited (implements `operator
/// co_await()` or `await_ready/suspend/resume()`)
///
/// - Up to `MaxCount` tasks will be consumed from the iterator.
/// - The iterator may produce less than `MaxCount` tasks.
/// - The return type will be a right-sized `std::vector<Result>` with size and
/// capacity equal to the number of tasks consumed from the iterator.
///
/// Submits items in range [Begin, min(Begin + MaxCount, End)) to the executor.
///
/// Note: You must ensure the iterators remain in scope until this has
/// been `co_await` ed.
template <
  typename AwaitableIter, typename Awaitable = std::iter_value_t<AwaitableIter>,
  typename Result = tmc::detail::awaitable_result_t<Awaitable>>
aw_spawn_many<Result, 0, AwaitableIter, AwaitableIter, false>
spawn_many(AwaitableIter&& Begin, AwaitableIter&& End, size_t MaxCount) {
  return aw_spawn_many<Result, 0, AwaitableIter, AwaitableIter, false>(
    std::forward<AwaitableIter>(Begin), std::forward<AwaitableIter>(End),
    MaxCount
  );
}

/// The single-argument form of spawn_func_many() has two overloads.
/// If `Count` is non-zero (this overload), a fixed-size `std::array<T, Count>`
/// will be allocated to return results in. The other overload (Count == 0)
/// supports range-types.
///
/// `Functor` must be a copyable type that implements `Result operator()`.
/// `FuncIter` must be an iterator type that implements `operator*()` and
/// `FuncIter& operator++()`.
///
/// Submits items in range [Begin, Begin + Count) to the executor.
///
/// Note: You must ensure the iterator remains in scope until this has
/// been `co_await` ed. A temporary iterator is OK only if this
/// is `co_await` ed immediately.
template <
  size_t Count = 0, typename FuncIter,
  typename Functor = std::iter_value_t<FuncIter>,
  typename Result = std::invoke_result_t<Functor&>>
aw_spawn_many<Result, Count, FuncIter, size_t, true>
spawn_func_many(FuncIter&& FunctorIterator)
  requires(Count != 0)
{
  return aw_spawn_many<Result, Count, FuncIter, size_t, true>(
    std::forward<FuncIter>(FunctorIterator), 0, 0
  );
}

/// The single-argument form of spawn_func_many() has two overloads.
/// If `Count` is zero (this overload), the single argument is treated as a
/// range. The other overload (Count != 0) supports fixed-size awaitable groups.
///
/// `FuncRange` must implement `begin()` and `end()` methods which return
/// an iterator type. The iterator type must implement `operator*()`,
/// `FuncIter& operator++()`, and `operator==(FuncIter const& rhs)`.
///
/// `Functor` must be a copyable type that implements `Result operator()`.
///
/// Submits items in range [Range.begin(), Range.end()) to the executor.
///
/// Note: You must ensure the range remains in scope until this has
/// been `co_await` ed. A temporary range is OK only if this
/// is `co_await` ed immediately.
template <
  size_t Count = 0, typename FuncRange,
  typename FuncIter = tmc::detail::range_iter<FuncRange>::type,
  typename Functor = std::iter_value_t<FuncIter>,
  typename Result = std::invoke_result_t<Functor&>>
aw_spawn_many<Result, 0, FuncIter, FuncIter, true>
spawn_func_many(FuncRange&& Range)
  requires(Count == 0)
{
  return aw_spawn_many<Result, 0, FuncIter, FuncIter, true>(
    Range.begin(), Range.end(), TMC_ALL_ONES
  );
}

/// For use when the number of items to spawn is a runtime parameter.
/// `Functor` must be a copyable type that implements `Result operator()`.
/// `FuncIter` must be an iterator type that implements `operator*()` and
/// `FuncIter& operator++()`.
/// `FunctorCount` must be non-zero.
///
/// Submits items in range [Begin, Begin + FunctorCount) to the executor.
///
/// Note: You must ensure the iterator remains in scope until this has
/// been `co_await` ed. A temporary iterator is OK only if this
/// is `co_await` ed immediately.
template <
  typename FuncIter, typename Functor = std::iter_value_t<FuncIter>,
  typename Result = std::invoke_result_t<Functor&>>
aw_spawn_many<Result, 0, FuncIter, size_t, true>
spawn_func_many(FuncIter&& FunctorIterator, size_t FunctorCount) {
  return aw_spawn_many<Result, 0, FuncIter, size_t, true>(
    std::forward<FuncIter>(FunctorIterator), FunctorCount, 0
  );
}

/// For use when the number of items to spawn may be variable.
/// `Functor` must be a copyable type that implements `Result operator()`.
/// `FuncIter` must be an iterator type that implements `operator*()`,
/// `FuncIter& operator++()`, and `operator==(FuncIter const& rhs)`.
///
/// - If `MaxCount` is non-zero, the return type will be a `std::array<Result,
/// MaxCount>`. Up to `MaxCount` tasks will be consumed from the
/// iterator. If the iterator produces less than `MaxCount` tasks, elements in
/// the return array beyond the number of results actually produced by the
/// iterator will be default-initialized.
/// Submits items in range [Begin, min(Begin + MaxCount, End)) to the executor.
///
/// - If `MaxCount` is zero/not provided, the return type will be a right-sized
/// `std::vector<Result>` with size and capacity equal to the number of tasks
/// produced by the iterator.
/// Submits items in range [Begin, End) to the executor.
///
/// Note: You must ensure the iterators remain in scope until this has
/// been `co_await` ed.
///
template <
  size_t MaxCount = 0, typename FuncIter,
  typename Functor = std::iter_value_t<FuncIter>,
  typename Result = std::invoke_result_t<Functor&>>
aw_spawn_many<Result, MaxCount, FuncIter, FuncIter, true>
spawn_func_many(FuncIter&& Begin, FuncIter&& End) {
  return aw_spawn_many<Result, MaxCount, FuncIter, FuncIter, true>(
    std::forward<FuncIter>(Begin), std::forward<FuncIter>(End), TMC_ALL_ONES
  );
}

/// For use when the number of items to spawn may be variable.
/// `Functor` must be a copyable type that implements `Result operator()`.
/// `FuncIter` must be an iterator type that implements `operator*()`,
/// `FuncIter& operator++()`, and `operator==(FuncIter const& rhs)`.
///
/// - Up to `MaxCount` tasks will be consumed from the iterator.
/// - The iterator may produce less than `MaxCount` tasks.
/// - The return type will be a right-sized `std::vector<Result>` with size and
/// capacity equal to the number of tasks consumed from the iterator.
///
/// Submits items in range [Begin, min(Begin + MaxCount, End)) to the executor.
///
/// Note: You must ensure the iterators remain in scope until this has
/// been `co_await` ed.
template <
  typename FuncIter, typename Functor = std::iter_value_t<FuncIter>,
  typename Result = std::invoke_result_t<Functor&>>
aw_spawn_many<Result, 0, FuncIter, FuncIter, true>
spawn_func_many(FuncIter&& Begin, FuncIter&& End, size_t MaxCount) {
  return aw_spawn_many<Result, 0, FuncIter, FuncIter, true>(
    std::forward<FuncIter>(Begin), std::forward<FuncIter>(End), MaxCount
  );
}

template <typename Result, size_t Count, bool IsEach, bool IsFunc>
class aw_spawn_many_impl {
public:
  union {
    std::coroutine_handle<> symmetric_task;
    ptrdiff_t remaining_count;
  };
  std::coroutine_handle<> continuation;
  tmc::ex_any* continuation_executor;
  union {
    std::atomic<ptrdiff_t> done_count;
    std::atomic<size_t> sync_flags;
  };

  struct empty {};
  using ResultArray = std::conditional_t<
    std::is_void_v<Result>, empty,
    std::conditional_t<
      Count == 0, std::vector<tmc::detail::result_storage_t<Result>>,
      std::array<tmc::detail::result_storage_t<Result>, Count>>>;
  TMC_NO_UNIQUE_ADDRESS ResultArray result_arr;

  template <typename, size_t, typename, typename, bool>
  friend class aw_spawn_many;

  // When result_each() is called, tasks are synchronized via an atomic bitmask
  // with only 63 (or 31, on 32-bit) slots for tasks. result_each() doesn't seem
  // like a good fit for larger task groups anyway. If you really need more
  // room, please open a GitHub issue explaining why...
  static_assert(!IsEach || Count < TMC_PLATFORM_BITS);

  // Prepares the work item but does not initiate it.
  template <typename T>
  TMC_FORCE_INLINE inline void
  prepare_work(T& Task, size_t Idx, [[maybe_unused]] size_t ContinuationPrio) {
    tmc::detail::get_awaitable_traits<T>::set_continuation(Task, &continuation);
    tmc::detail::get_awaitable_traits<T>::set_continuation_executor(
      Task, &continuation_executor
    );
    if constexpr (IsEach) {
      tmc::detail::get_awaitable_traits<T>::set_done_count(Task, &sync_flags);
      tmc::detail::get_awaitable_traits<T>::set_flags(
        Task, tmc::detail::task_flags::EACH |
                (Idx << tmc::detail::task_flags::TASKNUM_LOW_OFF) |
                ContinuationPrio
      );
    } else {
      tmc::detail::get_awaitable_traits<T>::set_done_count(Task, &done_count);
    }
    if constexpr (!std::is_void_v<Result>) {
      tmc::detail::get_awaitable_traits<T>::set_result_ptr(
        Task, &result_arr[Idx]
      );
    }
  }

  void set_done_count(size_t NumTasks) {
    if constexpr (IsEach) {
      remaining_count = static_cast<ptrdiff_t>(NumTasks);
      sync_flags.store(
        tmc::detail::task_flags::EACH, std::memory_order_release
      );
    } else {
      done_count.store(
        static_cast<ptrdiff_t>(NumTasks), std::memory_order_release
      );
    }
  }

  template <typename TaskIter>
  inline aw_spawn_many_impl(
    TaskIter Iter, size_t TaskCount, tmc::ex_any* Executor,
    tmc::ex_any* ContinuationExecutor, size_t Prio, bool DoSymmetricTransfer
  )
      : continuation_executor{ContinuationExecutor} {
    if constexpr (!IsEach) {
      symmetric_task = nullptr;
    }

    // Wrap unknown (WRAPPER) awaitables into work_items (tasks). Preserve the
    // type of known awaitables.
    using Awaitable = std::conditional_t<
      IsFunc, tmc::task<Result>,
      std::remove_cvref_t<std::iter_value_t<TaskIter>>>;

    constexpr auto mode = tmc::detail::get_awaitable_traits<Awaitable>::mode;
    static_assert(
      mode != tmc::detail::UNKNOWN, "This doesn't appear to be an awaitable."
    );

    using WorkItem = std::conditional_t<
      mode == tmc::detail::ASYNC_INITIATE, Awaitable, work_item>;
    using WorkItemArray = std::conditional_t<
      Count == 0, std::vector<WorkItem>, std::array<WorkItem, Count>>;

    size_t size;
    if constexpr (Count != 0) {
      size = Count;
    } else {
      size = TaskCount;
      if constexpr (IsEach) {
        if (size > TMC_PLATFORM_BITS - 1) {
          size = TMC_PLATFORM_BITS - 1;
        }
      }
      if constexpr (!std::is_void_v<Result>) {
        result_arr.resize(size);
      }
    }
    size_t continuationPriority = tmc::detail::this_thread::this_task.prio;

    if constexpr (mode == tmc::detail::ASYNC_INITIATE) {
      // ASYNC_INITIATE types may possibly not be stored in a vector or array
      // (no default/copy constructor), so initiate them individually

      set_done_count(size);
      for (size_t i = 0; i < size; ++i) {
        // TODO this std::move allows silently moving-from pointers and
        // arrays; reimplement those usages with move_iterator instead.
        // This is true for all of the iter moves in this class
        TMC_DISABLE_WARNING_PESSIMIZING_MOVE_BEGIN
        auto t = std::move(*Iter);
        TMC_DISABLE_WARNING_PESSIMIZING_MOVE_END
        prepare_work(t, i, continuationPriority);
        tmc::detail::get_awaitable_traits<Awaitable>::async_initiate(
          std::move(t), Executor, Prio
        );
        ++Iter;
      }
    } else { // mode != ASYNC_INITIATE
      // Batch other types of awaitables into a work_item array/vector
      // and submit them in bulk
      WorkItemArray taskArr;
      if constexpr (Count == 0) {
        taskArr.resize(size);
      }

      // Collect and prepare the tasks
      for (size_t i = 0; i < size; ++i) {
        TMC_DISABLE_WARNING_PESSIMIZING_MOVE_BEGIN
        auto t = tmc::detail::into_known<IsFunc>(std::move(*Iter));
        TMC_DISABLE_WARNING_PESSIMIZING_MOVE_END
        prepare_work(t, i, continuationPriority);
        taskArr[i] = tmc::detail::into_initiate(std::move(t));
        ++Iter;
      }

      // Initiate the tasks
      if (size == 0) {
        set_done_count(0);
        return;
      }
      auto postCount = DoSymmetricTransfer ? size - 1 : size;
      set_done_count(postCount);

      if (DoSymmetricTransfer) {
        symmetric_task = TMC_WORK_ITEM_AS_STD_CORO(taskArr[size - 1]);
      }
      tmc::detail::post_bulk_checked(Executor, taskArr.data(), postCount, Prio);
    }
  }

  template <typename TaskIter>
  inline aw_spawn_many_impl(
    TaskIter Begin, TaskIter End, size_t MaxCount, tmc::ex_any* Executor,
    tmc::ex_any* ContinuationExecutor, size_t Prio, bool DoSymmetricTransfer
  )
    requires(requires(TaskIter a, TaskIter b) {
      ++a;
      *a;
      a != b;
    })
      : continuation_executor{ContinuationExecutor} {
    if constexpr (!IsEach) {
      symmetric_task = nullptr;
    }

    size_t size;
    if constexpr (Count != 0) {
      size = Count;
    } else {
      size = MaxCount;
      if constexpr (IsEach) {
        if (size > TMC_PLATFORM_BITS - 1) {
          size = TMC_PLATFORM_BITS - 1;
        }
      }
      if constexpr (requires(TaskIter a, TaskIter b) { a - b; }) {
        // Caller didn't specify capacity to preallocate, but we can calculate
        size_t iterSize = static_cast<size_t>(End - Begin);
        if (iterSize < size) {
          size = iterSize;
        }
        if constexpr (!std::is_void_v<Result>) {
          result_arr.resize(size);
        }
      }
    }

    // Wrap unknown (WRAPPER) awaitables into work_items (tasks). Preserve the
    // type of known awaitables.

    using Awaitable = std::conditional_t<
      IsFunc, tmc::task<Result>,
      std::remove_cvref_t<std::iter_value_t<TaskIter>>>;

    constexpr auto mode = tmc::detail::get_awaitable_traits<Awaitable>::mode;
    static_assert(
      mode != tmc::detail::UNKNOWN, "This doesn't appear to be an awaitable."
    );

    using WorkItem = std::conditional_t<
      mode == tmc::detail::ASYNC_INITIATE, Awaitable, work_item>;
    using WorkItemArray = std::conditional_t<
      Count == 0, std::vector<WorkItem>, std::array<WorkItem, Count>>;

    size_t continuationPriority = tmc::detail::this_thread::this_task.prio;

    // Collect and prepare the tasks
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
            std::move(t), Executor, Prio
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
          auto t = tmc::detail::into_known<IsFunc>(std::move(*Begin));
          TMC_DISABLE_WARNING_PESSIMIZING_MOVE_END
          prepare_work(t, taskCount, continuationPriority);
          taskArr[taskCount] = tmc::detail::into_initiate(std::move(t));
          ++Begin;
          ++taskCount;
        }

        // Initiate the tasks
        if (taskCount == 0) {
          set_done_count(0);
          return;
        }
        if constexpr (mode == tmc::detail::ASYNC_INITIATE) {
          set_done_count(taskCount);
          for (size_t i = 0; i < taskCount; ++i) {
            tmc::detail::get_awaitable_traits<Awaitable>::async_initiate(
              std::move(taskArr[i]), Executor, Prio
            );
          }
        } else {
          auto postCount = DoSymmetricTransfer ? taskCount - 1 : taskCount;
          set_done_count(postCount);
          if (DoSymmetricTransfer) {
            symmetric_task = TMC_WORK_ITEM_AS_STD_CORO(taskArr[taskCount - 1]);
          }
          tmc::detail::post_bulk_checked(
            Executor, taskArr.data(), postCount, Prio
          );
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
                    mode == tmc::detail::ASYNC_INITIATE ||
                    mode == tmc::detail::WRAPPER) {
        // These types can be processed using a single vector
        WorkItemArray taskArr;
        while (Begin != End && taskCount < size) {
          TMC_DISABLE_WARNING_PESSIMIZING_MOVE_BEGIN
          taskArr.emplace_back(
            tmc::detail::into_initiate(
              tmc::detail::into_known<IsFunc>(std::move(*Begin))
            )
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

        // Initiate the tasks
        if (taskCount == 0) {
          set_done_count(0);
          return;
        }
        if constexpr (mode == tmc::detail::ASYNC_INITIATE) {
          set_done_count(taskCount);
          for (size_t i = 0; i < taskCount; ++i) {
            tmc::detail::get_awaitable_traits<Awaitable>::async_initiate(
              std::move(taskArr[i]), Executor, Prio
            );
          }
        } else {
          auto postCount = DoSymmetricTransfer ? taskCount - 1 : taskCount;
          set_done_count(postCount);
          if (DoSymmetricTransfer) {
            symmetric_task = TMC_WORK_ITEM_AS_STD_CORO(taskArr[taskCount - 1]);
          }
          tmc::detail::post_bulk_checked(
            Executor, taskArr.data(), postCount, Prio
          );
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

        // Initiate the tasks
        auto postCount = DoSymmetricTransfer ? taskCount - 1 : taskCount;
        set_done_count(postCount);
        if (DoSymmetricTransfer) {
          symmetric_task = originalCoroArr[taskCount - 1];
        }

        std::array<tmc::work_item, 64> workItemArr;
        size_t totalCount = 0;
        while (totalCount < postCount) {
          size_t submitCount = 0;
          while (submitCount < workItemArr.size() && totalCount < postCount) {
            workItemArr[submitCount] = tmc::detail::into_initiate(
              std::move(originalCoroArr[totalCount])
            );
            ++totalCount;
            ++submitCount;
          }
          tmc::detail::post_bulk_checked(
            Executor, workItemArr.data(), submitCount, Prio
          );
        }
      }
    }
  }

public:
  /*** SUPPORTS REGULAR AWAIT ***/
  /// Always suspends.
  inline bool await_ready() const noexcept
    requires(!IsEach)
  {
    // Always suspends, due to the possibility to resume on another executor.
    return false;
  }

  /// Suspends the outer coroutine, submits the wrapped task to the
  /// executor, and waits for it to complete.
  inline std::coroutine_handle<>
  await_suspend(std::coroutine_handle<> Outer) noexcept
    requires(!IsEach)
  {
#ifndef NDEBUG
    assert(done_count.load() >= 0 && "You may only co_await this once.");
#endif
    continuation = Outer;
    std::coroutine_handle<> next;
    if (symmetric_task != nullptr) {
      // symmetric transfer to the last task IF it should run immediately
      next = symmetric_task;
    } else {
      // This logic is necessary because we submitted all child tasks before
      // the parent suspended. Allowing parent to be resumed before it
      // suspends would be UB. Therefore we need to block the resumption until
      // here.
      auto remaining = done_count.fetch_sub(1, std::memory_order_acq_rel);
      // No symmetric transfer - all tasks were already posted.
      // Suspend if remaining > 0 (task is still running)
      if (remaining > 0) {
        next = std::noop_coroutine();
      } else { // Resume if remaining <= 0 (tasks already finished)
        if (continuation_executor == nullptr ||
            tmc::detail::this_thread::exec_is(continuation_executor)) {
          next = Outer;
        } else {
          // Need to resume on a different executor
          tmc::detail::post_checked(
            continuation_executor, std::move(Outer),
            tmc::detail::this_thread::this_task.prio
          );
          next = std::noop_coroutine();
        }
      }
    }
    return next;
  }

  /// If `Count` is a compile-time template argument, returns a
  /// `std::array<Result, Count>`. If `Count` is a runtime parameter, returns
  /// a `std::vector<Result>` with capacity `Count`. If `Result` is not
  /// default-constructible, it will be wrapped in an optional.
  TMC_AWAIT_RESUME inline std::add_rvalue_reference_t<ResultArray>
  await_resume() noexcept
    requires(!IsEach && !std::is_void_v<Result>)
  {
    return std::move(result_arr);
  }

  /// Does nothing.
  inline void await_resume() noexcept
    requires(!IsEach && std::is_void_v<Result>)
  {}
  /*** END REGULAR AWAIT ***/

  /*** SUPPORTS EACH() ***/
  /// Suspends if there are no ready results.
  inline bool await_ready() const noexcept
    requires(IsEach)
  {
    return tmc::detail::result_each_await_ready();
  }

  /// Suspends if there are no ready results.
  inline bool await_suspend(std::coroutine_handle<> Outer) noexcept
    requires(IsEach)
  {
    return tmc::detail::result_each_await_suspend(
      remaining_count, Outer, continuation, continuation_executor, sync_flags
    );
  }

  /// Returns the index of the current ready result. The result indexes
  /// correspond to the indexes of the originally submitted tasks. Results may
  /// become ready in any order, but when awaited repeatedly, each index from
  /// `[0..task_count)` will be returned exactly once. When there are no
  /// more results to be returned, the returned index will be equal to
  /// `end()`.
  TMC_AWAIT_RESUME inline size_t await_resume() noexcept
    requires(IsEach)
  {
    return tmc::detail::result_each_await_resume(remaining_count, sync_flags);
  }

  /// Provides a sentinel value that can be compared against the value
  /// returned from co_await.
  inline size_t end() noexcept
    requires(IsEach)
  {
    return 64;
  }

  // Gets the ready result at the given index.
  inline std::add_lvalue_reference_t<tmc::detail::result_storage_t<Result>>
  operator[](size_t idx) noexcept
    requires(IsEach && !std::is_void_v<Result>)
  {
    assert(idx < result_arr.size());
    return result_arr[idx];
  }

  // Provided for convenience only - to expose the same API as the
  // Result-returning awaitable version. Does nothing.
  inline void operator[]([[maybe_unused]] size_t idx) noexcept
    requires(IsEach && std::is_void_v<Result>)
  {}
  /*** END EACH() ***/

  // This must be awaited and all child tasks completed before destruction.
#ifndef NDEBUG
  ~aw_spawn_many_impl() noexcept {
    if constexpr (IsEach) {
      assert(remaining_count == 0 && "You must submit or co_await this.");
    } else {
      assert(done_count.load() < 0 && "You must submit or co_await this.");
    }
  }
#endif

  // Not movable or copyable due to awaitables being initiated in constructor,
  // and having pointers to this.
  aw_spawn_many_impl& operator=(const aw_spawn_many_impl& other) = delete;
  aw_spawn_many_impl(const aw_spawn_many_impl& other) = delete;
  aw_spawn_many_impl& operator=(aw_spawn_many_impl&& other) = delete;
  aw_spawn_many_impl(aw_spawn_many_impl&& other) = delete;
};

template <typename Result, size_t Count, bool IsFunc>
using aw_spawn_many_fork = tmc::detail::rvalue_only_awaitable<
  aw_spawn_many_impl<Result, Count, false, IsFunc>>;

template <typename Result, size_t Count, bool IsFunc>
using aw_spawn_many_each = tmc::detail::lvalue_only_awaitable<
  aw_spawn_many_impl<Result, Count, true, IsFunc>>;

template <
  typename Result, size_t Count, typename IterBegin, typename IterEnd,
  bool IsFunc>
class [[nodiscard("You must await or initiate the result of spawn_many().")]]
aw_spawn_many : public tmc::detail::run_on_mixin<
                  aw_spawn_many<Result, Count, IterBegin, IterEnd, IsFunc>>,
                public tmc::detail::resume_on_mixin<
                  aw_spawn_many<Result, Count, IterBegin, IterEnd, IsFunc>>,
                public tmc::detail::with_priority_mixin<
                  aw_spawn_many<Result, Count, IterBegin, IterEnd, IsFunc>> {
  friend class tmc::detail::run_on_mixin<aw_spawn_many>;
  friend class tmc::detail::resume_on_mixin<aw_spawn_many>;
  friend class tmc::detail::with_priority_mixin<aw_spawn_many>;
  static_assert(sizeof(task<Result>) == sizeof(std::coroutine_handle<>));
  static_assert(alignof(task<Result>) == alignof(std::coroutine_handle<>));

  IterBegin iter;
  IterEnd sentinel;
  size_t maxCount;
  tmc::ex_any* executor;
  tmc::ex_any* continuation_executor;
  size_t prio;
#ifndef NDEBUG
  bool is_empty;
#endif

public:
  /// For use when `TaskCount` is a runtime parameter.
  /// It is recommended to call `spawn_many()` instead of using this
  /// constructor directly.
  aw_spawn_many(IterBegin TaskIterator, IterEnd Sentinel, size_t MaxCount)
      : iter{TaskIterator}, sentinel{Sentinel}, maxCount{MaxCount},
        executor(tmc::detail::this_thread::executor),
        continuation_executor(tmc::detail::this_thread::executor),
        prio(tmc::detail::this_thread::this_task.prio)
#ifndef NDEBUG
        ,
        is_empty(false)
#endif
  {
  }

  aw_spawn_many_impl<Result, Count, false, IsFunc>
  operator co_await() && noexcept {
#ifndef NDEBUG
    assert(!is_empty && "You may only submit or co_await this once.");
    is_empty = true;
#endif
    bool doSymmetricTransfer =
      tmc::detail::this_thread::exec_prio_is(executor, prio);

    using Awaitable = std::conditional_t<
      IsFunc, tmc::task<Result>,
      std::remove_cvref_t<std::iter_value_t<IterBegin>>>;
    constexpr auto mode = tmc::detail::get_awaitable_traits<Awaitable>::mode;
    static_assert(
      mode != tmc::detail::UNKNOWN, "This doesn't appear to be an awaitable."
    );

    if constexpr (mode == tmc::detail::ASYNC_INITIATE) {
      doSymmetricTransfer = false;
    }
    if constexpr (std::is_convertible_v<IterEnd, size_t>) {
      // "Sentinel" is actually a count
      return aw_spawn_many_impl<Result, Count, false, IsFunc>(
        std::move(iter), std::move(sentinel), executor, continuation_executor,
        prio, doSymmetricTransfer
      );
    } else {
      // We have both a sentinel and a MaxCount
      return aw_spawn_many_impl<Result, Count, false, IsFunc>(
        std::move(iter), std::move(sentinel), maxCount, executor,
        continuation_executor, prio, doSymmetricTransfer
      );
    }
  }

  /// Submits the tasks to the executor immediately, without suspending the
  /// current coroutine. You must join the forked tasks by awaiting the returned
  /// awaitable before it goes out of scope.
  [[nodiscard(
    "You must co_await the fork() awaitable before it goes out of scope."
  )]] inline aw_spawn_many_fork<Result, Count, IsFunc>
  fork() && {
#ifndef NDEBUG
    assert(!is_empty && "You may only submit or co_await this once.");
    is_empty = true;
#endif
    if constexpr (std::is_convertible_v<IterEnd, size_t>) {
      // "Sentinel" is actually a count
      return aw_spawn_many_fork<Result, Count, IsFunc>(
        std::move(iter), std::move(sentinel), executor, continuation_executor,
        prio, false
      );
    } else {
      // We have both a sentinel and a MaxCount
      return aw_spawn_many_fork<Result, Count, IsFunc>(
        std::move(iter), std::move(sentinel), maxCount, executor,
        continuation_executor, prio, false
      );
    }
  }

  /// Rather than waiting for all results at once, each result will be made
  /// available immediately as it becomes ready. Each time this is co_awaited,
  /// it will return the index of a single ready result. The result indexes
  /// correspond to the indexes of the originally submitted tasks, and the
  /// values can be accessed using `operator[]`. Results may become ready in any
  /// order, but when awaited repeatedly, each index from `[0..task_count)` will
  /// be returned exactly once. You must await this repeatedly until all tasks
  /// are complete, at which point the index returned will be equal to the
  /// value of `end()`.
  inline aw_spawn_many_each<Result, Count, IsFunc> result_each() && {
#ifndef NDEBUG
    assert(!is_empty && "You may only submit or co_await this once.");
    is_empty = true;
#endif
    if constexpr (std::is_convertible_v<IterEnd, size_t>) {
      // "Sentinel" is actually a count
      return aw_spawn_many_each<Result, Count, IsFunc>(
        std::move(iter), std::move(sentinel), executor, continuation_executor,
        prio, false
      );
    } else {
      // We have both a sentinel and a MaxCount
      return aw_spawn_many_each<Result, Count, IsFunc>(
        std::move(iter), std::move(sentinel), maxCount, executor,
        continuation_executor, prio, false
      );
    }
  }

  /// Submits the tasks to the executor immediately. They cannot be awaited
  /// afterward.
  void detach()
    requires(std::is_void_v<Result>)
  {
#ifndef NDEBUG
    assert(!is_empty && "You may only submit or co_await this once.");
    is_empty = true;
#endif

    using Awaitable = std::conditional_t<
      IsFunc, tmc::task<Result>,
      std::remove_cvref_t<std::iter_value_t<IterBegin>>>;

    constexpr auto mode = tmc::detail::get_awaitable_traits<Awaitable>::mode;
    static_assert(
      mode != tmc::detail::UNKNOWN, "This doesn't appear to be an awaitable."
    );

    if constexpr (mode == tmc::detail::TMC_TASK ||
                  mode == tmc::detail::COROUTINE ||
                  mode == tmc::detail::WRAPPER) {
      using TaskArray = std::conditional_t<
        Count == 0, std::vector<work_item>, std::array<work_item, Count>>;
      TaskArray taskArr;

      if constexpr (std::is_convertible_v<IterEnd, size_t>) {
        // "Sentinel" is actually a count
        if constexpr (Count == 0) {
          taskArr.resize(sentinel);
        }
        const size_t size = taskArr.size();
        for (size_t i = 0; i < size; ++i) {
          TMC_DISABLE_WARNING_PESSIMIZING_MOVE_BEGIN
          taskArr[i] = tmc::detail::into_initiate(
            tmc::detail::into_known<IsFunc>(std::move(*iter))
          );
          TMC_DISABLE_WARNING_PESSIMIZING_MOVE_END
          ++iter;
        }
        tmc::detail::post_bulk_checked(executor, taskArr.data(), size, prio);
      } else {
        if constexpr (Count == 0 &&
                      requires(IterEnd a, IterBegin b) { a - b; }) {
          // Caller didn't specify capacity to preallocate, but we can
          // calculate
          size_t iterSize = static_cast<size_t>(sentinel - iter);
          if (maxCount < iterSize) {
            taskArr.resize(maxCount);
          } else {
            taskArr.resize(iterSize);
          }
        }

        size_t taskCount = 0;
        if constexpr (Count != 0 ||
                      requires(IterEnd a, IterBegin b) { a - b; }) {
          const size_t size = taskArr.size();
          while (iter != sentinel && taskCount < size) {
            TMC_DISABLE_WARNING_PESSIMIZING_MOVE_BEGIN
            taskArr[taskCount] = tmc::detail::into_initiate(
              tmc::detail::into_known<IsFunc>(std::move(*iter))
            );
            TMC_DISABLE_WARNING_PESSIMIZING_MOVE_END
            ++iter;
            ++taskCount;
          }
        } else {
          // We have no idea how many tasks there will be.
          while (iter != sentinel && taskCount < maxCount) {
            TMC_DISABLE_WARNING_PESSIMIZING_MOVE_BEGIN
            taskArr.emplace_back(
              tmc::detail::into_initiate(
                tmc::detail::into_known<IsFunc>(std::move(*iter))
              )
            );
            TMC_DISABLE_WARNING_PESSIMIZING_MOVE_END
            ++iter;
            ++taskCount;
          }
        }
        tmc::detail::post_bulk_checked(
          executor, taskArr.data(), taskCount, prio
        );
      }
    } else { // mode == ASYNC_INITIATE
      if constexpr (std::is_convertible_v<IterEnd, size_t>) {
        // "Sentinel" is actually a count
        size_t size;
        if constexpr (Count != 0) {
          size = Count;
        } else {
          size = sentinel;
        }
        for (size_t i = 0; i < size; ++i) {
          TMC_DISABLE_WARNING_PESSIMIZING_MOVE_BEGIN
          tmc::detail::get_awaitable_traits<Awaitable>::async_initiate(
            std::move(*iter), executor, prio
          );
          TMC_DISABLE_WARNING_PESSIMIZING_MOVE_END
          ++iter;
        }
      } else {
        size_t size;
        if constexpr (Count != 0) {
          size = Count;
        } else {
          size = maxCount;
        }
        size_t taskCount = 0;
        while (iter != sentinel && taskCount < size) {
          TMC_DISABLE_WARNING_PESSIMIZING_MOVE_BEGIN
          tmc::detail::get_awaitable_traits<Awaitable>::async_initiate(
            std::move(*iter), executor, prio
          );
          TMC_DISABLE_WARNING_PESSIMIZING_MOVE_END
          ++iter;
          ++taskCount;
        }
      }
    }
  }

#ifndef NDEBUG
  ~aw_spawn_many() noexcept {
    // This must be used, moved-from, or submitted for execution
    // in some way before destruction.
    assert(is_empty && "You must submit or co_await this.");
  }
#endif
  aw_spawn_many(const aw_spawn_many&) = delete;
  aw_spawn_many& operator=(const aw_spawn_many&) = delete;
  aw_spawn_many(aw_spawn_many&& Other)
      : iter(std::move(Other.iter)), sentinel(std::move(Other.sentinel)),
        maxCount(std::move(Other.maxCount)),
        executor(std::move(Other.executor)),
        continuation_executor(std::move(Other.continuation_executor)),
        prio(std::move(Other.prio)) {
#ifndef NDEBUG
    is_empty = Other.is_empty;
    Other.is_empty = true;
#endif
  }

  aw_spawn_many& operator=(aw_spawn_many&& Other) {
    iter = std::move(Other.iter);
    sentinel = std::move(Other.sentinel);
    maxCount = std::move(Other.maxCount);
    executor = std::move(Other.executor);
    continuation_executor = std::move(Other.continuation_executor);
    prio = std::move(Other.prio);
#ifndef NDEBUG
    is_empty = Other.is_empty;
    Other.is_empty = true;
#endif
    return *this;
  }
};

namespace detail {
template <
  typename Result, size_t Count, typename IterBegin, typename IterEnd,
  bool IsFunc>
struct awaitable_traits<
  aw_spawn_many<Result, Count, IterBegin, IterEnd, IsFunc>> {
  static constexpr configure_mode mode = WRAPPER;
  using result_type = std::conditional_t<
    std::is_void_v<Result>, void,
    std::conditional_t<
      Count == 0, std::vector<tmc::detail::result_storage_t<Result>>,
      std::array<tmc::detail::result_storage_t<Result>, Count>>>;
  using self_type = aw_spawn_many<Result, Count, IterBegin, IterEnd, IsFunc>;
  using awaiter_type = aw_spawn_many_impl<Result, Count, false, IsFunc>;

  static awaiter_type get_awaiter(self_type&& Awaitable) noexcept {
    return std::forward<self_type>(Awaitable).operator co_await();
  }
};
} // namespace detail
} // namespace tmc
