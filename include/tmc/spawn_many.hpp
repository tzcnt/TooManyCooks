// Copyright (c) 2023-2024 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/concepts.hpp" // IWYU pragma: keep
#include "tmc/detail/mixins.hpp"
#include "tmc/detail/spawn_many_each.hpp"
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
  requires(tmc::detail::is_task_result_v<Task, Result>)
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
  requires(tmc::detail::is_task_result_v<Task, Result>)
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
  requires(tmc::detail::is_task_result_v<Task, Result>)
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
  requires(tmc::detail::is_task_result_v<Task, Result>)
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
  requires(tmc::detail::is_func_result_v<Functor, Result>)
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
  requires(tmc::detail::is_func_result_v<Functor, Result>)
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
  requires(tmc::detail::is_func_result_v<Functor, Result>)
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
  requires(tmc::detail::is_func_result_v<Functor, Result>)
{
  return aw_task_many<Result, 0, FuncIter, FuncIter>(
    std::forward<FuncIter>(Begin), std::forward<FuncIter>(End), MaxCount
  );
}

template <typename Result, size_t Count> class aw_task_many_impl {
public:
  std::coroutine_handle<> symmetric_task;
  std::coroutine_handle<> continuation;
  tmc::detail::type_erased_executor* continuation_executor;
  using TaskArray = std::conditional_t<
    Count == 0, std::vector<work_item>, std::array<work_item, Count>>;
  using ResultArray = std::conditional_t<
    Count == 0, std::vector<Result>, std::array<Result, Count>>;
  std::atomic<int64_t> done_count;
  ResultArray result_arr;

  template <typename, size_t, typename, typename> friend class aw_task_many;

  template <typename TaskIter>
  inline aw_task_many_impl(
    TaskIter Iter, size_t TaskCount,
    tmc::detail::type_erased_executor* Executor,
    tmc::detail::type_erased_executor* ContinuationExecutor, size_t Prio,
    bool DoSymmetricTransfer
  )
      : symmetric_task{nullptr}, continuation_executor{ContinuationExecutor},
        done_count{0} {
    TaskArray taskArr;
    if constexpr (Count == 0) {
      taskArr.resize(TaskCount);
      result_arr.resize(TaskCount);
    }
    const size_t size = taskArr.size();
    if (size == 0) {
      return;
    }
    size_t i = 0;
    for (; i < size; ++i) {
      // TODO this std::move allows silently moving-from pointers and arrays
      // reimplement those usages with move_iterator instead
      // TODO if the original iterator is a vector, why create another here?
      tmc::detail::unsafe_task<Result> t(tmc::detail::into_task(std::move(*Iter)
      ));
      tmc::detail::set_continuation(t, &continuation);
      tmc::detail::set_continuation_executor(t, &continuation_executor);
      tmc::detail::set_done_count(t, &done_count);
      tmc::detail::set_result_ptr(t, &result_arr[i]);
      taskArr[i] = t;
      ++Iter;
    }
    if (DoSymmetricTransfer) {
      symmetric_task = TMC_WORK_ITEM_AS_STD_CORO(taskArr[i - 1]);
    }
    auto postCount = DoSymmetricTransfer ? size - 1 : size;
    done_count.store(
      static_cast<int64_t>(postCount), std::memory_order_release
    );

    if (postCount != 0) {
      tmc::detail::post_bulk_checked(Executor, taskArr.data(), postCount, Prio);
    }
  }

  template <typename TaskIter>
  inline aw_task_many_impl(
    TaskIter Begin, TaskIter End, size_t MaxCount,
    tmc::detail::type_erased_executor* Executor,
    tmc::detail::type_erased_executor* ContinuationExecutor, size_t Prio,
    bool DoSymmetricTransfer
  )
    requires(requires(TaskIter a, TaskIter b) {
              ++a;
              *a;
              a != b;
            })
      : symmetric_task{nullptr}, continuation_executor{ContinuationExecutor},
        done_count{0} {
    TaskArray taskArr;
    if constexpr (Count == 0 && requires(TaskIter a, TaskIter b) { a - b; }) {
      // Caller didn't specify capacity to preallocate, but we can calculate
      size_t iterSize = static_cast<size_t>(End - Begin);
      if (MaxCount < iterSize) {
        taskArr.resize(MaxCount);
        result_arr.resize(MaxCount);
      } else {
        taskArr.resize(iterSize);
        result_arr.resize(iterSize);
      }
    }

    size_t taskCount = 0;
    if constexpr (Count != 0 || requires(TaskIter a, TaskIter b) { a - b; }) {
      // Iterator could produce less than Count tasks, so count them.
      // Iterator could produce more than Count tasks - stop after taking Count.
      const size_t size = taskArr.size();
      while (Begin != End) {
        if (taskCount == size) {
          break;
        }
        // TODO this std::move allows silently moving-from pointers and arrays
        // reimplement those usages with move_iterator instead
        // TODO if the original iterator is a vector, why create another here?
        tmc::detail::unsafe_task<Result> t(
          tmc::detail::into_task(std::move(*Begin))
        );
        tmc::detail::set_continuation(t, &continuation);
        tmc::detail::set_continuation_executor(t, &continuation_executor);
        tmc::detail::set_done_count(t, &done_count);
        tmc::detail::set_result_ptr(t, &result_arr[taskCount]);
        taskArr[taskCount] = t;
        ++Begin;
        ++taskCount;
      }
      if (taskCount == 0) {
        return;
      }
    } else {
      // We have no idea how many tasks there will be.
      while (Begin != End) {
        if (taskCount == MaxCount) {
          break;
        }
        // TODO this std::move allows silently moving-from pointers and arrays
        // reimplement those usages with move_iterator instead
        // TODO if the original iterator is a vector, why create another here?
        tmc::detail::unsafe_task<Result> t(
          tmc::detail::into_task(std::move(*Begin))
        );
        tmc::detail::set_continuation(t, &continuation);
        tmc::detail::set_continuation_executor(t, &continuation_executor);
        tmc::detail::set_done_count(t, &done_count);
        taskArr.push_back(t);
        ++Begin;
        ++taskCount;
      }
      if (taskCount == 0) {
        return;
      }
      // We couldn't bind result_ptr before we determined how many tasks there
      // are, because reallocation would invalidate those pointers. Now bind
      // them.
      result_arr.resize(taskCount);
      for (size_t i = 0; i < taskCount; ++i) {
        auto t = tmc::detail::unsafe_task<Result>::from_address(
          TMC_WORK_ITEM_AS_STD_CORO(taskArr[i]).address()
        );
        tmc::detail::set_result_ptr(t, &result_arr[i]);
      }
    }

    if (DoSymmetricTransfer) {
      symmetric_task = tmc::detail::unsafe_task<Result>::from_address(
        TMC_WORK_ITEM_AS_STD_CORO(taskArr[taskCount - 1]).address()
      );
    }
    auto postCount = DoSymmetricTransfer ? taskCount - 1 : taskCount;
    done_count.store(
      static_cast<int64_t>(postCount), std::memory_order_release
    );

    if (postCount != 0) {
      tmc::detail::post_bulk_checked(Executor, taskArr.data(), postCount, Prio);
    }
  }

public:
  /// Always suspends.
  inline bool await_ready() const noexcept { return false; }

  /// Suspends the outer coroutine, submits the wrapped task to the
  /// executor, and waits for it to complete.
  TMC_FORCE_INLINE inline std::coroutine_handle<>
  await_suspend(std::coroutine_handle<> Outer) noexcept {
    continuation = Outer;
    std::coroutine_handle<> next;
    if (symmetric_task != nullptr) {
      // symmetric transfer to the last task IF it should run immediately
      next = symmetric_task;
    } else {
      // This logic is necessary because we submitted all child tasks before the
      // parent suspended. Allowing parent to be resumed before it suspends
      // would be UB. Therefore we need to block the resumption until here.
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
  /// a `std::vector<Result>` with capacity `Count`.
  inline ResultArray&& await_resume() noexcept { return std::move(result_arr); }
};

template <size_t Count> class aw_task_many_impl<void, Count> {
  std::coroutine_handle<> symmetric_task;
  std::coroutine_handle<> continuation;
  tmc::detail::type_erased_executor* continuation_executor;
  std::atomic<int64_t> done_count;
  using TaskArray = std::conditional_t<
    Count == 0, std::vector<work_item>, std::array<work_item, Count>>;

  template <typename, size_t, typename, typename> friend class aw_task_many;

  // Specialization for iterator of task<void>
  template <typename TaskIter>
  inline aw_task_many_impl(
    TaskIter Iter, size_t TaskCount,
    tmc::detail::type_erased_executor* Executor,
    tmc::detail::type_erased_executor* ContinuationExecutor, size_t Prio,
    bool DoSymmetricTransfer
  )
      : symmetric_task{nullptr}, continuation_executor{ContinuationExecutor},
        done_count{0} {
    TaskArray taskArr;
    if constexpr (Count == 0) {
      taskArr.resize(TaskCount);
    }
    const size_t size = taskArr.size();
    if (size == 0) {
      return;
    }
    size_t i = 0;
    for (; i < size; ++i) {
      // TODO this std::move allows silently moving-from pointers and arrays
      // reimplement those usages with move_iterator instead
      tmc::detail::unsafe_task<void> t(tmc::detail::into_task(std::move(*Iter))
      );
      tmc::detail::set_continuation(t, &continuation);
      tmc::detail::set_continuation_executor(t, &continuation_executor);
      tmc::detail::set_done_count(t, &done_count);
      taskArr[i] = t;
      ++Iter;
    }
    if (DoSymmetricTransfer) {
      symmetric_task = tmc::detail::unsafe_task<void>::from_address(
        TMC_WORK_ITEM_AS_STD_CORO(taskArr[i - 1]).address()
      );
    }
    auto postCount = DoSymmetricTransfer ? size - 1 : size;
    done_count.store(
      static_cast<int64_t>(postCount), std::memory_order_release
    );

    if (postCount != 0) {
      tmc::detail::post_bulk_checked(Executor, taskArr.data(), postCount, Prio);
    }
  }

  template <typename TaskIter>
  inline aw_task_many_impl(
    TaskIter Begin, TaskIter End, size_t MaxCount,
    tmc::detail::type_erased_executor* Executor,
    tmc::detail::type_erased_executor* ContinuationExecutor, size_t Prio,
    bool DoSymmetricTransfer
  )
    requires(requires(TaskIter a, TaskIter b) {
              ++a;
              *a;
              a != b;
            })
      : symmetric_task{nullptr}, continuation_executor{ContinuationExecutor},
        done_count{0} {
    TaskArray taskArr;
    if constexpr (Count == 0 && requires(TaskIter a, TaskIter b) { a - b; }) {
      // Caller didn't specify capacity to preallocate, but we can calculate
      size_t iterSize = static_cast<size_t>(End - Begin);
      if (MaxCount < iterSize) {
        taskArr.resize(MaxCount);
      } else {
        taskArr.resize(iterSize);
      }
    }

    size_t taskCount = 0;
    if constexpr (Count != 0 || requires(TaskIter a, TaskIter b) { a - b; }) {
      // Iterator could produce less than Count tasks, so count them.
      // Iterator could produce more than Count tasks - stop after taking Count.
      const size_t size = taskArr.size();
      while (Begin != End) {
        if (taskCount == size) {
          break;
        }
        // TODO this std::move allows silently moving-from pointers and arrays
        // reimplement those usages with move_iterator instead
        // TODO if the original iterator is a vector, why create another here?
        tmc::detail::unsafe_task<void> t(tmc::detail::into_task(std::move(*Begin
        )));
        tmc::detail::set_continuation(t, &continuation);
        tmc::detail::set_continuation_executor(t, &continuation_executor);
        tmc::detail::set_done_count(t, &done_count);
        taskArr[taskCount] = t;
        ++Begin;
        ++taskCount;
      }
    } else {
      // We have no idea how many tasks there will be.
      while (Begin != End) {
        if (taskCount == MaxCount) {
          break;
        }
        // TODO this std::move allows silently moving-from pointers and arrays
        // reimplement those usages with move_iterator instead
        // TODO if the original iterator is a vector, why create another here?
        tmc::detail::unsafe_task<void> t(tmc::detail::into_task(std::move(*Begin
        )));
        tmc::detail::set_continuation(t, &continuation);
        tmc::detail::set_continuation_executor(t, &continuation_executor);
        tmc::detail::set_done_count(t, &done_count);
        taskArr.push_back(t);
        ++Begin;
        ++taskCount;
      }
    }

    if (taskCount == 0) {
      return;
    }
    if (DoSymmetricTransfer) {
      symmetric_task = tmc::detail::unsafe_task<void>::from_address(
        TMC_WORK_ITEM_AS_STD_CORO(taskArr[taskCount - 1]).address()
      );
    }
    auto postCount = DoSymmetricTransfer ? taskCount - 1 : taskCount;
    done_count.store(
      static_cast<int64_t>(postCount), std::memory_order_release
    );

    if (postCount != 0) {
      tmc::detail::post_bulk_checked(Executor, taskArr.data(), postCount, Prio);
    }
  }

public:
  /// Always suspends.
  inline bool await_ready() const noexcept { return false; }

  /// Suspends the outer coroutine, submits the wrapped task to the
  /// executor, and waits for it to complete.
  TMC_FORCE_INLINE inline std::coroutine_handle<>
  await_suspend(std::coroutine_handle<> Outer) noexcept {
    continuation = Outer;
    std::coroutine_handle<> next;
    if (symmetric_task != nullptr) {
      // symmetric transfer to the last task IF it should run immediately
      next = symmetric_task;
    } else {
      // This logic is necessary because we submitted all child tasks before the
      // parent suspended. Allowing parent to be resumed before it suspends
      // would be UB. Therefore we need to block the resumption until here.
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

  /// Does nothing.
  inline void await_resume() noexcept {}
};

template <typename Result, size_t Count>
using aw_task_many_run_early =
  tmc::detail::rvalue_only_awaitable<aw_task_many_impl<Result, Count>>;

// Primary template is forward-declared in "tmc/detail/aw_run_early.hpp".
template <typename Result, size_t Count, typename IterBegin, typename IterEnd>
class [[nodiscard(
  "You must use the aw_task_many<Result> by one of: 1. co_await 2. run_early()"
)]] aw_task_many
    : public tmc::detail::run_on_mixin<
        aw_task_many<Result, Count, IterBegin, IterEnd>>,
      public tmc::detail::resume_on_mixin<
        aw_task_many<Result, Count, IterBegin, IterEnd>>,
      public tmc::detail::with_priority_mixin<
        aw_task_many<Result, Count, IterBegin, IterEnd>> {
  friend class tmc::detail::run_on_mixin<aw_task_many>;
  friend class tmc::detail::resume_on_mixin<aw_task_many>;
  friend class tmc::detail::with_priority_mixin<aw_task_many>;
  static_assert(sizeof(task<Result>) == sizeof(std::coroutine_handle<>));
  static_assert(alignof(task<Result>) == alignof(std::coroutine_handle<>));

  IterBegin iter;
  IterEnd sentinel;
  size_t maxCount;
  tmc::detail::type_erased_executor* executor;
  tmc::detail::type_erased_executor* continuation_executor;
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
        executor(tmc::detail::this_thread::executor),
        continuation_executor(tmc::detail::this_thread::executor),
        prio(tmc::detail::this_thread::this_task.prio)
#ifndef NDEBUG
        ,
        did_await(false)
#endif
  {
  }

  // aw_task_many(IterBegin TaskIterator)
  //   requires(Count != 0)
  //     : aw_task_many(TaskIterator, Count) {}

  aw_task_many_impl<Result, Count> operator co_await() && {
#ifndef NDEBUG
    assert(!did_await);
    did_await = true;
#endif
    bool doSymmetricTransfer = tmc::detail::this_thread::exec_is(executor) &&
                               tmc::detail::this_thread::prio_is(prio);
    if constexpr (std::is_convertible_v<IterEnd, size_t>) {
      // "Sentinel" is actually a count
      return aw_task_many_impl<Result, Count>(
        std::move(iter), std::move(sentinel), executor, continuation_executor,
        prio, doSymmetricTransfer
      );
    } else {
      // We have both a sentinel and a MaxCount
      return aw_task_many_impl<Result, Count>(
        std::move(iter), std::move(sentinel), maxCount, executor,
        continuation_executor, prio, doSymmetricTransfer
      );
    }
  }

  /// Submits the tasks to the executor immediately. They cannot be awaited
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
        taskArr[i] = tmc::detail::into_task(std::move(*iter));
        ++iter;
      }
      tmc::detail::post_bulk_checked(executor, taskArr.data(), size, prio);
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
          taskArr[taskCount] = tmc::detail::into_task(std::move(*iter));
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
          taskArr.emplace_back(tmc::detail::into_task(std::move(*iter)));
          ++iter;
          ++taskCount;
        }
      }

      if (taskCount != 0) {
        tmc::detail::post_bulk_checked(
          executor, taskArr.data(), taskCount, prio
        );
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

  /// Submits the tasks to the executor immediately, without suspending the
  /// current coroutine. You must await the return type before destroying it.
  inline aw_task_many_run_early<Result, Count> run_early() && {
#ifndef NDEBUG
    assert(!did_await);
    did_await = true;
#endif
    if constexpr (std::is_convertible_v<IterEnd, size_t>) {
      // "Sentinel" is actually a count
      return aw_task_many_run_early<Result, Count>(
        std::move(iter), std::move(sentinel), executor, continuation_executor,
        prio, false
      );
    } else {
      // We have both a sentinel and a MaxCount
      return aw_task_many_run_early<Result, Count>(
        std::move(iter), std::move(sentinel), maxCount, executor,
        continuation_executor, prio, false
      );
    }
  }

  /// Rather than waiting for all results at once, each result will be made
  /// available immediately as it becomes ready. Returns results one at a time,
  /// as they become ready. Each time this is co_awaited, it will return the
  /// index of a single ready result. The result indexes correspond to the
  /// indexes of the originally submitted tasks, and the values can be accessed
  /// using `operator[]`. Results may become ready in any order, but when
  /// awaited repeatedly, each index from `[0..task_count)` will be returned
  /// exactly once. You must await this repeatedly until all tasks are complete,
  /// at which point the index returned will be equal to the value of `end()`.
  inline aw_task_many_each<Result, Count> each() && {
#ifndef NDEBUG
    assert(!did_await);
    did_await = true;
#endif
    if constexpr (std::is_convertible_v<IterEnd, size_t>) {
      // "Sentinel" is actually a count
      return aw_task_many_each<Result, Count>(
        std::move(iter), std::move(sentinel), executor, continuation_executor,
        prio
      );
    } else {
      // We have both a sentinel and a MaxCount
      return aw_task_many_each<Result, Count>(
        std::move(iter), std::move(sentinel), maxCount, executor,
        continuation_executor, prio
      );
    }
  }
};

} // namespace tmc
