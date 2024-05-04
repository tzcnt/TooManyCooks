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

/// For use when the number of items to spawn is known at compile time
/// `Count` must be non-zero.
/// `Iter` must be an iterator type that implements `operator*()` and
/// `Iter& operator++()`.
///
/// Submits `Count` items to the executor.
template <
  size_t Count, typename TaskIter,
  typename Result = typename std::iter_value_t<TaskIter>::result_type>
aw_task_many<Result, Count, TaskIter, size_t> spawn_many(TaskIter TaskIterator)
  requires(std::is_convertible_v<std::iter_value_t<TaskIter>, task<Result>>)
{
  static_assert(Count != 0);
  return aw_task_many<Result, Count, TaskIter, size_t>(TaskIterator);
}

/// For use when the number of items to spawn is a runtime parameter.
/// `It` must be an iterator type that implements `operator*()` and
/// `It& operator++()`.
/// `TaskCount` must be non-zero.
///
/// Submits `TaskCount` items to the executor.
template <
  typename TaskIter,
  typename Result = typename std::iter_value_t<TaskIter>::result_type>
aw_task_many<Result, 0, TaskIter, size_t>
spawn_many(TaskIter TaskIterator, size_t TaskCount)
  requires(std::is_convertible_v<std::iter_value_t<TaskIter>, task<Result>>)
{
  return aw_task_many<Result, 0, TaskIter, size_t>(TaskIterator, TaskCount);
}

template <
  size_t Count = 0, typename TaskIter,
  typename Result = typename std::iter_value_t<TaskIter>::result_type>
aw_task_many<Result, Count, TaskIter, TaskIter>
spawn_many(TaskIter BeginIter, TaskIter EndIter)
  requires(std::is_convertible_v<std::iter_value_t<TaskIter>, task<Result>>)
{
  return aw_task_many<Result, Count, TaskIter, TaskIter>(BeginIter, EndIter);
}

/// For use when the number of items to spawn is known at compile time
/// `Count` must be non-zero.
/// `Functor` must be a copyable type that implements `Result operator()`.
/// `FunctorIterator` must be a pointer to an array of `Functor`.
///
/// Submits `Count` items to the executor.
template <
  size_t Count, typename FuncIter,
  typename Functor = std::iter_value_t<FuncIter>,
  typename Result = std::invoke_result_t<Functor>>
aw_task_many<Result, Count, FuncIter, size_t>
spawn_many(FuncIter FunctorIterator)
  requires(
    std::is_invocable_r_v<Result, Functor> && (!requires {
      typename Functor::result_type;
    } || !std::is_convertible_v<Functor, task<typename Functor::result_type>>)
  )
{
  static_assert(Count != 0);
  return aw_task_many<Result, Count, FuncIter, size_t>(FunctorIterator);
}

/// For use when the number of items to spawn is a runtime parameter.
/// `Functor` must be a copyable type that implements `Result operator()`.
/// `FunctorIterator` must be a pointer to an array of `Functor`.
/// `FunctorCount` must be non-zero.
///
/// Submits `FunctorCount` items to the executor.
template <
  typename FuncIter, typename Functor = std::iter_value_t<FuncIter>,
  typename Result = std::invoke_result_t<Functor>>
aw_task_many<Result, 0, FuncIter, size_t>
spawn_many(FuncIter FunctorIterator, size_t FunctorCount)
  requires(
    std::is_invocable_r_v<Result, Functor> && (!requires {
      typename Functor::result_type;
    } || !std::is_convertible_v<Functor, task<typename Functor::result_type>>)
  )
{
  return aw_task_many<Result, 0, FuncIter, size_t>(
    FunctorIterator, FunctorCount
  );
}

template <
  size_t Count = 0, typename FuncIter,
  typename Functor = std::iter_value_t<FuncIter>,
  typename Result = std::invoke_result_t<Functor>>
aw_task_many<Result, Count, FuncIter, size_t>
spawn_many(FuncIter BeginIter, FuncIter EndIter)
  requires(
    std::is_invocable_r_v<Result, Functor> && (!requires {
      typename Functor::result_type;
    } || !std::is_convertible_v<Functor, task<typename Functor::result_type>>)
  )
{
  return aw_task_many<Result, Count, FuncIter, size_t>(BeginIter, EndIter);
}

template <typename Result, size_t Count> class aw_task_many_impl {
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

  template <typename, size_t, typename, typename> friend class aw_task_many;

  template <typename TaskIter>
  inline aw_task_many_impl(
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
      Executor->post_bulk(taskArr.data(), Prio, postCount);
    }
  }

  template <typename TaskIter>
  inline aw_task_many_impl(
    TaskIter IterBegin, TaskIter IterEnd,
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
      // Caller didn't specify capacity to preallocate, but we can calculate it
      result_arr.resize(IterEnd - IterBegin);
      taskArr.resize(IterEnd - IterBegin);
    }

    size_t taskCount;
    if constexpr (Count != 0 || requires(TaskIter a, TaskIter b) { a - b; }) {
      // We know there will be at most taskArr.size() tasks, but there could be
      // less, so count the number of tasks that were actually produced.
      taskCount = 0;
      while (IterBegin != IterEnd) {
        // TODO this std::move allows silently moving-from pointers and arrays
        // reimplement those usages with move_iterator instead
        // TODO if the original iterator is a vector, why create another here?
        detail::unsafe_task<Result> t(detail::into_task(std::move(*IterBegin)));
        auto& p = t.promise();
        p.continuation = &continuation;
        p.continuation_executor = &continuation_executor;
        p.done_count = &done_count;
        p.result_ptr = &result_arr[taskCount];
        taskArr[taskCount] = t;
        ++IterBegin;
        ++taskCount;
      }
      if (taskCount == 0) {
        return;
      }
    } else {
      // We have no idea how many tasks there will be.
      while (IterBegin != IterEnd) {
        // TODO this std::move allows silently moving-from pointers and arrays
        // reimplement those usages with move_iterator instead
        // TODO if the original iterator is a vector, why create another here?
        detail::unsafe_task<Result> t(detail::into_task(std::move(*IterBegin)));
        auto& p = t.promise();
        p.continuation = &continuation;
        p.continuation_executor = &continuation_executor;
        p.done_count = &done_count;
        taskArr.push_back(t);
        ++IterBegin;
      }
      taskCount = taskArr.size();
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
      Executor->post_bulk(taskArr.data(), Prio, postCount);
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
            continuation_executor == detail::this_thread::executor) {
          next = Outer;
        } else {
          // Need to resume on a different executor
          continuation_executor->post(
            std::move(Outer), detail::this_thread::this_task.prio
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
  detail::unsafe_task<void> symmetric_task;
  std::coroutine_handle<> continuation;
  detail::type_erased_executor* continuation_executor;
  std::atomic<int64_t> done_count;
  using TaskArray = std::conditional_t<
    Count == 0, std::vector<work_item>, std::array<work_item, Count>>;

  template <typename, size_t, typename, typename> friend class aw_task_many;

  // Specialization for iterator of task<void>
  template <typename TaskIter>
  inline aw_task_many_impl(
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
      Executor->post_bulk(taskArr.data(), Prio, postCount);
    }
  }

  template <typename TaskIter>
  inline aw_task_many_impl(
    TaskIter IterBegin, TaskIter IterEnd,
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
      // Caller didn't specify capacity to preallocate, but we can calculate it
      taskArr.resize(IterEnd - IterBegin);
    }

    size_t taskCount;
    if constexpr (Count != 0 || requires(TaskIter a, TaskIter b) { a - b; }) {
      // We know there will be at most taskArr.size() tasks, but there could be
      // less, so count the number of tasks that were actually produced.
      taskCount = 0;
      while (IterBegin != IterEnd) {
        // TODO this std::move allows silently moving-from pointers and arrays
        // reimplement those usages with move_iterator instead
        // TODO if the original iterator is a vector, why create another here?
        detail::unsafe_task<void> t(detail::into_task(std::move(*IterBegin)));
        auto& p = t.promise();
        p.continuation = &continuation;
        p.continuation_executor = &continuation_executor;
        p.done_count = &done_count;
        taskArr[taskCount] = t;
        ++IterBegin;
        ++taskCount;
      }
    } else {
      // We have no idea how many tasks there will be.
      while (IterBegin != IterEnd) {
        // TODO this std::move allows silently moving-from pointers and arrays
        // reimplement those usages with move_iterator instead
        // TODO if the original iterator is a vector, why create another here?
        detail::unsafe_task<void> t(detail::into_task(std::move(*IterBegin)));
        auto& p = t.promise();
        p.continuation = &continuation;
        p.continuation_executor = &continuation_executor;
        p.done_count = &done_count;
        taskArr.push_back(t);
        ++IterBegin;
      }
      taskCount = taskArr.size();
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
      Executor->post_bulk(taskArr.data(), Prio, postCount);
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
            continuation_executor == detail::this_thread::executor) {
          next = Outer;
        } else {
          // Need to resume on a different executor
          continuation_executor->post(
            std::move(Outer), detail::this_thread::this_task.prio
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
  detail::rvalue_only_awaitable<aw_task_many_impl<Result, Count>>;

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

  IterBegin iter;
  IterEnd sentinelOrCount;
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
  aw_task_many(IterBegin TaskIterator, IterEnd SentinelOrTaskCount)
      : iter{TaskIterator}, sentinelOrCount{SentinelOrTaskCount},
        executor(detail::this_thread::executor),
        continuation_executor(detail::this_thread::executor),
        prio(detail::this_thread::this_task.prio)
#ifndef NDEBUG
        ,
        did_await(false)
#endif
  {
  }

  /// For use when `Count` is known at compile time.
  /// It is recommended to call `spawn_many()` instead of using this constructor
  /// directly.
  aw_task_many(IterBegin TaskIterator)
    requires(Count != 0)
      : aw_task_many(TaskIterator, Count) {}

  aw_task_many_impl<Result, Count> operator co_await() && {
#ifndef NDEBUG
    assert(!did_await);
    did_await = true;
#endif
    bool doSymmetricTransfer =
      executor == detail::this_thread::executor &&
      prio <= detail::this_thread::this_task.yield_priority->load(
                std::memory_order_relaxed
              );
    return aw_task_many_impl<Result, Count>(
      std::move(iter), std::move(sentinelOrCount), executor,
      continuation_executor, prio, doSymmetricTransfer
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
    if constexpr (Count == 0) {
      taskArr.resize(sentinelOrCount);
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
    executor->post_bulk(taskArr.data(), prio, size);
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
  inline aw_task_many_run_early<Result, Count> run_early() && {
#ifndef NDEBUG
    assert(!did_await);
    did_await = true;
#endif
    return aw_task_many_run_early<Result, Count>(
      std::move(iter), sentinelOrCount, executor, continuation_executor, prio,
      false
    );
  }
};

} // namespace tmc
