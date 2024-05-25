// Copyright (c) 2023-2024 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/concepts.hpp" // IWYU pragma: keep
#include "tmc/detail/mixins.hpp"
#include "tmc/detail/thread_locals.hpp"
#include "tmc/task.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <coroutine>
#include <iterator>
#include <type_traits>
#include <vector>

namespace tmc {
/// The customizable task wrapper / awaitable type returned by
/// `tmc::spawn_many(tmc::task<Result>)`.
template <typename Result, size_t Count, typename IterBegin, typename IterEnd>
class aw_task_many;

/// For use when the number of items to spawn is known at compile time.
/// `Count` must be non-zero.
/// `TaskIter` must be an iterator type that implements `operator*()` and
/// `TaskIter& operator++()`.
///
/// Submits items in range [Begin, Begin + Count) to the executor.
template <
  size_t Count, typename TaskIter, typename Task = std::iter_value_t<TaskIter>,
  typename Result = Task::result_type>
aw_task_many<Result, Count, TaskIter, size_t> spawn_many(TaskIter&& Begin)
  requires(detail::is_task_result_v<Task, Result>)
{
  static_assert(Count != 0);
  return aw_task_many<Result, Count, TaskIter, size_t>(
    std::forward<TaskIter>(Begin), 0, 0
  );
}

/// For use when the number of items to spawn is a runtime parameter.
/// `TaskIter` must be an iterator type that implements `operator*()` and
/// `TaskIter& operator++()`.
/// `TaskCount` must be non-zero.
///
/// Submits items in range [Begin, Begin + TaskCount) to the executor.
template <
  typename TaskIter, typename Task = std::iter_value_t<TaskIter>,
  typename Result = Task::result_type>
aw_task_many<Result, 0, TaskIter, size_t>
spawn_many(TaskIter&& Begin, size_t TaskCount)
  requires(detail::is_task_result_v<Task, Result>)
{
  return aw_task_many<Result, 0, TaskIter, size_t>(
    std::forward<TaskIter>(Begin), TaskCount, 0
  );
}

/// For use when the number of items to spawn may be variable.
/// `TaskIter` must be an iterator type that implements `operator*()` and
/// `TaskIter& operator++()`.
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
template <
  size_t MaxCount = 0, typename TaskIter,
  typename Task = std::iter_value_t<TaskIter>,
  typename Result = Task::result_type>
aw_task_many<Result, MaxCount, TaskIter, TaskIter>
spawn_many(TaskIter&& Begin, TaskIter&& End)
  requires(
    detail::is_task_result_v<Task, Result> &&
    requires(TaskIter a, TaskIter b) { // TODO write is_iterator_v
      ++a;
      *a;
      a != b;
    }
  )
{
  return aw_task_many<Result, MaxCount, TaskIter, TaskIter>(
    std::forward<TaskIter>(Begin), std::forward<TaskIter>(End),
    std::numeric_limits<size_t>::max()
  );
}

/// For use when the number of items to spawn may be variable.
/// `TaskIter` must be an iterator type that implements `operator*()` and
/// `TaskIter& operator++()`.
///
/// - Up to `MaxCount` tasks will be consumed from the iterator.
/// - The iterator may produce less than `MaxCount` tasks.
/// - The return type will be a right-sized `std::vector<Result>` with size and
/// capacity equal to the number of tasks consumed from the iterator.
///
/// Submits items in range [Begin, min(Begin + MaxCount, End)) to the executor.
template <
  typename TaskIter, typename Task = std::iter_value_t<TaskIter>,
  typename Result = Task::result_type>
aw_task_many<Result, 0, TaskIter, TaskIter>
spawn_many(TaskIter&& Begin, TaskIter&& End, size_t MaxCount)
  requires(detail::is_task_result_v<Task, Result>)
{
  return aw_task_many<Result, 0, TaskIter, TaskIter>(
    std::forward<TaskIter>(Begin), std::forward<TaskIter>(End), MaxCount
  );
}

/// For use when the number of items to spawn is known at compile time.
/// `Count` must be non-zero.
/// `Functor` must be a copyable type that implements `Result operator()`.
/// `FuncIter` must be an iterator type that implements `operator*()` and
/// `FuncIter& operator++()`.
///
/// Submits items in range [Begin, Begin + Count) to the executor.
template <
  size_t Count, typename FuncIter,
  typename Functor = std::iter_value_t<FuncIter>,
  typename Result = std::invoke_result_t<Functor>>
aw_task_many<Result, Count, FuncIter, size_t>
spawn_many(FuncIter&& FunctorIterator)
  requires(detail::is_func_result_v<Functor, Result>)
{
  static_assert(Count != 0);
  return aw_task_many<Result, Count, FuncIter, size_t>(
    std::forward<FuncIter>(FunctorIterator), 0, 0
  );
}

/// For use when the number of items to spawn is a runtime parameter.
/// `Functor` must be a copyable type that implements `Result operator()`.
/// `FuncIter` must be an iterator type that implements `operator*()` and
/// `FuncIter& operator++()`.
/// `FunctorCount` must be non-zero.
///
/// Submits items in range [Begin, Begin + FunctorCount) to the executor.
template <
  typename FuncIter, typename Functor = std::iter_value_t<FuncIter>,
  typename Result = std::invoke_result_t<Functor>>
aw_task_many<Result, 0, FuncIter, size_t>
spawn_many(FuncIter&& FunctorIterator, size_t FunctorCount)
  requires(detail::is_func_result_v<Functor, Result>)
{
  return aw_task_many<Result, 0, FuncIter, size_t>(
    std::forward<FuncIter>(FunctorIterator), FunctorCount, 0
  );
}

/// For use when the number of items to spawn may be variable.
/// `Functor` must be a copyable type that implements `Result operator()`.
/// `FuncIter` must be an iterator type that implements `operator*()` and
/// `FuncIter& operator++()`.
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
template <
  size_t MaxCount = 0, typename FuncIter,
  typename Functor = std::iter_value_t<FuncIter>,
  typename Result = std::invoke_result_t<Functor>>
aw_task_many<Result, MaxCount, FuncIter, FuncIter>
spawn_many(FuncIter&& Begin, FuncIter&& End)
  requires(detail::is_func_result_v<Functor, Result>)
{
  return aw_task_many<Result, MaxCount, FuncIter, FuncIter>(
    std::forward<FuncIter>(Begin), std::forward<FuncIter>(End),
    std::numeric_limits<size_t>::max()
  );
}

/// For use when the number of items to spawn may be variable.
/// `Functor` must be a copyable type that implements `Result operator()`.
/// `FuncIter` must be an iterator type that implements `operator*()` and
/// `FuncIter& operator++()`.
///
/// - Up to `MaxCount` tasks will be consumed from the iterator.
/// - The iterator may produce less than `MaxCount` tasks.
/// - The return type will be a right-sized `std::vector<Result>` with size and
/// capacity equal to the number of tasks consumed from the iterator.
///
/// Submits items in range [Begin, min(Begin + MaxCount, End)) to the executor.
template <
  typename FuncIter, typename Functor = std::iter_value_t<FuncIter>,
  typename Result = std::invoke_result_t<Functor>>
aw_task_many<Result, 0, FuncIter, FuncIter>
spawn_many(FuncIter&& Begin, FuncIter&& End, size_t MaxCount)
  requires(detail::is_func_result_v<Functor, Result>)
{
  return aw_task_many<Result, 0, FuncIter, FuncIter>(
    std::forward<FuncIter>(Begin), std::forward<FuncIter>(End), MaxCount
  );
}

template <typename Result, size_t Count, typename IterBegin, typename IterEnd>
class aw_task_many_impl {
public:
  // From aw_task_many
  IterBegin iter;
  IterEnd sentinel;
  size_t maxCount;
  detail::type_erased_executor* executor;
  detail::type_erased_executor* continuation_executor;
  size_t prio;
  bool run_early;

  // Needed to suspend
  using TaskArray = std::conditional_t<
    Count == 0, std::vector<work_item>, std::array<work_item, Count>>;
  using ResultArray = std::conditional_t<
    Count == 0, std::vector<Result>, std::array<Result, Count>>;
  std::coroutine_handle<> continuation;
  std::atomic<int64_t> done_count;
  ResultArray result_arr;

  template <typename, size_t, typename, typename> friend class aw_task_many;

  aw_task_many_impl(
    IterBegin TaskIterator, IterEnd Sentinel, size_t MaxCount,
    detail::type_erased_executor* Executor,
    detail::type_erased_executor* ContinuationExecutor, size_t Prio,
    bool RunEarly
  )
      : iter{TaskIterator}, sentinel{Sentinel}, maxCount{MaxCount},
        executor(Executor), continuation_executor(ContinuationExecutor),
        prio(Prio), run_early(RunEarly) {}

public:
  /// Always suspends.
  inline bool await_ready() const noexcept { return false; }

  /// Suspends the outer coroutine, submits the wrapped task to the
  /// executor, and waits for it to complete.
  TMC_FORCE_INLINE inline std::coroutine_handle<>
  await_suspend(std::coroutine_handle<> Outer) noexcept {
    continuation = Outer;
    if constexpr (std::is_convertible_v<IterEnd, size_t>) {
      // "Sentinel" is actually a count
      const size_t TaskCount = sentinel;
      TaskArray taskArr;
      if constexpr (Count == 0) {
        taskArr.resize(TaskCount);
        result_arr.resize(TaskCount);
      }
      const size_t size = taskArr.size();
      if (size == 0) {
        return Outer;
      }
      size_t i = 0;
      for (; i < size; ++i) {
        // TODO this std::move allows silently moving-from pointers and arrays
        // reimplement those usages with move_iterator instead
        // TODO if the original iterator is a vector, why create another here?
        detail::unsafe_task<Result> t(detail::into_task(std::move(*iter)));
        auto& p = t.promise();
        p.continuation = &continuation;
        p.continuation_executor = &continuation_executor;
        p.done_count = &done_count;
        p.result_ptr = &result_arr[i];
        taskArr[i] = t;
        ++iter;
      }
      bool doSymmetricTransfer = detail::this_thread::exec_is(executor) &&
                                 detail::this_thread::prio_is(prio);
      auto postCount = doSymmetricTransfer ? size - 1 : size;
      done_count.store(
        static_cast<int64_t>(postCount), std::memory_order_release
      );

      if (postCount != 0) {
        detail::post_bulk_checked(executor, taskArr.data(), postCount, prio);
      }
      if (doSymmetricTransfer) {
        return std::coroutine_handle<>::from_address(
          TMC_WORK_ITEM_AS_STD_CORO(taskArr[i - 1]).address()
        );
      }
    } else {
      // We have both a sentinel and a MaxCount
      TaskArray taskArr;
      if constexpr (Count == 0 && requires(IterEnd e, IterBegin b) { e - b; }) {
        // Caller didn't specify capacity to preallocate, but we can calculate
        size_t iterSize = static_cast<size_t>(sentinel - iter);
        if (maxCount < iterSize) {
          taskArr.resize(maxCount);
          result_arr.resize(maxCount);
        } else {
          taskArr.resize(iterSize);
          result_arr.resize(iterSize);
        }
      }

      size_t taskCount = 0;
      if constexpr (Count != 0 || requires(IterEnd e, IterBegin b) { e - b; }) {
        // Iterator could produce less than Count tasks, so count them.
        // Iterator could produce more than Count tasks - stop after taking
        // Count.
        const size_t size = taskArr.size();
        while (iter != sentinel) {
          if (taskCount == size) {
            break;
          }
          // TODO this std::move allows silently moving-from pointers and arrays
          // reimplement those usages with move_iterator instead
          // TODO if the original iterator is a vector, why create another here?
          detail::unsafe_task<Result> t(detail::into_task(std::move(*iter)));
          auto& p = t.promise();
          p.continuation = &continuation;
          p.continuation_executor = &continuation_executor;
          p.done_count = &done_count;
          p.result_ptr = &result_arr[taskCount];
          taskArr[taskCount] = t;
          ++iter;
          ++taskCount;
        }
        if (taskCount == 0) {
          return Outer;
        }
      } else {
        // We have no idea how many tasks there will be.
        while (iter != sentinel) {
          if (taskCount == maxCount) {
            break;
          }
          // TODO this std::move allows silently moving-from pointers and arrays
          // reimplement those usages with move_iterator instead
          // TODO if the original iterator is a vector, why create another here?
          detail::unsafe_task<Result> t(detail::into_task(std::move(*iter)));
          auto& p = t.promise();
          p.continuation = &continuation;
          p.continuation_executor = &continuation_executor;
          p.done_count = &done_count;
          taskArr.push_back(t);
          ++iter;
          ++taskCount;
        }
        if (taskCount == 0) {
          return Outer;
        }
        // We couldn't bind result_ptr before we determined how many tasks there
        // are, because reallocation would invalidate those pointers. Now bind
        // them.
        result_arr.resize(taskCount);
        for (size_t i = 0; i < taskCount; ++i) {
          auto t = detail::unsafe_task<Result>::from_address(
            TMC_WORK_ITEM_AS_STD_CORO(taskArr[i]).address()
          );
          t.promise().result_ptr = &result_arr[i];
        }
      }

      bool doSymmetricTransfer = detail::this_thread::exec_is(executor) &&
                                 detail::this_thread::prio_is(prio);

      auto postCount = doSymmetricTransfer ? taskCount - 1 : taskCount;
      done_count.store(
        static_cast<int64_t>(postCount), std::memory_order_release
      );

      if (postCount != 0) {
        detail::post_bulk_checked(executor, taskArr.data(), postCount, prio);
      }
      if (doSymmetricTransfer) {
        return std::coroutine_handle<>::from_address(
          TMC_WORK_ITEM_AS_STD_CORO(taskArr[taskCount - 1]).address()
        );
      }
    }
    if (run_early) {
      // This logic is necessary because we submitted all child tasks before the
      // parent suspended. Allowing parent to be resumed before it suspends
      // would be UB. Therefore we need to block the resumption until here.
      std::coroutine_handle<> next;
      auto remaining = done_count.fetch_sub(1, std::memory_order_acq_rel);
      // No symmetric transfer - all tasks were already posted.
      // Suspend if remaining > 0 (task is still running)
      if (remaining > 0) {
        next = std::noop_coroutine();
      } else { // Resume if remaining <= 0 (tasks already finished)
        if (continuation_executor == nullptr ||
            detail::this_thread::exec_is(continuation_executor)) {
          next = Outer;
        } else {
          // Need to resume on a different executor
          detail::post_checked(
            continuation_executor, std::move(Outer),
            detail::this_thread::this_task.prio
          );
          next = std::noop_coroutine();
        }
      }
      return next;
    }
    return std::noop_coroutine();
  }

  /// If `Count` is a compile-time template argument, returns a
  /// `std::array<Result, Count>`. If `Count` is a runtime parameter, returns
  /// a `std::vector<Result>` with capacity `Count`.
  inline ResultArray&& await_resume() noexcept { return std::move(result_arr); }
};

template <size_t Count, typename IterBegin, typename IterEnd>
class aw_task_many_impl<void, Count, IterBegin, IterEnd> {
public:
  // From aw_task_many
  IterBegin iter;
  IterEnd sentinel;
  size_t maxCount;
  detail::type_erased_executor* executor;
  detail::type_erased_executor* continuation_executor;
  size_t prio;
  bool run_early;

  // Needed to suspend
  using TaskArray = std::conditional_t<
    Count == 0, std::vector<work_item>, std::array<work_item, Count>>;
  std::coroutine_handle<> continuation;
  std::atomic<int64_t> done_count;

  template <typename, size_t, typename, typename> friend class aw_task_many;

  aw_task_many_impl(
    IterBegin TaskIterator, IterEnd Sentinel, size_t MaxCount,
    detail::type_erased_executor* Executor,
    detail::type_erased_executor* ContinuationExecutor, size_t Prio,
    bool RunEarly
  )
      : iter{TaskIterator}, sentinel{Sentinel}, maxCount{MaxCount},
        executor(Executor), continuation_executor(ContinuationExecutor),
        prio(Prio), run_early(RunEarly) {}

public:
  /// Always suspends.
  inline bool await_ready() const noexcept { return false; }

  /// Suspends the outer coroutine, submits the wrapped task to the
  /// executor, and waits for it to complete.
  TMC_FORCE_INLINE inline std::coroutine_handle<>
  await_suspend(std::coroutine_handle<> Outer) noexcept {
    continuation = Outer;
    if constexpr (std::is_convertible_v<IterEnd, size_t>) {
      // "Sentinel" is actually a count
      const size_t TaskCount = sentinel;
      TaskArray taskArr;
      if constexpr (Count == 0) {
        taskArr.resize(TaskCount);
      }
      const size_t size = taskArr.size();
      if (size == 0) {
        return Outer;
      }
      size_t i = 0;
      for (; i < size; ++i) {
        // TODO this std::move allows silently moving-from pointers and arrays
        // reimplement those usages with move_iterator instead
        // TODO if the original iterator is a vector, why create another here?
        detail::unsafe_task<void> t(detail::into_task(std::move(*iter)));
        auto& p = t.promise();
        p.continuation = &continuation;
        p.continuation_executor = &continuation_executor;
        p.done_count = &done_count;
        taskArr[i] = t;
        ++iter;
      }
      bool doSymmetricTransfer = detail::this_thread::exec_is(executor) &&
                                 detail::this_thread::prio_is(prio);
      auto postCount = doSymmetricTransfer ? size - 1 : size;
      done_count.store(
        static_cast<int64_t>(postCount), std::memory_order_release
      );

      if (postCount != 0) {
        detail::post_bulk_checked(executor, taskArr.data(), postCount, prio);
      }
      if (doSymmetricTransfer) {
        return std::coroutine_handle<>::from_address(
          TMC_WORK_ITEM_AS_STD_CORO(taskArr[i - 1]).address()
        );
      }
    } else {
      // We have both a sentinel and a MaxCount
      TaskArray taskArr;
      if constexpr (Count == 0 && requires(IterEnd e, IterBegin b) { e - b; }) {
        // Caller didn't specify capacity to preallocate, but we can calculate
        size_t iterSize = static_cast<size_t>(sentinel - iter);
        if (maxCount < iterSize) {
          taskArr.resize(maxCount);
        } else {
          taskArr.resize(iterSize);
        }
      }

      size_t taskCount = 0;
      if constexpr (Count != 0 || requires(IterEnd e, IterBegin b) { e - b; }) {
        // Iterator could produce less than Count tasks, so count them.
        // Iterator could produce more than Count tasks - stop after taking
        // Count.
        const size_t size = taskArr.size();
        while (iter != sentinel) {
          if (taskCount == size) {
            break;
          }
          // TODO this std::move allows silently moving-from pointers and arrays
          // reimplement those usages with move_iterator instead
          // TODO if the original iterator is a vector, why create another here?
          detail::unsafe_task<void> t(detail::into_task(std::move(*iter)));
          auto& p = t.promise();
          p.continuation = &continuation;
          p.continuation_executor = &continuation_executor;
          p.done_count = &done_count;
          taskArr[taskCount] = t;
          ++iter;
          ++taskCount;
        }
        if (taskCount == 0) {
          return Outer;
        }
      } else {
        // We have no idea how many tasks there will be.
        while (iter != sentinel) {
          if (taskCount == maxCount) {
            break;
          }
          // TODO this std::move allows silently moving-from pointers and arrays
          // reimplement those usages with move_iterator instead
          // TODO if the original iterator is a vector, why create another here?
          detail::unsafe_task<void> t(detail::into_task(std::move(*iter)));
          auto& p = t.promise();
          p.continuation = &continuation;
          p.continuation_executor = &continuation_executor;
          p.done_count = &done_count;
          taskArr.push_back(t);
          ++iter;
          ++taskCount;
        }
        if (taskCount == 0) {
          return Outer;
        }
      }

      bool doSymmetricTransfer = detail::this_thread::exec_is(executor) &&
                                 detail::this_thread::prio_is(prio);

      auto postCount = doSymmetricTransfer ? taskCount - 1 : taskCount;
      done_count.store(
        static_cast<int64_t>(postCount), std::memory_order_release
      );

      if (postCount != 0) {
        detail::post_bulk_checked(executor, taskArr.data(), postCount, prio);
      }
      if (doSymmetricTransfer) {
        return std::coroutine_handle<>::from_address(
          TMC_WORK_ITEM_AS_STD_CORO(taskArr[taskCount - 1]).address()
        );
      }
    }
    if (run_early) {
      // This logic is necessary because we submitted all child tasks before the
      // parent suspended. Allowing parent to be resumed before it suspends
      // would be UB. Therefore we need to block the resumption until here.
      std::coroutine_handle<> next;
      auto remaining = done_count.fetch_sub(1, std::memory_order_acq_rel);
      // No symmetric transfer - all tasks were already posted.
      // Suspend if remaining > 0 (task is still running)
      if (remaining > 0) {
        next = std::noop_coroutine();
      } else { // Resume if remaining <= 0 (tasks already finished)
        if (continuation_executor == nullptr ||
            detail::this_thread::exec_is(continuation_executor)) {
          next = Outer;
        } else {
          // Need to resume on a different executor
          detail::post_checked(
            continuation_executor, std::move(Outer),
            detail::this_thread::this_task.prio
          );
          next = std::noop_coroutine();
        }
      }
      return next;
    }
    return std::noop_coroutine();
  }

  /// Does nothing.
  inline void await_resume() noexcept {}
};

template <typename Result, size_t Count, typename IterBegin, typename IterEnd>
using aw_task_many_run_early = detail::rvalue_only_awaitable<
  aw_task_many_impl<Result, Count, IterBegin, IterEnd>>;

// Primary template is forward-declared in "tmc/detail/aw_run_early.hpp".
template <typename Result, size_t Count, typename IterBegin, typename IterEnd>
class [[nodiscard(
  "You must use the aw_task_many<Result> by one of: 1. co_await 2. run_early()"
)]] aw_task_many
    : public detail::run_on_mixin<
        aw_task_many<Result, Count, IterBegin, IterEnd>>,
      public detail::resume_on_mixin<
        aw_task_many<Result, Count, IterBegin, IterEnd>>,
      public detail::with_priority_mixin<
        aw_task_many<Result, Count, IterBegin, IterEnd>> {
  friend class detail::run_on_mixin<aw_task_many>;
  friend class detail::resume_on_mixin<aw_task_many>;
  friend class detail::with_priority_mixin<aw_task_many>;
  static_assert(sizeof(task<Result>) == sizeof(std::coroutine_handle<>));
  static_assert(alignof(task<Result>) == alignof(std::coroutine_handle<>));

protected:
  IterBegin iter;
  IterEnd sentinel;
  size_t maxCount;
  detail::type_erased_executor* executor;
  detail::type_erased_executor* continuation_executor;
  size_t prio;
#ifndef NDEBUG
  bool did_await;
#endif

public:
  /// For use when `TaskCount` is a runtime parameter.
  /// It is recommended to call `spawn_many()` instead of using this constructor
  /// directly.
  aw_task_many(IterBegin TaskIterator, IterEnd Sentinel, size_t MaxCount)
      : iter{TaskIterator}, sentinel{Sentinel}, maxCount{MaxCount},
        executor(detail::this_thread::executor),
        continuation_executor(detail::this_thread::executor),
        prio(detail::this_thread::this_task.prio)
#ifndef NDEBUG
        ,
        did_await(false)
#endif
  {
  }

  aw_task_many_impl<Result, Count, IterBegin, IterEnd> operator co_await() && {
#ifndef NDEBUG
    assert(!did_await);
    did_await = true;
#endif
    return aw_task_many_impl<Result, Count, IterBegin, IterEnd>(
      std::move(iter), std::move(sentinel), maxCount, executor,
      continuation_executor, prio, false
    );
  }

  /// Submit the tasks to the executor immediately. They cannot be awaited
  /// afterward.
  void detach()
    requires(std::is_void_v<Result>)
  {
#ifndef NDEBUG
    assert(!did_await);
    did_await = true;
#endif
    using TaskArray = std::conditional_t<
      Count == 0, std::vector<work_item>, std::array<work_item, Count>>;
    TaskArray taskArr;

    if constexpr (std::is_convertible_v<IterEnd, size_t>) {
      // "Sentinel" is actually a count
      if constexpr (Count == 0) {
        taskArr.resize(sentinel);
      }
      const size_t size = taskArr.size();
      if (size == 0) {
        return;
      }
      for (size_t i = 0; i < size; ++i) {
        // TODO this std::move allows silently moving-from pointers and arrays
        // reimplement those usages with move_iterator instead
        taskArr[i] = detail::into_task(std::move(*iter));
        ++iter;
      }
      detail::post_bulk_checked(executor, taskArr.data(), size, prio);
    } else {
      if constexpr (Count == 0 && requires(IterEnd a, IterBegin b) { a - b; }) {
        // Caller didn't specify capacity to preallocate, but we can calculate
        size_t iterSize = static_cast<size_t>(sentinel - iter);
        if (maxCount < iterSize) {
          taskArr.resize(maxCount);
        } else {
          taskArr.resize(iterSize);
        }
      }

      size_t taskCount = 0;
      if constexpr (Count != 0 || requires(IterEnd a, IterBegin b) { a - b; }) {
        const size_t size = taskArr.size();
        while (iter != sentinel) {
          if (taskCount == size) {
            break;
          }
          // TODO this std::move allows silently moving-from pointers and arrays
          // reimplement those usages with move_iterator instead
          taskArr[taskCount] = detail::into_task(std::move(*iter));
          ++iter;
          ++taskCount;
        }
      } else {
        // We have no idea how many tasks there will be.
        while (iter != sentinel) {
          if (taskCount == maxCount) {
            break;
          }
          // TODO this std::move allows silently moving-from pointers and arrays
          // reimplement those usages with move_iterator instead
          taskArr.emplace_back(detail::into_task(std::move(*iter)));
          ++iter;
          ++taskCount;
        }
      }

      if (taskCount != 0) {
        detail::post_bulk_checked(executor, taskArr.data(), taskCount, prio);
      }
    }
  }

#ifndef NDEBUG
  ~aw_task_many() noexcept { assert(did_await); }
#endif
  aw_task_many(const aw_task_many&) = delete;
  aw_task_many& operator=(const aw_task_many&) = delete;
  aw_task_many(aw_task_many&& Other) = delete;
  //       : iter(std::move(Other.iter)),
  //         sentinelOrCount(std::move(Other.sentinelOrCount)),
  //         executor(std::move(Other.executor)),
  //         continuation_executor(std::move(Other.continuation_executor)),
  //         prio(std::move(Other.prio)) {
  // #ifndef NDEBUG
  //     did_await = Other.did_await;
  //     Other.did_await = true; // prevent other from posting
  // #endif
  //   }

  aw_task_many& operator=(aw_task_many&& Other) = delete;
  //    {
  //     iter = std::move(Other.iter);
  //     sentinelOrCount = std::move(Other.sentinelOrCount);
  //     executor = std::move(Other.executor);
  //     continuation_executor = std::move(Other.continuation_executor);
  //     prio = std::move(Other.prio);
  // #ifndef NDEBUG
  //     did_await = Other.did_await;
  //     Other.did_await = true; // prevent other from posting
  // #endif
  //     return *this;
  //   }

  /// Submits the wrapped tasks immediately, without suspending the current
  /// coroutine. You must await the return type before destroying it.
  inline aw_task_many_run_early<Result, Count, IterBegin, IterEnd>
  run_early() && {
#ifndef NDEBUG
    assert(!did_await);
    did_await = true;
#endif
    return aw_task_many_run_early<Result, Count, IterBegin, IterEnd>(
      std::move(iter), sentinel, executor, continuation_executor, prio, true
    );
  }
};
} // namespace tmc
