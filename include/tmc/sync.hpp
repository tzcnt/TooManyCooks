// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

// sync.hpp provides methods for external code to submit work to TMC executors
// and perform a blocking wait for that code to complete.

// Unlike other TMC functions, which require you to commit to waiting or not
// waiting for a value in the caller, the functions in sync.hpp allow you to
// detach or ignore the result of a task at any time. This is to comply with the
// expected behavior of std::future / std::promise, although it does come at a
// small performance penalty.

#include "tmc/detail/compat.hpp"
#include "tmc/detail/concepts_awaitable.hpp"
#include "tmc/detail/concepts_work_item.hpp"
#include "tmc/ex_any.hpp"
#include "tmc/task.hpp"
#include "tmc/utils.hpp"
#include "tmc/work_item.hpp"

#include <atomic>
#include <coroutine>
#include <future>
#include <memory>
#include <vector>

namespace tmc {
/// Submits `Work` for execution on `Executor` at priority `Priority`. Tasks or
/// functors that return values cannot be submitted this way; see
/// `post_waitable` instead.
template <typename E, typename TaskOrFunc>
void post(
  E&& Executor, TaskOrFunc&& Work, size_t Priority = 0,
  size_t ThreadHint = NO_HINT
) noexcept
  requires(
    tmc::detail::is_task_void_v<TaskOrFunc> ||
    tmc::detail::is_func_void_v<TaskOrFunc>
  )
{
  if constexpr (std::is_convertible_v<TaskOrFunc, work_item>) {
    tmc::detail::get_executor_traits<E>::post(
      Executor, work_item(static_cast<TaskOrFunc&&>(Work)), Priority, ThreadHint
    );
  } else {
    tmc::detail::get_executor_traits<E>::post(
      Executor, tmc::detail::into_work_item(static_cast<TaskOrFunc&&>(Work)),
      Priority, ThreadHint
    );
  }
}

// CORO
/// Submits `Task` to `Executor` for execution at priority `Priority`.
/// The return value is a `std::future<Result>` that can be used to poll or
/// blocking wait for the result to be ready.
template <typename E, typename Result>
[[nodiscard]] std::future<Result> post_waitable(
  E&& Executor, task<Result>&& Task, size_t Priority = 0,
  size_t ThreadHint = NO_HINT
)
  requires(!std::is_void_v<Result>)
{
  std::promise<Result> promise;
  std::future<Result> future = promise.get_future();
  task<void> tp =
    [](std::promise<Result> Promise, task<Result> InnerTask) -> task<void> {
    Promise.set_value(co_await std::move(InnerTask));
  }(std::move(promise), std::move(Task).resume_on(Executor));
  post(Executor, std::move(tp), Priority, ThreadHint);
  return future;
}

/// Submits `Task` to `Executor` for execution at priority `Priority`.
/// The return value is a `std::future<void>` that can be used to poll or
/// blocking wait for the task to complete.
template <typename E>
[[nodiscard]] std::future<void> post_waitable(
  E&& Executor, task<void>&& Task, size_t Priority = 0,
  size_t ThreadHint = NO_HINT
) {
  std::promise<void> promise;
  std::future<void> future = promise.get_future();
  task<void> tp =
    [](std::promise<void> Promise, task<void> InnerTask) -> task<void> {
    co_await std::move(InnerTask);
    Promise.set_value();
  }(std::move(promise), std::move(Task).resume_on(Executor));
  post(Executor, std::move(tp), Priority, ThreadHint);
  return future;
}

// FUNC - these won't compile with TMC_WORK_ITEM=FUNC
// Because a std::function can't hold a move-only lambda

/// Given a functor that returns a value `Result`, this submits `Functor` to
/// `Executor` for execution at priority `Priority`. The return value is a
/// `std::future<Result>` that can be used to poll or blocking wait for the
/// result to be ready.
template <
  typename E, typename FuncResult,
  typename Result = std::invoke_result_t<FuncResult>>
[[nodiscard]] std::future<Result> post_waitable(
  E&& Executor, FuncResult&& Func, size_t Priority = 0,
  size_t ThreadHint = NO_HINT
)
  requires(
    !std::is_void_v<Result> && tmc::detail::is_func_result_v<FuncResult, Result>
  )
{
  static_assert(
    !requires { typename std::coroutine_traits<Result>::promise_type; },
    "You passed an unevaluated coroutine function - this is probably a bug. "
    "You should call the function before passing to post_waitable."
  );

#if TMC_WORK_ITEM_IS(FUNC)
  std::promise<Result>* promise = new std::promise<Result>();
  std::future<Result> future = promise->get_future();
  post(
    Executor,
    // TODO keep lvalue reference to func, but move rvalue func to new value
    // https://stackoverflow.com/a/29324846
    [prom = promise, func = static_cast<FuncResult&&>(Func)]() mutable {
      prom->set_value(func());
      delete prom;
    },
    Priority, ThreadHint
  );
#else
  std::promise<Result> promise;
  std::future<Result> future = promise.get_future();
  post(
    Executor,
    // TODO keep lvalue reference to func, but move rvalue func to new value
    // https://stackoverflow.com/a/29324846
    [prom = std::move(promise),
     func = static_cast<FuncResult&&>(Func)]() mutable {
      prom.set_value(func());
    },
    Priority, ThreadHint
  );
#endif
  return future;
}

/// Given a functor that returns `void`, this submits `Functor` to `Executor`
/// for execution at priority `Priority`. The return value is a
/// `std::future<void>` that can be used to poll or blocking wait for the task
/// to complete.
template <typename E, typename FuncVoid>
[[nodiscard]] std::future<void> post_waitable(
  E&& Executor, FuncVoid&& Func, size_t Priority = 0,
  size_t ThreadHint = NO_HINT
)
  requires(tmc::detail::is_func_void_v<FuncVoid>)
{
#if TMC_WORK_ITEM_IS(FUNC)
  std::promise<void>* promise = new std::promise<void>();
  std::future<void> future = promise->get_future();
  post(
    Executor,
    // TODO keep lvalue reference to func, but move rvalue func to new value
    // https://stackoverflow.com/a/29324846
    [prom = std::move(promise),
     func = static_cast<FuncVoid&&>(Func)]() mutable {
      func();
      prom->set_value();
      delete prom;
    },
    Priority, ThreadHint
  );
#else
  std::promise<void> promise;
  std::future<void> future = promise.get_future();
  post(
    Executor,
    // TODO keep lvalue reference to func, but move rvalue func to new value
    // https://stackoverflow.com/a/29324846
    [prom = std::move(promise),
     func = static_cast<FuncVoid&&>(Func)]() mutable {
      func();
      prom.set_value();
    },
    Priority, ThreadHint
  );
#endif
  return future;
}

// CORO

/// `Iter` must be an iterator type that exposes `task<void> operator*()` and
/// `Iter& operator++()`.
///
/// Submits items in range [Begin, Begin + Count) to the executor at priority
/// `Priority`. The return value is a `std::future<void>` that can be used to
/// poll or blocking wait for all of the tasks to complete.
///
/// Bulk waitables only support void return; if you want to return values,
/// preallocate a result array and capture a reference to it in your tasks.
template <
  typename E, typename TaskIter, typename Task = std::iter_value_t<TaskIter>>
[[nodiscard]] std::future<void> post_bulk_waitable(
  E&& Executor, TaskIter&& Begin, size_t Count, size_t Priority = 0,
  size_t ThreadHint = NO_HINT
)
  requires(tmc::detail::is_task_void_v<Task>)
{
  if (Count == 0) {
    std::promise<void> p;
    auto f = p.get_future();
    p.set_value();
    return f;
  }
  struct BulkSyncState {
    std::promise<void> promise;
    std::atomic<ptrdiff_t> done_count;
    std::coroutine_handle<> continuation;
    tmc::ex_any* continuation_executor;
  };
  std::shared_ptr<BulkSyncState> sharedState =
    std::make_shared<BulkSyncState>(std::promise<void>(), Count - 1, nullptr);

  // shared_state will be kept alive until continuation runs
  sharedState->continuation =
    [](std::shared_ptr<BulkSyncState> State) -> task<void> {
    State->promise.set_value();
    co_return;
  }(sharedState);
  if constexpr (requires {
                  tmc::detail::get_executor_traits<E>::type_erased(Executor);
                }) {
    sharedState->continuation_executor =
      tmc::detail::get_executor_traits<E>::type_erased(Executor);
  } else {
    sharedState->continuation_executor = Executor;
  }

  tmc::detail::get_executor_traits<E>::post_bulk(
    Executor,
    iter_adapter(
      static_cast<TaskIter&&>(Begin),
      [sharedState](TaskIter iter) mutable -> task<void> {
        TMC_DISABLE_WARNING_PESSIMIZING_MOVE_BEGIN
        task<void> t = std::move(*iter);
        TMC_DISABLE_WARNING_PESSIMIZING_MOVE_END
        tmc::detail::get_awaitable_traits<task<void>>::set_continuation(
          t, &sharedState->continuation
        );
        tmc::detail::get_awaitable_traits<task<void>>::set_done_count(
          t, &sharedState->done_count
        );
        tmc::detail::get_awaitable_traits<task<void>>::
          set_continuation_executor(t, &sharedState->continuation_executor);
        return t;
      }
    ),
    Count, Priority, ThreadHint
  );
  return sharedState->promise.get_future();
}

/// `Iter` must be an iterator type that exposes `task<void> operator*()` and
/// `Iter& operator++()`.
///
/// Submits items in range [Begin, End) to the executor at priority `Priority`.
/// The return value is a `std::future<void>` that can be used to poll or
/// blocking wait for all of the tasks to complete.
///
/// Bulk waitables only support void return; if you want to return values,
/// preallocate a result array and capture a reference to it in your tasks.
template <
  typename E, typename TaskIter, typename Task = std::iter_value_t<TaskIter>>
[[nodiscard]] std::future<void> post_bulk_waitable(
  E&& Executor, TaskIter&& Begin, TaskIter&& End, size_t Priority = 0,
  size_t ThreadHint = NO_HINT
)
  requires(tmc::detail::is_task_void_v<Task>)
{
  if constexpr (requires(TaskIter a, TaskIter b) { a - b; }) {
    size_t count = static_cast<size_t>(End - Begin);
    return post_bulk_waitable(
      Executor, static_cast<TaskIter&&>(Begin), count, Priority, ThreadHint
    );
  } else {
    std::vector<tmc::task<void>> tasks;
    while (Begin != End) {
      tasks.emplace_back(*Begin);
      ++Begin;
    }
    return post_bulk_waitable(
      Executor, tasks.begin(), tasks.size(), Priority, ThreadHint
    );
  }
}

/// `TaskRange` must implement `begin()` and `end()` functions which return
/// an iterator type. The iterator type must implement `task<void> operator*()`
/// and `Iter& operator++()`.
///
/// Submits items in range [Range.begin(), Range.end()) to the executor at
/// priority `Priority`. The return value is a `std::future<void>` that can be
/// used to poll or blocking wait for all of the tasks to complete.
///
/// Bulk waitables only support void return; if you want to return values,
/// preallocate a result array and capture a reference to it in your tasks.
template <
  typename E, typename TaskRange,
  typename TaskIter = tmc::detail::range_iter<TaskRange>::type,
  typename Task = std::iter_value_t<TaskIter>>
[[nodiscard]] std::future<void> post_bulk_waitable(
  E&& Executor, TaskRange&& Range, size_t Priority = 0,
  size_t ThreadHint = NO_HINT
)
  requires(tmc::detail::is_task_void_v<Task>)
{
  return post_bulk_waitable(
    Executor, static_cast<TaskRange&&>(Range).begin(),
    static_cast<TaskRange&&>(Range).end(), Priority, ThreadHint
  );
}

// FUNC

/// `FuncIter` must be an iterator type that exposes `Functor operator*()` and
/// `FuncIter& operator++()`.
/// `Functor` must expose `void operator()`.
/// Submits items in range [Begin, Begin + Count) to the executor at priority
/// `Priority`. The return value is a `std::future<void>` that can be used to
/// poll or blocking wait for the result to be ready.
///
/// Bulk waitables only support void return; if you want to return values,
/// preallocate a result array and capture a reference to it in your tasks.
template <
  typename E, typename FuncIter, typename Functor = std::iter_value_t<FuncIter>>
[[nodiscard]] std::future<void> post_bulk_waitable(
  E&& Executor, FuncIter&& Begin, size_t Count, size_t Priority = 0,
  size_t ThreadHint = NO_HINT
)
  requires(tmc::detail::is_func_void_v<Functor>)
{
  if (Count == 0) {
    std::promise<void> p;
    auto f = p.get_future();
    p.set_value();
    return f;
  }
  struct BulkSyncState {
    std::promise<void> promise;
    std::atomic<ptrdiff_t> done_count;
  };
  std::shared_ptr<BulkSyncState> sharedState =
    std::make_shared<BulkSyncState>(std::promise<void>(), Count - 1);
#if TMC_WORK_ITEM_IS(CORO)
  TMC_DISABLE_WARNING_PESSIMIZING_MOVE_BEGIN
  tmc::detail::get_executor_traits<E>::post_bulk(
    Executor,
    iter_adapter(
      static_cast<FuncIter&&>(Begin),
      [sharedState](FuncIter iter) mutable -> std::coroutine_handle<> {
        return [](
                 Functor t, std::shared_ptr<BulkSyncState> SharedState
               ) -> task<void> {
          t();
          if (SharedState->done_count.fetch_sub(1, std::memory_order_acq_rel) ==
              0) {
            SharedState->promise.set_value();
          }
          co_return;
        }(std::move(*iter), sharedState);
      }
    ),
    Count, Priority, ThreadHint
  );
  TMC_DISABLE_WARNING_PESSIMIZING_MOVE_END
#else
  tmc::detail::get_executor_traits<E>::post_bulk(
    Executor,
    iter_adapter(
      static_cast<FuncIter&&>(Begin),
      [sharedState](FuncIter iter) mutable -> auto {
        return [f = std::move(*iter), sharedState]() mutable {
          f();
          if (sharedState->done_count.fetch_sub(1, std::memory_order_acq_rel) ==
              0) {
            sharedState->promise.set_value();
          }
        };
      }
    ),
    Count, Priority, ThreadHint
  );
#endif
  return sharedState->promise.get_future();
}

/// `FuncIter` must be an iterator type that exposes `Functor operator*()` and
/// `FuncIter& operator++()`.
/// `Functor` must expose `void operator()`.
///
/// Submits items in range [Begin, End) to the executor at priority `Priority`.
/// The return value is a `std::future<void>` that can be used to poll or
/// blocking wait for the result to be ready.
///
/// Bulk waitables only support void return; if you want to return values,
/// preallocate a result array and capture a reference to it in your tasks.
template <
  typename E, typename FuncIter, typename Functor = std::iter_value_t<FuncIter>>
// TODO implement this for iterators and ranges
[[nodiscard]] std::future<void> post_bulk_waitable(
  E&& Executor, FuncIter&& Begin, FuncIter&& End, size_t Priority = 0,
  size_t ThreadHint = NO_HINT
)
  requires(tmc::detail::is_func_void_v<Functor>)
{
  if constexpr (requires(FuncIter a, FuncIter b) { a - b; }) {
    size_t count = static_cast<size_t>(End - Begin);
    return post_bulk_waitable(
      Executor, static_cast<FuncIter&&>(Begin), count, Priority, ThreadHint
    );
  } else {
    std::vector<Functor> tasks;
    while (Begin != End) {
      tasks.emplace_back(*Begin);
      ++Begin;
    }
    return post_bulk_waitable(
      Executor, tasks.begin(), tasks.size(), Priority, ThreadHint
    );
  }
}

/// `TaskRange` must implement `begin()` and `end()` functions which return
/// an iterator type. The iterator type must implement `task<void> operator*()`
/// and `Iter& operator++()`.
///
/// Submits items in range [Range.begin(), Range.end()) to the executor at
/// priority `Priority`. The return value is a `std::future<void>` that can be
/// used to poll or blocking wait for all of the tasks to complete.
///
/// Bulk waitables only support void return; if you want to return values,
/// preallocate a result array and capture a reference to it in your tasks.
template <
  typename E, typename FuncRange,
  typename FuncIter = tmc::detail::range_iter<FuncRange>::type,
  typename Functor = std::iter_value_t<FuncIter>>
[[nodiscard]] std::future<void> post_bulk_waitable(
  E&& Executor, FuncRange&& Range, size_t Priority = 0,
  size_t ThreadHint = NO_HINT
)
  requires(tmc::detail::is_func_void_v<Functor>)
{
  return post_bulk_waitable(
    Executor, static_cast<FuncRange&&>(Range).begin(),
    static_cast<FuncRange&&>(Range).end(), Priority, ThreadHint
  );
}

/// `Iter` must be an iterator type that implements `operator*()` and
/// `Iter& operator++()`.
/// The type of the items in `Iter` must be `task<void>` or a type
/// implementing `void operator()`.
///
/// Submits items in range [Begin, Begin + Count) to the executor at priority
/// `Priority`.
template <
  typename E, typename Iter, typename TaskOrFunc = std::iter_value_t<Iter>>
void post_bulk(
  E&& Executor, Iter&& Begin, size_t Count, size_t Priority = 0,
  size_t ThreadHint = NO_HINT
)
  requires(
    tmc::detail::is_task_void_v<TaskOrFunc> ||
    tmc::detail::is_func_void_v<TaskOrFunc>
  )
{
  if (Count == 0) {
    return;
  }
  if constexpr (std::is_convertible_v<TaskOrFunc, work_item>) {
    tmc::detail::get_executor_traits<E>::post_bulk(
      Executor, static_cast<Iter&&>(Begin), Count, Priority, ThreadHint
    );
  } else {
    tmc::detail::get_executor_traits<E>::post_bulk(
      Executor,
      tmc::iter_adapter(
        static_cast<Iter&&>(Begin),
        [](Iter& it) -> work_item { return tmc::detail::into_work_item(*it); }
      ),
      Count, Priority, ThreadHint
    );
  }
}

/// `Iter` must be an iterator type that implements `operator*()` and
/// `Iter& operator++()`.
/// The type of the items in `Iter` must be `task<void>` or a type
/// implementing `void operator()`.
///
/// Submits items in range [Begin, End) to the executor at priority `Priority`.
template <
  typename E, typename Iter, typename TaskOrFunc = std::iter_value_t<Iter>>
void post_bulk(
  E&& Executor, Iter&& Begin, Iter&& End, size_t Priority = 0,
  size_t ThreadHint = NO_HINT
)
  requires(
    tmc::detail::is_task_void_v<TaskOrFunc> ||
    tmc::detail::is_func_void_v<TaskOrFunc>
  )
{
  if constexpr (requires(Iter a, Iter b) { a - b; }) {
    size_t count = static_cast<size_t>(End - Begin);
    post_bulk(
      Executor, static_cast<Iter&&>(Begin), count, Priority, ThreadHint
    );
  } else {
    std::vector<work_item> tasks;
    while (Begin != End) {
      tasks.emplace_back(tmc::detail::into_work_item(*Begin));
      ++Begin;
    }
    tmc::detail::get_executor_traits<E>::post_bulk(
      Executor, tasks.begin(), tasks.size(), Priority, ThreadHint
    );
  }
}

/// `WorkItemRange` must implement `begin()` and `end()` functions which return
/// an iterator type. The iterator type must implement `operator*()` and
/// `Iter& operator++()`.
/// The type of the items in `Iter` must be `task<void>` or a type
/// implementing `void operator()`.
///
/// Submits items in range [Range.begin(), Range.end()) to the executor at
/// priority `Priority`.
template <
  typename E, typename WorkItemRange,
  typename Iter = tmc::detail::range_iter<WorkItemRange>::type,
  typename TaskOrFunc = std::iter_value_t<Iter>>
void post_bulk(
  E&& Executor, WorkItemRange&& Range, size_t Priority = 0,
  size_t ThreadHint = NO_HINT
)
  requires(
    tmc::detail::is_task_void_v<TaskOrFunc> ||
    tmc::detail::is_func_void_v<TaskOrFunc>
  )
{
  tmc::post_bulk<E&&, Iter, TaskOrFunc>(
    static_cast<E&&>(Executor), static_cast<WorkItemRange&&>(Range).begin(),
    static_cast<WorkItemRange&&>(Range).end(), Priority, ThreadHint
  );
}

} // namespace tmc
