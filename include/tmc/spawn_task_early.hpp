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
template <typename result_t> struct aw_early_task;

template <IsNotVoid result_t> struct aw_early_task<result_t> {
  // don't actually store handle as we can't interact with it after posting
  // task<result_t> handle;
  std::coroutine_handle<> continuation;
  result_t result;
  std::atomic<int64_t> done_count;

  aw_early_task(task<result_t> wrapped, size_t prio,
                detail::type_erased_executor *executor)
      : continuation{nullptr}, done_count(1) {
    auto &p = wrapped.promise();
    p.continuation = &continuation;
    p.result_ptr = &result;
    p.done_count = &done_count;
    // TODO fence maybe not required if there's one inside the queue?
    std::atomic_thread_fence(std::memory_order_release);
    executor->post_variant(std::coroutine_handle<>(wrapped), prio);
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

  constexpr result_t &await_resume() & noexcept { return result; }
  constexpr result_t &&await_resume() && noexcept { return std::move(result); }
};

template <IsVoid result_t> struct aw_early_task<result_t> {
  // don't actually store handle as we can't interact with it after posting
  // task<result_t> handle;
  std::coroutine_handle<> continuation;
  std::atomic<int64_t> done_count;

  aw_early_task(task<result_t> wrapped, size_t prio,
                detail::type_erased_executor *executor)
      : continuation{nullptr}, done_count(1) {
    auto &p = wrapped.promise();
    p.continuation = &continuation;
    p.done_count = &done_count;
    // TODO fence maybe not required if there's one inside the queue?
    std::atomic_thread_fence(std::memory_order_release);
    executor->post_variant(std::coroutine_handle<>(wrapped), prio);
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

  constexpr void await_resume() const noexcept {}
};
} // namespace tmc
