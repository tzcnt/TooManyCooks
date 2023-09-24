#pragma once
#include "tmc/task.hpp"
#include "tmc/utils.hpp"
#include <atomic>
#include <coroutine>
#include <future>
#include <memory>

namespace tmc {

// CORO
template <typename E, typename R>
std::future<R> post_waitable(E &ex, task<R> coro, size_t prio)
  requires(!std::is_void_v<R>)
{
  std::promise<R> promise;
  std::future<void> future = promise.get_future();
  task<void> tp = [](std::promise<R> promise, task<R> coro) -> task<void> {
    promise.set_value(co_await coro);
  }(std::move(promise), coro.resume_on(ex));
  post(ex, std::coroutine_handle<>(tp), prio);
  return future;
}

template <typename E>
std::future<void> post_waitable(E &ex, task<void> coro, size_t prio) {
  std::promise<void> promise;
  std::future<void> future = promise.get_future();
  task<void> tp = [](std::promise<void> promise,
                     task<void> coro) -> task<void> {
    co_await coro;
    promise.set_value();
  }(std::move(promise), coro.resume_on(ex));
  post(ex, std::coroutine_handle<>(tp), prio);
  return future;
}

// FUNC RETURNING CORO
template <typename E, typename T, typename R>
std::future<R> post_waitable(E &ex, T &&func, size_t prio)
  requires(!std::is_convertible_v<T, std::coroutine_handle<>> &&
           std::is_same_v<std::invoke_result_t<T>, task<R>> &&
           !std::is_void_v<R>)
{
  std::promise<R> promise;
  std::future<void> future = promise.get_future();
  task<void> tp = [](std::promise<R> promise, task<R> coro) -> task<void> {
    promise.set_value(co_await coro);
  }(std::move(promise), func().resume_on(ex));
  post(ex, std::coroutine_handle<>(tp), prio);
  return future;
}

template <typename E, typename T>
std::future<void> post_waitable(E &ex, T &&func, size_t prio)
  requires(!std::is_convertible_v<T, std::coroutine_handle<>> &&
           std::is_same_v<std::invoke_result_t<T>, task<void>>)
{
  std::promise<void> promise;
  std::future<void> future = promise.get_future();
  task<void> tp = [](std::promise<void> promise,
                     task<void> coro) -> task<void> {
    co_await coro;
    promise.set_value();
  }(std::move(promise), func().resume_on(ex));
  post(ex, std::coroutine_handle<>(tp), prio);
  return future;
}

// FUNC - these won't compile with TMC_WORK_ITEM=FUNC
// Because a std::function can't hold a move-only lambda
template <typename E, typename T, typename R = std::invoke_result_t<T>>
std::future<R> post_waitable(E &ex, T &&func, size_t prio)
  requires(!std::is_convertible_v<T, std::coroutine_handle<>> &&
           !std::is_convertible_v<R, std::coroutine_handle<>> &&
           !std::is_void_v<R>)
{
  std::promise<R> promise;
  std::future<void> future = promise.get_future();
  post(
      ex,
      [promise = std::move(promise), func]() mutable {
        promise.set_value(func());
      },
      prio);
  return future;
}

template <typename E, typename T, typename R = std::invoke_result_t<T>>
std::future<void> post_waitable(E &ex, T &&func, size_t prio)
  requires(!std::is_convertible_v<T, std::coroutine_handle<>> &&
           std::is_void_v<R>)
{
  std::promise<void> promise;
  std::future<void> future = promise.get_future();
  post(
      ex,
      [promise = std::move(promise), func]() mutable {
        func();
        promise.set_value();
      },
      prio);
  return future;
}

// Bulk waitables only return void; if you want to return values,
// preallocate an array and capture it into the coroutines
// CORO
template <typename E, typename Iter>
std::future<void> post_bulk_waitable(E &ex, Iter it, size_t prio, size_t count)
  requires(std::is_convertible_v<std::iter_value_t<Iter>, task<void>>)
{
  struct state {
    std::promise<void> promise;
    std::atomic<int64_t> done_count;
    std::coroutine_handle<> continuation;
  };
  std::shared_ptr<state> shared_state =
      std::make_shared<state>(std::promise<void>(), count - 1, nullptr);

  // shared_state will be kept alive until continuation runs
  task<void> tp = [](std::shared_ptr<state> state) -> task<void> {
    state->promise.set_value();
    co_return;
  }(shared_state);
  shared_state->continuation = tp;

  ex.post_bulk(iter_adapter(it,
                            [&ex, shared_state](Iter it) mutable -> task<void> {
                              task<void> t = *it;
                              t.resume_on(ex);
                              auto &p = t.promise();
                              // address of continuation (double dereference) is
                              // needed
                              p.continuation = &shared_state->continuation;
                              p.done_count = &shared_state->done_count;
                              return t;
                            }),
               prio, count);
  return shared_state->promise.get_future();
}

// FUNC RETURNING CORO
template <typename E, typename Iter, typename T = std::iter_value_t<Iter>>
std::future<void> post_bulk_waitable(E &ex, Iter it, size_t prio, size_t count)
  requires(!std::is_convertible_v<T, std::coroutine_handle<>> &&
           std::is_same_v<std::invoke_result_t<T>, task<void>>)
{
  struct state {
    std::promise<void> promise;
    std::atomic<int64_t> done_count;
    std::coroutine_handle<> continuation;
  };
  std::shared_ptr<state> shared_state = std::shared_ptr<state>(
      new state{std::promise<void>(), count - 1, nullptr});

  // shared_state will be kept alive until continuation runs
  task<void> tp = [](std::shared_ptr<state> state) -> task<void> {
    state->promise.set_value();
    co_return;
  }(shared_state);
  shared_state->continuation = tp;

  ex.post_bulk(iter_adapter(it,
                            [&ex, shared_state](Iter it) mutable -> task<void> {
                              task<void> t = (*it)();
                              t.resume_on(ex);
                              auto &p = t.promise();
                              // address of continuation (double dereference) is
                              // needed
                              p.continuation = &shared_state->continuation;
                              p.done_count = &shared_state->done_count;
                              return t;
                            }),
               prio, count);
  return shared_state->promise.get_future();
}

// FUNC
template <typename E, typename Iter, typename T = std::iter_value_t<Iter>,
          typename R = std::invoke_result_t<T>>
std::future<void> post_bulk_waitable(E &ex, Iter it, size_t prio, size_t count)
  requires(!std::is_convertible_v<T, std::coroutine_handle<>> &&
           std::is_void_v<R>)
{
  struct state {
    std::promise<void> promise;
    std::atomic<int64_t> done_count;
  };
  std::shared_ptr<state> shared_state =
      std::make_shared<state>(std::promise<void>(), count - 1);
  ex.post_bulk(iter_adapter(it,
                            [shared_state](Iter it) mutable -> auto {
                              return [f = *it, shared_state]() {
                                f();
                                if (shared_state->done_count.fetch_sub(
                                        1, std::memory_order_acq_rel) == 0) {
                                  shared_state->promise.set_value();
                                }
                              };
                            }),
               prio, count);
  return shared_state->promise.get_future();
}

} // namespace tmc
