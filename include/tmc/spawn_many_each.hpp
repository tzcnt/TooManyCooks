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

template <typename Result, size_t Count> class aw_task_many_each_impl {
public:
  detail::unsafe_task<Result> symmetric_task;
  std::coroutine_handle<> continuation;
  detail::type_erased_executor* continuation_executor;
  using TaskArray = std::conditional_t<
    Count == 0, std::vector<work_item>, std::array<work_item, Count>>;
  using ResultArray = std::conditional_t<
    Count == 0, std::vector<Result>, std::array<Result, Count>>;
  std::atomic<int64_t> done_count;
  ResultArray result_arr;

  template <typename, size_t, typename, typename>
  friend class aw_task_many_each;

  template <typename TaskIter>
  inline aw_task_many_each_impl(
    TaskIter Iter, size_t TaskCount, detail::type_erased_executor* Executor,
    detail::type_erased_executor* ContinuationExecutor, size_t Prio,
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
      detail::unsafe_task<Result> t(detail::into_task(std::move(*Iter)));
      auto& p = t.promise();
      p.continuation = &continuation;
      p.continuation_executor = &continuation_executor;
      p.done_count = &done_count;
      p.result_ptr = &result_arr[i];
      taskArr[i] = t;
      ++Iter;
    }
    if (DoSymmetricTransfer) {
      symmetric_task = detail::unsafe_task<Result>::from_address(
        TMC_WORK_ITEM_AS_STD_CORO(taskArr[i - 1]).address()
      );
    }
    auto postCount = DoSymmetricTransfer ? size - 1 : size;
    done_count.store(
      static_cast<int64_t>(postCount), std::memory_order_release
    );

    if (postCount != 0) {
      detail::post_bulk_checked(Executor, taskArr.data(), postCount, Prio);
    }
  }

  template <typename TaskIter>
  inline aw_task_many_each_impl(
    TaskIter Begin, TaskIter End, size_t MaxCount,
    detail::type_erased_executor* Executor,
    detail::type_erased_executor* ContinuationExecutor, size_t Prio,
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
        detail::unsafe_task<Result> t(detail::into_task(std::move(*Begin)));
        auto& p = t.promise();
        p.continuation = &continuation;
        p.continuation_executor = &continuation_executor;
        p.done_count = &done_count;
        p.result_ptr = &result_arr[taskCount];
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
        detail::unsafe_task<Result> t(detail::into_task(std::move(*Begin)));
        auto& p = t.promise();
        p.continuation = &continuation;
        p.continuation_executor = &continuation_executor;
        p.done_count = &done_count;
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
        auto t = detail::unsafe_task<Result>::from_address(
          TMC_WORK_ITEM_AS_STD_CORO(taskArr[i]).address()
        );
        t.promise().result_ptr = &result_arr[i];
      }
    }

    if (DoSymmetricTransfer) {
      symmetric_task = detail::unsafe_task<Result>::from_address(
        TMC_WORK_ITEM_AS_STD_CORO(taskArr[taskCount - 1]).address()
      );
    }
    auto postCount = DoSymmetricTransfer ? taskCount - 1 : taskCount;
    done_count.store(
      static_cast<int64_t>(postCount), std::memory_order_release
    );

    if (postCount != 0) {
      detail::post_bulk_checked(Executor, taskArr.data(), postCount, Prio);
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
    }
    return next;
  }

  /// If `Count` is a compile-time template argument, returns a
  /// `std::array<Result, Count>`. If `Count` is a runtime parameter, returns
  /// a `std::vector<Result>` with capacity `Count`.
  inline ResultArray&& await_resume() noexcept { return std::move(result_arr); }
};

template <size_t Count> class aw_task_many_each_impl<void, Count> {
  detail::unsafe_task<void> symmetric_task;
  std::coroutine_handle<> continuation;
  detail::type_erased_executor* continuation_executor;
  std::atomic<int64_t> done_count;
  using TaskArray = std::conditional_t<
    Count == 0, std::vector<work_item>, std::array<work_item, Count>>;

  template <typename, size_t, typename, typename>
  friend class aw_task_many_each;

  // Specialization for iterator of task<void>
  template <typename TaskIter>
  inline aw_task_many_each_impl(
    TaskIter Iter, size_t TaskCount, detail::type_erased_executor* Executor,
    detail::type_erased_executor* ContinuationExecutor, size_t Prio,
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
      detail::unsafe_task<void> t(detail::into_task(std::move(*Iter)));
      auto& p = t.promise();
      p.continuation = &continuation;
      p.continuation_executor = &continuation_executor;
      p.done_count = &done_count;
      taskArr[i] = t;
      ++Iter;
    }
    if (DoSymmetricTransfer) {
      symmetric_task = detail::unsafe_task<void>::from_address(
        TMC_WORK_ITEM_AS_STD_CORO(taskArr[i - 1]).address()
      );
    }
    auto postCount = DoSymmetricTransfer ? size - 1 : size;
    done_count.store(
      static_cast<int64_t>(postCount), std::memory_order_release
    );

    if (postCount != 0) {
      detail::post_bulk_checked(Executor, taskArr.data(), postCount, Prio);
    }
  }

  template <typename TaskIter>
  inline aw_task_many_each_impl(
    TaskIter Begin, TaskIter End, size_t MaxCount,
    detail::type_erased_executor* Executor,
    detail::type_erased_executor* ContinuationExecutor, size_t Prio,
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
        detail::unsafe_task<void> t(detail::into_task(std::move(*Begin)));
        auto& p = t.promise();
        p.continuation = &continuation;
        p.continuation_executor = &continuation_executor;
        p.done_count = &done_count;
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
        detail::unsafe_task<void> t(detail::into_task(std::move(*Begin)));
        auto& p = t.promise();
        p.continuation = &continuation;
        p.continuation_executor = &continuation_executor;
        p.done_count = &done_count;
        taskArr.push_back(t);
        ++Begin;
        ++taskCount;
      }
    }

    if (taskCount == 0) {
      return;
    }
    if (DoSymmetricTransfer) {
      symmetric_task = detail::unsafe_task<void>::from_address(
        TMC_WORK_ITEM_AS_STD_CORO(taskArr[taskCount - 1]).address()
      );
    }
    auto postCount = DoSymmetricTransfer ? taskCount - 1 : taskCount;
    done_count.store(
      static_cast<int64_t>(postCount), std::memory_order_release
    );

    if (postCount != 0) {
      detail::post_bulk_checked(Executor, taskArr.data(), postCount, Prio);
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
    }
    return next;
  }

  /// Does nothing.
  inline void await_resume() noexcept {}
};

template <typename Result, size_t Count>
using aw_task_many_each =
  detail::rvalue_only_awaitable<aw_task_many_each_impl<Result, Count>>;

} // namespace tmc
