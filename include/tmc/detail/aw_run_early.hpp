#pragma once
#include "tmc/detail/concepts.hpp"
#include "tmc/detail/thread_locals.hpp"
#include "tmc/task.hpp"
#include <cassert>
#include <coroutine>
#include <functional>
#include <mutex>

namespace tmc {
// Forward declared friend classes.
// Defined in "tmc/spawn_task.hpp" / "tmc/spawn_task_many.hpp" which includes
// this header.

/// The customizable task wrapper / awaitable type returned by
/// `tmc::spawn(tmc::task<result_t>)`.
template <typename result_t> class aw_spawned_task;
/// The customizable task wrapper / awaitable type returned by
/// `tmc::spawn_many(tmc::task<result_t>)`.
template <typename result_t, size_t count> class aw_task_many;

/// A wrapper that converts lazy task(s) to eager task(s),
/// and allows the task(s) to be awaited after it has been started.
/// It is created by calling run_early() on a parent awaitable
/// from spawn() or spawn_many().
///
/// `result_t` is the type of a single result value (same as that of the wrapped
/// `task<result_t>`).
///
/// `output_t` may be a `result_t`, `std::vector<result_t>`,
/// or `std::array<result_t, count>` depending on what type of awaitable this
/// was created from.
template <typename result_t, typename output_t>
class [[nodiscard("You must co_await aw_run_early. "
                  "It is not safe to destroy aw_run_early without first "
                  "awaiting it.")]] aw_run_early;

template <IsNotVoid result_t, typename output_t>
class aw_run_early<result_t, output_t> {
  friend class aw_spawned_task<result_t>;
  template <typename R, size_t count> friend class aw_task_many;
  std::coroutine_handle<> continuation;
  detail::type_erased_executor* continuation_executor;
  output_t result;
  std::atomic<int64_t> done_count;

  // Private constructor from aw_spawned_task. Takes ownership of parent's
  // task.
  aw_run_early(
    task<result_t> wrapped, size_t prio, detail::type_erased_executor* executor,
    detail::type_erased_executor* continuation_executor_in
  )
      : continuation{nullptr}, continuation_executor(continuation_executor_in),
        done_count(1) {
    auto& p = wrapped.promise();
    p.continuation = &continuation;
    p.continuation_executor = &continuation_executor;
    p.result_ptr = &result;
    p.done_count = &done_count;
    // TODO fence maybe not required if there's one inside the queue?
    std::atomic_thread_fence(std::memory_order_release);
    executor->post_variant(std::coroutine_handle<>(wrapped), prio);
  }

  // Private constructor from aw_task_many. Takes ownership of parent's tasks.
  // For use when count is runtime dynamic - take ownership of parent's result
  // vector.
  template <size_t count> aw_run_early(aw_task_many<result_t, count>&& parent) {
    continuation_executor = parent.continuation_executor;
    if constexpr (std::is_same_v<output_t, std::vector<result_t>>) {
      result = std::move(parent.result);
    }
    const auto size = parent.wrapped.size();
    for (size_t i = 0; i < size; ++i) {
      auto& p =
        task<result_t>::from_address(parent.wrapped[i].address()).promise();
      p.continuation = &continuation;
      p.continuation_executor = &continuation_executor;
      p.done_count = &done_count;
      p.result_ptr = &result[i];
    }
    done_count.store(size, std::memory_order_release);
    parent.executor->post_bulk(parent.wrapped.data(), parent.prio, size);
    parent.did_await = true;
  }

public:
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
    if (remaining > 0) {
      return true;
    }
    // Resume if remaining <= 0 (worker already finished)
    if (continuation_executor == detail::this_thread::executor) {
      return false;
    } else {
      // Need to resume on a different executor
      continuation_executor->post_variant(
        std::move(outer), detail::this_thread::this_task.prio
      );
      return true;
    }

    return (remaining > 0);
  }

  constexpr output_t& await_resume() & noexcept { return result; }
  constexpr output_t&& await_resume() && noexcept { return std::move(result); }

  // This must be awaited and the child task completed before destruction.
  ~aw_run_early() noexcept { assert(done_count.load() < 0); }

  // Not movable or copyable due to child task being spawned in constructor,
  // and having pointers to this.
  aw_run_early& operator=(const aw_run_early& other) = delete;
  aw_run_early(const aw_run_early& other) = delete;
  aw_run_early& operator=(const aw_run_early&& other) = delete;
  aw_run_early(const aw_run_early&& other) = delete;
};

template <IsVoid result_t, IsVoid output_t>
class aw_run_early<result_t, output_t> {
  friend class aw_spawned_task<result_t>;
  template <typename R, size_t count> friend class aw_task_many;
  std::coroutine_handle<> continuation;
  detail::type_erased_executor* continuation_executor;
  std::atomic<int64_t> done_count;

  // Private constructor from aw_spawned_task. Takes ownership of parent's
  // task.
  aw_run_early(
    task<result_t> wrapped, size_t prio, detail::type_erased_executor* executor,
    detail::type_erased_executor* continuation_executor_in
  )
      : continuation{nullptr}, done_count(1),
        continuation_executor(continuation_executor_in) {
    auto& p = wrapped.promise();
    p.continuation = &continuation;
    p.continuation_executor = &continuation_executor;
    p.done_count = &done_count;
    // TODO fence maybe not required if there's one inside the queue?
    std::atomic_thread_fence(std::memory_order_release);
    executor->post_variant(std::coroutine_handle<>(wrapped), prio);
  }

  // Private constructor from aw_task_many. Takes ownership of parent's tasks.
  template <size_t count> aw_run_early(aw_task_many<result_t, count>&& parent) {
    continuation_executor = parent.continuation_executor;
    const auto size = parent.wrapped.size();
    for (size_t i = 0; i < size; ++i) {
      auto& p =
        task<result_t>::from_address(parent.wrapped[i].address()).promise();
      p.continuation = &continuation;
      p.continuation_executor = &continuation_executor;
      p.done_count = &done_count;
    }
    done_count.store(size, std::memory_order_release);
    parent.executor->post_bulk(parent.wrapped.data(), parent.prio, size);
    parent.did_await = true;
  }

public:
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
    if (remaining > 0) {
      return true;
    }
    // Resume if remaining <= 0 (worker already finished)
    if (continuation_executor == detail::this_thread::executor) {
      return false;
    } else {
      // Need to resume on a different executor
      continuation_executor->post_variant(
        std::move(outer), detail::this_thread::this_task.prio
      );
      return true;
    }
  }

  constexpr void await_resume() const noexcept {}

  // This must be awaited and the child task completed before destruction.
  ~aw_run_early() noexcept { assert(done_count.load() < 0); }

  // Not movable or copyable due to child task being spawned in constructor,
  // and having pointers to this.
  aw_run_early& operator=(const aw_run_early& other) = delete;
  aw_run_early(const aw_run_early& other) = delete;
  aw_run_early& operator=(const aw_run_early&& other) = delete;
  aw_run_early(const aw_run_early&& other) = delete;
};
} // namespace tmc
