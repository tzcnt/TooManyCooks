#pragma once
#include "tmc/detail/aw_run_early.hpp"
#include "tmc/detail/concepts.hpp" // IWYU pragma: keep
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

/// For use when the number of items to spawn is known at compile time
/// `Count` must be non-zero.
/// `Iter` must be an iterator type that implements `operator*()` and
/// `Iter& operator++()`.
///
/// Submits `Count` items to the executor.
template <
  size_t Count, typename TaskIter,
  typename Result = typename std::iter_value_t<TaskIter>::result_type>
aw_task_many<Result, Count, TaskIter> spawn_many(TaskIter TaskIterator)
  requires(std::is_convertible_v<std::iter_value_t<TaskIter>, task<Result>>)
{
  static_assert(Count != 0);
  return aw_task_many<Result, Count, TaskIter>(TaskIterator);
}

/// For use when the number of items to spawn is known at compile time
/// `Count` must be non-zero.
/// `Functor` must be a copyable type that implements `Result operator()`.
/// `FunctorIterator` must be a pointer to an array of `Functor`.
///
/// Submits `Count` items to the executor.
// template <
//   size_t Count, typename FuncIter,
//   typename Functor = std::iter_value_t<FuncIter>,
//   typename Result = std::invoke_result_t<Functor>>
// aw_task_many<Result, Count> spawn_many(FuncIter FunctorIterator)
//   requires(
//     std::is_invocable_r_v<Result, Functor> && (!requires {
//       typename Functor::result_type;
//     } || !std::is_convertible_v<Functor, task<typename
//     Functor::result_type>>)
//   )
// {
//   static_assert(Count != 0);
//   return aw_task_many<Result, Count>(FunctorIterator);
// }

/// For use when the number of items to spawn is a runtime parameter.
/// `It` must be an iterator type that implements `operator*()` and
/// `It& operator++()`.
/// `TaskCount` must be non-zero.
///
/// Submits `TaskCount` items to the executor.
template <
  typename TaskIter,
  typename Result = typename std::iter_value_t<TaskIter>::result_type>
aw_task_many<Result, 0, TaskIter>
spawn_many(TaskIter TaskIterator, size_t TaskCount)
  requires(std::is_convertible_v<std::iter_value_t<TaskIter>, task<Result>>)
{
  return aw_task_many<Result, 0, TaskIter>(TaskIterator, TaskCount);
}

/// For use when the number of items to spawn is a runtime parameter.
/// `Functor` must be a copyable type that implements `Result operator()`.
/// `FunctorIterator` must be a pointer to an array of `Functor`.
/// `FunctorCount` must be non-zero.
///
/// Submits `FunctorCount` items to the executor.
// template <
//   typename FuncIter, typename Functor = std::iter_value_t<FuncIter>,
//   typename Result = std::invoke_result_t<Functor>>
// aw_task_many<Result, 0>
// spawn_many(FuncIter FunctorIterator, size_t FunctorCount)
//   requires(
//     std::is_invocable_r_v<Result, Functor> && (!requires {
//       typename Functor::result_type;
//     } || !std::is_convertible_v<Functor, task<typename
//     Functor::result_type>>)
//   )
// {
//   return aw_task_many<Result, 0>(FunctorIterator, FunctorCount);
// }

template <typename Result, size_t Count> class aw_task_many_impl;

template <typename Result, size_t Count> class aw_task_many_impl {
  detail::unsafe_task<Result> symmetric_task;
  std::coroutine_handle<> continuation;
  detail::type_erased_executor* continuation_executor;
  using ResultArray = std::conditional_t<
    Count == 0, std::vector<Result>, std::array<Result, Count>>;
  ResultArray result;
  std::atomic<int64_t> done_count;

  template <typename, size_t, typename> friend class aw_task_many;
  template <typename TaskIter>
  inline aw_task_many_impl(
    TaskIter Iter, size_t TaskCount, detail::type_erased_executor* executor,
    detail::type_erased_executor* continuation_executor, size_t prio
  );

public:
  /// Always suspends.
  inline bool await_ready() const noexcept;

  /// Suspends the outer coroutine, submits the wrapped task to the
  /// executor, and waits for it to complete.
  inline std::coroutine_handle<> await_suspend(std::coroutine_handle<> Outer
  ) noexcept;

  /// Returns the value provided by the wrapped task.
  inline ResultArray&& await_resume() noexcept;
};

template <size_t Count> class aw_task_many_impl<void, Count> {
  detail::unsafe_task<void> symmetric_task;
  std::coroutine_handle<> continuation;
  detail::type_erased_executor* continuation_executor;
  std::atomic<int64_t> done_count;

  template <typename, size_t, typename> friend class aw_task_many;

  // Specialization for iterator of task<void>
  template <typename TaskIter>
  inline aw_task_many_impl(
    TaskIter Iter, size_t TaskCount, detail::type_erased_executor* executor,
    detail::type_erased_executor* continuation_executor, size_t prio
  );

public:
  /// Always suspends.
  inline bool await_ready() const noexcept;

  /// Suspends the outer coroutine, submits the wrapped task to the
  /// executor, and waits for it to complete.
  inline std::coroutine_handle<> await_suspend(std::coroutine_handle<> Outer
  ) noexcept;

  /// Does nothing.
  inline void await_resume() noexcept;
};

// Primary template is forward-declared in "tmc/detail/aw_run_early.hpp".
template <typename Result, size_t Count, typename TaskIter>
class [[nodiscard(
  "You must use the aw_task_many<Result> by one of: 1. co_await 2. run_early()"
)]] aw_task_many {
  static_assert(sizeof(task<Result>) == sizeof(std::coroutine_handle<>));
  static_assert(alignof(task<Result>) == alignof(std::coroutine_handle<>));

  /// If `Count` is a compile-time template argument, returns a
  /// `std::array<Result, Count>`. If `Count` is a runtime parameter, returns
  /// a `std::vector<Result>` of size `Count`;
  // friend class aw_run_early<Result, ResultArray>;
  TaskIter iter;
  size_t count;
  detail::type_erased_executor* executor;
  detail::type_erased_executor* continuation_executor;
  size_t prio;
#ifndef NDEBUG
  bool did_await;
#endif

  /// Private / delegated constructor
  aw_task_many(TaskIter TaskIterator, size_t size)
      : iter{TaskIterator}, count{size},
        executor(detail::this_thread::executor),
        continuation_executor(detail::this_thread::executor),
        prio(detail::this_thread::this_task.prio)
#ifndef NDEBUG
        ,
        did_await(false)
#endif
  {
  }

public:
  /// For use when `Count` is known at compile time
  /// It is recommended to call `spawn_many()` instead of using this constructor
  /// directly.
  aw_task_many(TaskIter TaskIterator)
    requires(Count != 0)
      : aw_task_many(TaskIterator, Count) {}

  /// For use when `Count` is a runtime parameter.
  /// It is recommended to call `spawn_many()` instead of using this constructor
  /// directly.
  aw_task_many(TaskIter TaskIterator, size_t TaskCount)
    requires(Count == 0)
      : aw_task_many(TaskIterator, TaskCount) {}

  aw_task_many_impl<Result, Count> operator co_await() {
#ifndef NDEBUG
    assert(!did_await);
    did_await = true;
#endif
    return aw_task_many_impl<Result, Count>(
      std::move(iter), count, executor, continuation_executor, prio
    );
  }

#ifndef NDEBUG
  ~aw_task_many() noexcept { assert(did_await); }
#endif
  aw_task_many(const aw_task_many&) = delete;
  aw_task_many& operator=(const aw_task_many&) = delete;
  aw_task_many(aw_task_many&& Other) = delete;
  // aw_task_many(aw_task_many&& other) {
  //   executor = std::move(other.executor);
  //   continuation = std::move(other.continuation);
  //   wrapped = std::move(other.wrapped);
  //   result = std::move(other.result);
  //   prio = other.prio;
  //   did_await = other.did_await;
  //   other.did_await = true; // prevent other from posting
  // }
  aw_task_many& operator=(aw_task_many&& Other) = delete;
  // aw_task_many& operator=(aw_task_many&& other) {
  //   executor = std::move(other.executor);
  //   continuation = std::move(other.continuation);
  //   wrapped = std::move(other.wrapped);
  //   result = std::move(other.result);
  //   prio = other.prio;
  //   did_await = other.did_await;
  //   other.did_await = true; // prevent other from posting
  //   return *this;
  // }

  /// After the spawned tasks complete, the outer coroutine will be resumed
  /// on the provided executor.
  inline aw_task_many& resume_on(detail::type_erased_executor* Executor) {
    continuation_executor = Executor;
    return *this;
  }
  /// After the spawned tasks complete, the outer coroutine will be resumed
  /// on the provided executor.
  template <detail::TypeErasableExecutor Exec>
  aw_task_many& resume_on(Exec& Executor) {
    return resume_on(Executor.type_erased());
  }
  /// After the spawned tasks complete, the outer coroutine will be resumed
  /// on the provided executor.
  template <detail::TypeErasableExecutor Exec>
  aw_task_many& resume_on(Exec* Executor) {
    return resume_on(Executor->type_erased());
  }

  /// The wrapped tasks will run on the provided executor.
  inline aw_task_many& run_on(detail::type_erased_executor* Executor) {
    executor = Executor;
    return *this;
  }
  /// The wrapped tasks will run on the provided executor.
  template <detail::TypeErasableExecutor Exec>
  aw_task_many& run_on(Exec& Executor) {
    return run_on(Executor.type_erased());
  }
  /// The wrapped tasks will run on the provided executor.
  template <detail::TypeErasableExecutor Exec>
  aw_task_many& run_on(Exec* Executor) {
    return run_on(Executor->type_erased());
  }

  /// Sets the priority of the wrapped tasks. If co_awaited, the outer
  /// coroutine will also be resumed with this priority.
  inline aw_task_many& with_priority(size_t Priority) {
    prio = Priority;
    return *this;
  }

  // /// Submits the wrapped tasks immediately, without suspending the current
  // /// coroutine. You must await the return type before destroying it.
  // inline aw_run_early<Result, ResultArray> run_early() && {
  //   return aw_run_early<Result, ResultArray>(std::move(*this));
  // }
};

template <size_t Count, typename TaskIter>
class [[nodiscard("You must use the aw_task_many<void> by one of: 1. co_await "
                  "2. detach() 3. run_early()"
)]] aw_task_many<void, Count, TaskIter> {
  static_assert(sizeof(task<void>) == sizeof(std::coroutine_handle<>));
  static_assert(alignof(task<void>) == alignof(std::coroutine_handle<>));
  // friend class aw_run_early<void, void>;
  TaskIter iter;
  size_t count;
  detail::type_erased_executor* executor;
  detail::type_erased_executor* continuation_executor;
  size_t prio;
#ifndef NDEBUG
  bool did_await;
#endif

  /// Private / delegated constructor
  aw_task_many(TaskIter TaskIterator, size_t size)
      : iter{TaskIterator}, count{size},
        executor(detail::this_thread::executor),
        continuation_executor(detail::this_thread::executor),
        prio(detail::this_thread::this_task.prio)
#ifndef NDEBUG
        ,
        did_await(false)
#endif
  {
  }

public:
  /// For use when `Count` is known at compile time
  /// It is recommended to call `spawn_many()` instead of using this constructor
  /// directly.
  aw_task_many(TaskIter TaskIterator)
    requires(Count != 0)
      : aw_task_many(TaskIterator, Count) {}

  /// For use when `Count` is a runtime parameter.
  /// It is recommended to call `spawn_many()` instead of using this constructor
  /// directly.
  aw_task_many(TaskIter TaskIterator, size_t TaskCount)
    requires(Count == 0)
      : aw_task_many(TaskIterator, TaskCount) {}

  aw_task_many_impl<void, Count> operator co_await() {
#ifndef NDEBUG
    assert(!did_await);
    did_await = true;
#endif
    return aw_task_many_impl<void, Count>(
      std::move(iter), count, executor, continuation_executor, prio
    );
  }

  /// Submit the tasks to the executor immediately. They cannot be awaited
  /// afterward.
  void detach() {
#ifndef NDEBUG
    assert(!did_await);
    did_await = true;
#endif
    using TaskArray = std::conditional_t<
      Count == 0, std::vector<work_item>, std::array<work_item, Count>>;
    TaskArray arr;
    if constexpr (Count == 0) {
      arr.resize(count);
    }
    const size_t size = arr.size();
    if (size == 0) {
      return;
    }
    for (size_t i = 0; i < size; ++i) {
      arr[i] = detail::into_unsafe_task(*iter);
      ++iter;
    }
    executor->post_bulk(arr.data(), prio, size);
  }

#ifndef NDEBUG
  ~aw_task_many() noexcept { assert(did_await); }
#endif

  aw_task_many(const aw_task_many&) = delete;
  aw_task_many& operator=(const aw_task_many&) = delete;
  aw_task_many(aw_task_many&& Other) = delete;
  // aw_task_many(aw_task_many&& other) {
  //   executor = std::move(other.executor);
  //   continuation = std::move(other.continuation);
  //   wrapped = std::move(other.wrapped);
  //   prio = other.prio;
  //   did_await = other.did_await;
  //   other.did_await = true; // prevent other from posting
  // }
  aw_task_many& operator=(aw_task_many&& Other) = delete;
  // aw_task_many& operator=(aw_task_many&& other) {
  //   executor = std::move(other.executor);
  //   continuation = std::move(other.continuation);
  //   wrapped = std::move(other.wrapped);
  //   prio = other.prio;
  //   did_await = other.did_await;
  //   other.did_await = true; // prevent other from posting
  //   return *this;
  // }

  /// After the spawned tasks complete, the outer coroutine will be resumed
  /// on the provided executor.
  inline aw_task_many& resume_on(detail::type_erased_executor* Executor) {
    continuation_executor = Executor;
    return *this;
  }
  /// After the spawned tasks complete, the outer coroutine will be resumed
  /// on the provided executor.
  template <detail::TypeErasableExecutor Exec>
  aw_task_many& resume_on(Exec& Executor) {
    return resume_on(Executor.type_erased());
  }
  /// After the spawned tasks complete, the outer coroutine will be resumed
  /// on the provided executor.
  template <detail::TypeErasableExecutor Exec>
  aw_task_many& resume_on(Exec* Executor) {
    return resume_on(Executor->type_erased());
  }

  /// The wrapped tasks will run on the provided executor.
  inline aw_task_many& run_on(detail::type_erased_executor* Executor) {
    executor = Executor;
    return *this;
  }
  /// The wrapped tasks will run on the provided executor.
  template <detail::TypeErasableExecutor Exec>
  aw_task_many& run_on(Exec& Executor) {
    return run_on(Executor.type_erased());
  }
  /// The wrapped tasks will run on the provided executor.
  template <detail::TypeErasableExecutor Exec>
  aw_task_many& run_on(Exec* Executor) {
    return run_on(Executor->type_erased());
  }

  /// Sets the priority of the wrapped tasks. If co_awaited, the outer
  /// coroutine will also be resumed with this priority.
  inline aw_task_many& with_priority(size_t Priority) {
    prio = Priority;
    return *this;
  }

  // /// Submits the wrapped task immediately, without suspending the current
  // /// coroutine. You must await the return type before destroying it.
  // inline aw_run_early<void, void> run_early() && {
  //   return aw_run_early<void, void>(std::move(*this));
  // }
}; // namespace tmc

template <typename Result, size_t Count>
template <typename TaskIter>
aw_task_many_impl<Result, Count>::aw_task_many_impl(
  TaskIter Iter, size_t TaskCount, detail::type_erased_executor* Executor,
  detail::type_erased_executor* ContinuationExecutor, size_t Prio
)
    : symmetric_task{nullptr}, continuation_executor{ContinuationExecutor},
      done_count{0} {
  using TaskArray = std::conditional_t<
    Count == 0, std::vector<work_item>, std::array<work_item, Count>>;
  TaskArray arr;
  if constexpr (Count == 0) {
    arr.resize(TaskCount);
    result.resize(TaskCount);
  }
  const size_t size = arr.size();
  if (size == 0) {
    return;
  }
  bool doSymmetricTransfer =
    Executor == detail::this_thread::executor &&
    Prio <= detail::this_thread::this_task.yield_priority->load(
              std::memory_order_relaxed
            );
  size_t i = 0;
  for (; i < size; ++i) {
    detail::unsafe_task<Result> t(detail::into_unsafe_task(*Iter));
    auto& p = t.promise();
    p.continuation = &continuation;
    p.continuation_executor = &continuation_executor;
    p.done_count = &done_count;
    p.result_ptr = &result[i];
    arr[i] = t;
    ++Iter;
  }
  if (doSymmetricTransfer) {
    symmetric_task = detail::unsafe_task<Result>::from_address(
      TMC_WORK_ITEM_AS_STD_CORO(arr[i - 1]).address()
    );
  }
  auto postCount = doSymmetricTransfer ? size - 1 : size;
  done_count.store(static_cast<int64_t>(postCount), std::memory_order_release);

  if (postCount != 0) {
    Executor->post_bulk(arr.data(), Prio, postCount);
  }
}

/// Always suspends.
template <typename Result, size_t Count>
inline bool aw_task_many_impl<Result, Count>::await_ready() const noexcept {
  return false;
}

/// Suspends the outer coroutine, submits the wrapped task to the
/// executor, and waits for it to complete.
template <typename Result, size_t Count>
inline std::coroutine_handle<> __attribute__((always_inline))
aw_task_many_impl<Result, Count>::await_suspend(std::coroutine_handle<> Outer
) noexcept {
  continuation = Outer;
  std::coroutine_handle<> next;
  if (symmetric_task != nullptr) {
    // symmetric transfer to the last task IF it should run immediately
    next = symmetric_task;
  } else {
    // This logic is necessary because we submitted all child tasks before the
    // parent suspended. Allowing parent to be resumed before it suspends would
    // be UB. Therefore we need to block the resumption until here.
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

/// Returns the value provided by the wrapped function.
template <typename Result, size_t Count>
inline aw_task_many_impl<Result, Count>::ResultArray&&
aw_task_many_impl<Result, Count>::await_resume() noexcept {
  return std::move(result);
}

template <size_t Count>
template <typename TaskIter>
inline aw_task_many_impl<void, Count>::aw_task_many_impl(
  TaskIter Iter, size_t TaskCount, detail::type_erased_executor* Executor,
  detail::type_erased_executor* ContinuationExecutor, size_t Prio
)
    : symmetric_task{nullptr}, continuation_executor{ContinuationExecutor},
      done_count{0} {
  using TaskArray = std::conditional_t<
    Count == 0, std::vector<work_item>, std::array<work_item, Count>>;
  TaskArray arr;
  if constexpr (Count == 0) {
    arr.resize(TaskCount);
  }
  const size_t size = arr.size();
  if (size == 0) {
    return;
  }
  bool doSymmetricTransfer =
    Executor == detail::this_thread::executor &&
    Prio <= detail::this_thread::this_task.yield_priority->load(
              std::memory_order_relaxed
            );
  size_t i = 0;
  for (; i < size; ++i) {
    detail::unsafe_task<void> t(detail::into_unsafe_task(*Iter));
    auto& p = t.promise();
    p.continuation = &continuation;
    p.continuation_executor = &continuation_executor;
    p.done_count = &done_count;
    arr[i] = t;
    ++Iter;
  }
  if (doSymmetricTransfer) {
    symmetric_task = detail::unsafe_task<void>::from_address(
      TMC_WORK_ITEM_AS_STD_CORO(arr[i - 1]).address()
    );
  }
  auto postCount = doSymmetricTransfer ? size - 1 : size;
  done_count.store(static_cast<int64_t>(postCount), std::memory_order_release);

  if (postCount != 0) {
    Executor->post_bulk(arr.data(), Prio, postCount);
  }
}

template <size_t Count>
inline bool aw_task_many_impl<void, Count>::await_ready() const noexcept {
  return false;
}

/// Suspends the outer coroutine, submits the wrapped task to the
/// executor, and waits for it to complete.
template <size_t Count>
inline std::coroutine_handle<> __attribute__((always_inline))
aw_task_many_impl<void, Count>::await_suspend(std::coroutine_handle<> Outer
) noexcept {
  continuation = Outer;
  std::coroutine_handle<> next;
  if (symmetric_task != nullptr) {
    // symmetric transfer to the last task IF it should run immediately
    next = symmetric_task;
  } else {
    // This logic is necessary because we submitted all child tasks before the
    // parent suspended. Allowing parent to be resumed before it suspends would
    // be UB. Therefore we need to block the resumption until here.
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
template <size_t Count>
inline void aw_task_many_impl<void, Count>::await_resume() noexcept {}

} // namespace tmc

// some ways to capture transformer iterator instead of entire wrapper
// (and reduce size of this struct)
// causes problems with CTAD; need to split Functor and non-Functor versions
// template <typename InputIter> struct TaskIterTransformer {
//     InputIter task_iter;
//     aw_task_many *me;
//     std::coroutine_handle<> operator()(size_t i) {
//       task<void> t = *task_iter;
//       ++task_iter;
//       auto &p = t.promise();
//       p.continuation = &me->continuation;
//       p.done_count = &me->done_count;
//       p.result_ptr = &me->result[i];
//       return t;
//     }
//   };
// iter = iter_adapter(0, TaskIterTransformer{std::move(task_iter), this});
// iter = [this, task_iter = std::move(task_iter)](
//     size_t i) -> std::coroutine_handle<> {
//   task<void> t = *task_iter;
//   ++task_iter;
//   auto &p = t.promise();
//   p.continuation = &continuation;
//   p.done_count = &done_count;
//   p.result_ptr = &result[i];
//   return t;
// });
