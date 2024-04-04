// sync.hpp provides methods for external code to submit work to TMC executors
// and perform a blocking wait for that code to complete.

// Unlike other TMC functions, which require you to commit to waiting or not
// waiting for a value in the caller, the functions in sync.hpp allow you to
// detach or ignore the result of a task at any time. This is to comply with the
// expected behavior of std::future / std::promise, although it does come at a
// small performance penalty.
#pragma once
#include "tmc/task.hpp"
#include "tmc/utils.hpp"
#include <atomic>
#include <coroutine>
#include <future>
#include <memory>

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
template <typename E, typename T, typename R = std::invoke_result_t<T>>
std::future<R> post_waitable(E& Executor, T&& Functor, size_t Priority)
  requires(!std::is_convertible_v<T, std::coroutine_handle<>> && !std::is_convertible_v<R, std::coroutine_handle<>> && !std::is_void_v<R>)
{
  std::promise<R> promise;
  std::future<R> future = promise.get_future();
  post(
    Executor,
    [prom = std::move(promise), func = std::forward<T>(Functor)]() mutable {
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
template <typename E, typename T, typename R = std::invoke_result_t<T>>
std::future<void> post_waitable(E& Executor, T&& Functor, size_t Priority)
  requires(!std::is_convertible_v<T, std::coroutine_handle<>> && std::is_void_v<R>)
{
  std::promise<void> promise;
  std::future<void> future = promise.get_future();
  post(
    Executor,
    [prom = std::move(promise), func = std::forward<T>(Functor)]() mutable {
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
/// Reads `Count` coroutines from `TaskIterator` and submits them to `Executor`
/// for execution at priority `Priority`. The return value is a
/// `std::future<void>` that can be used to poll or blocking wait for all of the
/// tasks to complete.
///
/// Bulk waitables only support void return; if you want to return values,
/// preallocate a result array and capture it into the coroutines.
template <typename E, typename Iter>
std::future<void> post_bulk_waitable(
  E& Executor, Iter TaskIterator, size_t Priority, size_t Count
)
  requires(std::is_convertible_v<std::iter_value_t<Iter>, task<void>>)
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
      TaskIterator,
      [sharedState](Iter iter) mutable -> task<void> {
        task<void> t = *iter;
        auto& p = t.promise();
        p.continuation = &sharedState->continuation;
        p.done_count = &sharedState->done_count;
        p.continuation_executor = &sharedState->continuation_executor;
        return t;
      }
    ),
    Priority, Count
  );
  return sharedState->promise.get_future();
}

// FUNC

/// `Iter` must be an iterator type that exposes `T operator*()` and
/// `Iter& operator++()`.
/// `T` must expose `void operator()`.
/// Reads `Count` functions from `FunctorIterator` and submits the functions to
/// `Executor` for execution at priority `Priority`. The return value is a
/// `std::future<void>` that can be used to poll or blocking wait for the result
/// to be ready.
///
/// Bulk waitables only support void return; if you want to return values,
/// preallocate a result array and capture it into the coroutines.
template <
  typename E, typename Iter, typename T = std::iter_value_t<Iter>,
  typename R = std::invoke_result_t<T>>
std::future<void> post_bulk_waitable(
  E& Executor, Iter FunctorIterator, size_t Priority, size_t Count
)
  requires(!std::is_convertible_v<T, std::coroutine_handle<>> && std::is_void_v<R>)
{
  struct BulkSyncState {
    std::promise<void> promise;
    std::atomic<int64_t> done_count;
  };
  std::shared_ptr<BulkSyncState> sharedState =
    std::make_shared<BulkSyncState>(std::promise<void>(), Count - 1);
#if WORK_ITEM_IS(CORO)
  Executor.post_bulk(
    iter_adapter(
      FunctorIterator,
      [sharedState](Iter iter) mutable -> std::coroutine_handle<> {
        return [](
                 T t, std::shared_ptr<BulkSyncState> sharedState
               ) -> task<void> {
          t();
          if (sharedState->done_count.fetch_sub(1, std::memory_order_acq_rel) == 0) {
            sharedState->promise.set_value();
          }
          co_return;
        }(std::forward<T>(*iter), sharedState);
      }
    ),
    Priority, Count
  );
#else
  Executor.post_bulk(
    iter_adapter(
      FunctorIterator,
      [sharedState](Iter iter) mutable -> auto {
        return [f = *iter, sharedState]() {
          f();
          if (sharedState->done_count.fetch_sub(1, std::memory_order_acq_rel) == 0) {
            sharedState->promise.set_value();
          }
        };
      }
    ),
    Priority, Count
  );
#endif
  return sharedState->promise.get_future();
}

} // namespace tmc
