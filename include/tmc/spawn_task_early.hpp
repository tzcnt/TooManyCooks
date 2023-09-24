#pragma once
#include "tmc/detail/concepts.hpp"
#include "tmc/spawn_task_many.hpp"
#include "tmc/task.hpp"
#include <atomic>
#include <cassert>
#include <coroutine>
#include <exception>
#include <functional>

namespace tmc {

// A wrapper that converts a lazy task to an eager task,
// and allows the task to be awaited after it has been started.
template <typename result_t, typename inner> struct aw_early_task;

template <IsNotVoid result_t> struct aw_early_task<result_t, task<result_t>> {
  // don't actually store handle as we can't interact with it after posting
  // task<result_t> handle;
  std::coroutine_handle<> continuation;
  result_t result;
  std::atomic<int64_t> done_count;

  // Parameter `handle_in` must be moved into this, as it is no longer safe
  // to interact with normally after it has been eagerly started.
  // TODO require && here and implement task move constructor
  aw_early_task(task<result_t> handle_in, size_t prio)
      : continuation{nullptr}, done_count(1) {
    auto &p = handle_in.promise();
    p.continuation = &continuation;
    p.result_ptr = &result;
    p.done_count = &done_count;
    // TODO fence maybe not required if there's one inside the queue?
    std::atomic_thread_fence(std::memory_order_release);
    detail::this_thread::executor->post_variant(
        std::coroutine_handle<>(handle_in), prio);
  }
  // not movable or copyable due to spawned task pointing to this's variables
  aw_early_task &operator=(const aw_early_task &other) = delete;
  aw_early_task(const aw_early_task &other) = delete;
  aw_early_task &operator=(const aw_early_task &&other) = delete;
  aw_early_task(const aw_early_task &&other) = delete;

  constexpr bool await_ready() noexcept {
    // not safe to decrement done_count in this
    // as soon as done_count is decremented, this might be resumed
    // thus it must be suspended beforehand
    return false;
    // this works but isn't really helpful
    // return done_count.load(std::memory_order_acq_rel) <= 0;
  }
  constexpr bool await_suspend(std::coroutine_handle<> outer) noexcept {
    // For unknown reasons, it doesn't work to start with done_count at 0,
    // Then increment here and check before storing continuation...
    continuation = outer;
    auto remaining = done_count.fetch_sub(1, std::memory_order_acq_rel);
    // Worker was already posted.
    // Suspend if remaining > 0 (worker is still running)
    // Resume immediately if remaining <= 0 (worker already finished)
    return (remaining > 0);
  }
  // don't move result as running_task may be awaited more than once
  // (eventually) this is safe to do from a single task, but not from multiple
  // tasks
  constexpr result_t await_resume() const noexcept { return result; }
};

template <IsVoid result_t> struct aw_early_task<result_t, task<result_t>> {
  // don't actually store handle as we can't interact with it after posting
  // task<result_t> handle;
  std::coroutine_handle<> continuation;
  std::atomic<int64_t> done_count;

  // Parameter `handle_in` must be moved into this, as it is no longer safe
  // to interact with normally after it has been eagerly started.
  // TODO require && here and implement task move constructor
  aw_early_task(task<result_t> handle_in, size_t prio)
      : continuation{nullptr}, done_count(1) {
    auto &p = handle_in.promise();
    p.continuation = &continuation;
    p.done_count = &done_count;
    // TODO fence maybe not required if there's one inside the queue?
    std::atomic_thread_fence(std::memory_order_release);
    detail::this_thread::executor->post_variant(
        std::coroutine_handle<>(handle_in), prio);
  }
  // not movable or copyable due to spawned task pointing to this's variables
  aw_early_task &operator=(const aw_early_task &other) = delete;
  aw_early_task(const aw_early_task &other) = delete;
  aw_early_task &operator=(const aw_early_task &&other) = delete;
  aw_early_task(const aw_early_task &&other) = delete;

  constexpr bool await_ready() noexcept {
    // not safe to decrement done_count in this
    // as soon as done_count is decremented, this might be resumed
    // thus it must be suspended beforehand
    return false;
    // this works but isn't really helpful
    // return done_count.load(std::memory_order_acq_rel) <= 0;
  }
  constexpr bool await_suspend(std::coroutine_handle<> outer) noexcept {
    // For unknown reasons, it doesn't work to start with done_count at 0,
    // Then increment here and check before storing continuation...
    continuation = outer;
    auto remaining = done_count.fetch_sub(1, std::memory_order_acq_rel);
    // Worker was already posted.
    // Suspend if remaining > 0 (worker is still running)
    // Resume immediately if remaining <= 0 (worker already finished)
    return (remaining > 0);
  }
  // don't move result as running_task may be awaited more than once
  // (eventually) this is safe to do from a single task, but not from multiple
  // tasks
  constexpr void await_resume() const noexcept {}
};

// Parameter `t` must be moved into this, as it is no longer safe
// to interact with normally after it has been eagerly started.
// TODO require && here and implement task move constructor
template <typename result_t>
[[nodiscard("You must co_await the return of "
            "spawn_early(). It is not safe to destroy aw_early_task prior to "
            "awaiting it.")]] aw_early_task<result_t, task<result_t>>
spawn_early(task<result_t> t) {
  // This works only as long as GCE is applied - run_task cannot be moved
  return aw_early_task<result_t, task<result_t>>(
      std::move(t), detail::this_thread::this_task.prio);
}

// Parameter `t` must be moved into this, as it is no longer safe
// to interact with normally after it has been eagerly started.
// TODO require && here and implement task move constructor
template <typename result_t>
[[nodiscard("You must co_await the return of "
            "spawn_early(). It is not safe to destroy aw_early_task prior to "
            "awaiting it.")]] aw_early_task<result_t, task<result_t>>
spawn_early(task<result_t> t, size_t prio) {
  // This works only as long as GCE is applied - run_task cannot be moved
  return aw_early_task<result_t, task<result_t>>(std::move(t), prio);
}
} // namespace tmc
