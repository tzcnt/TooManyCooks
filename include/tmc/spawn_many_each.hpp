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
    // TODO enforce size < 64
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
      static_cast<int64_t>((1ULL << size) - 1), std::memory_order_release
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
      static_cast<int64_t>((1ULL << taskCount) - 1), std::memory_order_release
    );

    if (postCount != 0) {
      detail::post_bulk_checked(Executor, taskArr.data(), postCount, Prio);
    }
  }

public:
  /// Always suspends.
  inline bool await_ready() const noexcept {
    // High bit is unset, because we are running
    return done_count.load(std::memory_order_acquire) != 0;
  }

  /// Suspends the outer coroutine, submits the wrapped task to the
  /// executor, and waits for each of them to complete.
  TMC_FORCE_INLINE inline bool await_suspend(std::coroutine_handle<> Outer
  ) noexcept {
    continuation = Outer;
    // This logic is necessary because we submitted all child tasks before the
    // parent suspended. Allowing parent to be resumed before it suspends
    // would be UB. Therefore we need to block the resumption until here.
    // WARNING: You can use fetch_add here because we know this bit wasn't set.
    // It generates xadd instruction which is slightly more efficient than
    // fetch_or. But not safe to use if the bit might already be set.
    auto readyTasks =
      done_count.fetch_add(detail::task_flags::EACH, std::memory_order_acq_rel);
#ifdef _MSC_VER
    size_t readyTaskCount = static_cast<size_t>(__popcnt64(readyTasks));
#else
    size_t readyTaskCount =
      static_cast<size_t>(__builtin_popcountll(readyTasks));
#endif
    if (readyTaskCount != 0) {
      // A child task already completed, so try to resume immediately.
      // Reset the running bitflag to confirm a 3rd task didn't already resume.
      bool should_resume =
        detail::task_flags::EACH &
        done_count.fetch_and(
          ~(detail::task_flags::EACH), std::memory_order_acq_rel
        );
      if (should_resume) {
        if (continuation_executor == nullptr ||
            detail::this_thread::exec_is(continuation_executor)) {
          return false; // OK to resume inline
        }
        // Need to resume on a different executor
        detail::post_checked(
          continuation_executor, std::move(Outer),
          detail::this_thread::this_task.prio
        );
      }
    }
    return true;
  }

  /// Returns the index of the current ready result. The result indexes
  /// correspond to the indexes of the originally submitted tasks. Results may
  /// become ready in any order, but when awaited repeatedly, each index from
  /// `[0..end())` will be returned exactly once. When there are no more results
  /// to be returned, the returned index will be equal to `end()`.
  inline size_t await_resume() noexcept {
    size_t slots = done_count.load(std::memory_order_acquire);
#ifdef _MSC_VER
    size_t slot = static_cast<size_t>(_tzcnt_u64(slots));
#else
    size_t slot = static_cast<size_t>(__builtin_ctzll(slots));
#endif
    // High bit is unset, because we are resuming
    if (slot == 64) {
      return end();
    }
    // TODO make sure this uses LOCK AND, and not CMPXCHG on x86
    // Otherwise try fetch_sub
    done_count.fetch_and(int64_t(~(1ULL << slot)), std::memory_order_release);
    return slot;
  }

  /// Provides a sentinel value that can be compared against the value returned
  /// from co_await.
  inline size_t end() noexcept { return 64; }

  // Gets the ready result at the given index.
  inline Result& operator[](size_t idx) noexcept { return result_arr[idx]; }
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
      static_cast<int64_t>((1ULL << size) - 1), std::memory_order_release
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
      // Iterator could produce more than Count tasks - stop after taking
      // Count.
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
      static_cast<int64_t>((1ULL << taskCount) - 1), std::memory_order_release
    );

    if (postCount != 0) {
      detail::post_bulk_checked(Executor, taskArr.data(), postCount, Prio);
    }
  }

public:
  /// Always suspends.
  inline bool await_ready() const noexcept {
    // High bit is unset, because we are running
    return done_count.load(std::memory_order_acquire) != 0;
  }

  /// Suspends the outer coroutine, submits the wrapped task to the
  /// executor, and waits for each of them to complete.
  TMC_FORCE_INLINE inline bool await_suspend(std::coroutine_handle<> Outer
  ) noexcept {
    continuation = Outer;
    // This logic is necessary because we submitted all child tasks before the
    // parent suspended. Allowing parent to be resumed before it suspends
    // would be UB. Therefore we need to block the resumption until here.
    // WARNING: You can use fetch_add here because we know this bit wasn't set.
    // It generates xadd instruction which is slightly more efficient than
    // fetch_or. But not safe to use if the bit might already be set.
    auto readyTasks =
      done_count.fetch_add(detail::task_flags::EACH, std::memory_order_acq_rel);
#ifdef _MSC_VER
    size_t readyTaskCount = static_cast<size_t>(__popcnt64(readyTasks));
#else
    size_t readyTaskCount =
      static_cast<size_t>(__builtin_popcountll(readyTasks));
#endif
    if (readyTaskCount != 0) {
      // A child task already completed, so try to resume immediately.
      // Reset the running bitflag to confirm a 3rd task didn't already resume.
      bool should_resume =
        detail::task_flags::EACH &
        done_count.fetch_and(
          ~(detail::task_flags::EACH), std::memory_order_acq_rel
        );
      if (should_resume) {
        if (continuation_executor == nullptr ||
            detail::this_thread::exec_is(continuation_executor)) {
          return false; // OK to resume inline
        }
        // Need to resume on a different executor
        detail::post_checked(
          continuation_executor, std::move(Outer),
          detail::this_thread::this_task.prio
        );
      }
    }
    return true;
  }

  /// Returns the index of the current ready result. The result indexes
  /// correspond to the indexes of the originally submitted tasks. Results may
  /// become ready in any order, but when awaited repeatedly, each index from
  /// `[0..end())` will be returned exactly once. When there are no more results
  /// to be returned, the returned index will be equal to `end()`.
  inline size_t await_resume() noexcept {
    size_t slots = done_count.load(std::memory_order_acquire);
#ifdef _MSC_VER
    size_t slot = static_cast<size_t>(_tzcnt_u64(slots));
#else
    size_t slot = static_cast<size_t>(__builtin_ctzll(slots));
#endif
    // High bit is unset, because we are resuming
    if (slot == 64) {
      return end();
    }
    // TODO make sure this uses LOCK AND, and not CMPXCHG on x86
    // Otherwise try fetch_sub
    done_count.fetch_and(int64_t(~(1ULL << slot)), std::memory_order_release);
    return slot;
  }

  /// Provides a sentinel value that can be compared against the value returned
  /// from co_await.
  inline size_t end() noexcept { return 64; }

  // Provided for convenience only - to expose the same API as the
  // Result-returning awaitable version. Does nothing.
  inline void operator[](size_t idx) noexcept {}
};

template <typename Result, size_t Count>
using aw_task_many_each =
  detail::rvalue_only_awaitable<aw_task_many_each_impl<Result, Count>>;

} // namespace tmc
