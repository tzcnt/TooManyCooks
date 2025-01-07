// Copyright (c) 2023-2024 Logan McDougall
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

#include "tmc/task.hpp"
#include "tmc/utils.hpp"

#include <atomic>
#include <coroutine>
#include <future>
#include <memory>
#include <vector>

namespace tmc {

// CORO
/// Submits `Task` to `Executor` for execution at priority `Priority`.
/// The return value is a `std::future<R>` that can be used to poll or blocking
/// wait for the result to be ready.
template <typename E, typename R>
std::future<R> post_waitable(E& Executor, task<R>&& Task, size_t Priority)
  requires(!std::is_void_v<R>)
{
  std::promise<R> promise;
  std::future<R> future = promise.get_future();
  task<void> tp = [](std::promise<R> Promise, task<R> InnerTask) -> task<void> {
    Promise.set_value(co_await std::move(InnerTask));
  }(std::move(promise), std::move(Task.resume_on(Executor)));
  post(Executor, std::move(tp), Priority);
  return future;
}

/// Submits `Task` to `Executor` for execution at priority `Priority`.
/// The return value is a `std::future<void>` that can be used to poll or
/// blocking wait for the task to complete.
template <typename E>
std::future<void>
post_waitable(E& Executor, task<void>&& Task, size_t Priority) {
  std::promise<void> promise;
  std::future<void> future = promise.get_future();
  task<void> tp =
    [](std::promise<void> Promise, task<void> InnerTask) -> task<void> {
    co_await std::move(InnerTask);
    Promise.set_value();
  }(std::move(promise), std::move(Task.resume_on(Executor)));
  post(Executor, std::move(tp), Priority);
  return future;
}

// FUNC - these won't compile with TMC_WORK_ITEM=FUNC
// Because a std::function can't hold a move-only lambda

/// Given a functor that returns a value `R`, this:
/// Submits `Functor` to `Executor` for execution at priority `Priority`.
/// The return value is a `std::future<R>` that can be used to poll or blocking
/// wait for the result to be ready.
template <typename E, typename F, typename R = std::invoke_result_t<F>>
std::future<R> post_waitable(E& Executor, F&& Functor, size_t Priority)
  requires(!std::is_void_v<R> && tmc::detail::is_func_result_v<F, R>)
{
  std::promise<R> promise;
  std::future<R> future = promise.get_future();
  post(
    Executor,
    // TODO keep lvalue reference to func, but move rvalue func to new value
    // https://stackoverflow.com/a/29324846
    [prom = std::move(promise), func = static_cast<F&&>(Functor)]() mutable {
      prom.set_value(func());
    },
    Priority
  );
  return future;
}

/// Given a functor that returns `void`, this:
/// Submits `Functor` to `Executor` for execution at priority `Priority`.
/// The return value is a `std::future<void>` that can be used to poll or
/// blocking wait for the task to complete.
template <typename E, typename F>
std::future<void> post_waitable(E& Executor, F&& Functor, size_t Priority)
  requires(tmc::detail::is_func_void_v<F>)
{
  std::promise<void> promise;
  std::future<void> future = promise.get_future();
  post(
    Executor,
    // TODO keep lvalue reference to func, but move rvalue func to new value
    // https://stackoverflow.com/a/29324846
    [prom = std::move(promise), func = static_cast<F&&>(Functor)]() mutable {
      func();
      prom.set_value();
    },
    Priority
  );
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
std::future<void>
post_bulk_waitable(E& Executor, TaskIter&& Begin, size_t Count, size_t Priority)
  requires(tmc::detail::is_task_void_v<Task>)
{
  struct BulkSyncState {
    std::promise<void> promise;
    std::atomic<int64_t> done_count;
    std::coroutine_handle<> continuation;
    tmc::detail::type_erased_executor* continuation_executor;
  };
  std::shared_ptr<BulkSyncState> sharedState =
    std::make_shared<BulkSyncState>(std::promise<void>(), Count - 1, nullptr);

  // shared_state will be kept alive until continuation runs
  sharedState->continuation = [](std::shared_ptr<BulkSyncState> State
                              ) -> task<void> {
    State->promise.set_value();
    co_return;
  }(sharedState);
  if constexpr (requires { Executor.type_erased(); }) {
    sharedState->continuation_executor = Executor.type_erased();
  } else {
    sharedState->continuation_executor = Executor;
  }

  Executor.post_bulk(
    iter_adapter(
      std::forward<TaskIter>(Begin),
      [sharedState](TaskIter iter) mutable -> task<void> {
        task<void> t = *iter;
        tmc::detail::awaitable_traits<task<void>>::set_continuation(
          t, &sharedState->continuation
        );
        tmc::detail::awaitable_traits<task<void>>::set_done_count(
          t, &sharedState->done_count
        );
        tmc::detail::awaitable_traits<task<void>>::set_continuation_executor(
          t, &sharedState->continuation_executor
        );
        return t;
      }
    ),
    Count, Priority
  );
  return sharedState->promise.get_future();
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
std::future<void>
post_bulk_waitable(E& Executor, FuncIter&& Begin, size_t Count, size_t Priority)
  requires(tmc::detail::is_func_void_v<Functor>)
{
  struct BulkSyncState {
    std::promise<void> promise;
    std::atomic<int64_t> done_count;
  };
  std::shared_ptr<BulkSyncState> sharedState =
    std::make_shared<BulkSyncState>(std::promise<void>(), Count - 1);
#if TMC_WORK_ITEM_IS(CORO)
  Executor.post_bulk(
    iter_adapter(
      std::forward<FuncIter>(Begin),
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
        }(*iter, sharedState);
      }
    ),
    Count, Priority
  );
#else
  Executor.post_bulk(
    iter_adapter(
      std::forward<FuncIter>(Begin),
      [sharedState](FuncIter iter) mutable -> auto {
        return [f = *iter, sharedState]() {
          f();
          if (sharedState->done_count.fetch_sub(1, std::memory_order_acq_rel) ==
              0) {
            sharedState->promise.set_value();
          }
        };
      }
    ),
    Count, Priority
  );
#endif
  return sharedState->promise.get_future();
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
void post_bulk(E& Executor, Iter&& Begin, size_t Count, size_t Priority)
  requires(tmc::detail::is_task_void_v<TaskOrFunc> || tmc::detail::is_func_void_v<TaskOrFunc>)
{
  if constexpr (std::is_convertible_v<TaskOrFunc, work_item>) {
    Executor.post_bulk(std::forward<Iter>(Begin), Count, Priority);
  } else {
    Executor.post_bulk(
      tmc::iter_adapter(
        std::forward<Iter>(Begin),
        [](Iter& it) -> work_item { return tmc::detail::into_work_item(*it); }
      ),
      Count, Priority
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
void post_bulk(E& Executor, Iter&& Begin, Iter&& End, size_t Priority)
  requires(tmc::detail::is_task_void_v<TaskOrFunc> || tmc::detail::is_func_void_v<TaskOrFunc>)
{
  if constexpr (requires(Iter a, Iter b) { a - b; }) {
    size_t Count = End - Begin;
    if constexpr (std::is_convertible_v<TaskOrFunc, work_item>) {
      Executor.post_bulk(std::forward<Iter>(Begin), Count, Priority);
    } else {
      Executor.post_bulk(
        tmc::iter_adapter(
          std::forward<Iter>(Begin),
          [](Iter& it) -> work_item { return tmc::detail::into_work_item(*it); }
        ),
        Count, Priority
      );
    }
  } else {
    std::vector<work_item> tasks;
    while (Begin != End) {
      tasks.emplace_back(tmc::detail::into_work_item(*Begin));
      ++Begin;
    }
    Executor.post_bulk(tasks.begin(), tasks.size(), Priority);
  }
}

} // namespace tmc
