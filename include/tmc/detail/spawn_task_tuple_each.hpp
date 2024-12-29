// Copyright (c) 2023-2024 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/aw_run_early.hpp"
#include "tmc/detail/concepts.hpp" // IWYU pragma: keep
#include "tmc/detail/thread_locals.hpp"
#include "tmc/task.hpp"

#include <cassert>
#include <coroutine>
#include <tuple>
#include <type_traits>
#include <variant>

namespace tmc {
namespace detail {
// Replace void with std::monostate (void is not a valid tuple element type)
template <typename T>
using void_to_monostate =
  std::conditional_t<std::is_void_v<T>, std::monostate, T>;

// Get the last type of a parameter pack
// In C++26 you can use pack indexing instead: T...[sizeof...(T) - 1]
template <typename... T> struct last_type {
  using type = typename decltype((std::type_identity<T>{}, ...))::type;
};

template <> struct last_type<> {
  // workaround for empty tuples - the task object will be void / empty
  using type = void;
};

template <typename... T> using last_type_t = last_type<T...>::type;
} // namespace detail

template <typename... Result> class aw_spawned_task_tuple_each_impl {
  static constexpr auto Count = sizeof...(Result);
  // This class uses an atomic bitmask with only 63 slots for tasks.
  // each() doesn't seem like a good fit for larger task groups anyway.
  // If you really need this, please open a GitHub issue explaining why...
  static_assert(Count < 64);

  std::coroutine_handle<> continuation;
  detail::type_erased_executor* continuation_executor;
  std::atomic<uint64_t> sync_flags;
  int64_t remaining_count;
  using result_tuple = std::tuple<detail::void_to_monostate<Result>...>;
  result_tuple result;
  friend aw_spawned_task_tuple<Result...>;

  template <typename T>
  TMC_FORCE_INLINE inline void prepare_task(
    detail::unsafe_task<T> Task, detail::void_to_monostate<T>* TaskResult,
    size_t I, work_item& Task_out
  ) {
    auto& p = Task.promise();
    p.continuation = &continuation;
    p.continuation_executor = &continuation_executor;
    p.done_count = &sync_flags;
    if constexpr (!std::is_void_v<T>) {
      p.result_ptr = TaskResult;
    }
    p.flags = detail::task_flags::EACH | I;
    Task_out = Task;
  }

  aw_spawned_task_tuple_each_impl(
    std::tuple<task<Result>...>&& Tasks, detail::type_erased_executor* Executor,
    detail::type_erased_executor* ContinuationExecutor, size_t Prio
  )
      : continuation_executor{ContinuationExecutor}, sync_flags{0},
        remaining_count{0} {
    if (Count == 0) {
      return;
    }
    std::array<work_item, Count> taskArr;

    // Prepare each task as if I loops from [0..Count),
    // but using compile-time indexes and types.
    [&]<size_t... I>(std::index_sequence<I...>) {
      ((prepare_task(
         detail::unsafe_task<Result>(std::get<I>(std::move(Tasks))),
         &std::get<I>(result), I, taskArr[I]
       )),
       ...);
    }(std::make_index_sequence<Count>{});

    remaining_count = Count;
    sync_flags.store(detail::task_flags::EACH, std::memory_order_release);

    if (Count != 0) {
      detail::post_bulk_checked(Executor, taskArr.data(), Count, Prio);
    }
  }

public:
  /// Always suspends.
  inline bool await_ready() const noexcept {
    if (remaining_count == 0) {
      return true;
    }
    auto resumeState = sync_flags.load(std::memory_order_acquire);
    // High bit is set, because we are running
    assert((resumeState & detail::task_flags::EACH) != 0);
    auto readyBits = resumeState & ~detail::task_flags::EACH;
    return readyBits != 0;
  }

  /// Suspends the outer coroutine, submits the wrapped task to the
  /// executor, and waits for it to complete.
  TMC_FORCE_INLINE inline bool await_suspend(std::coroutine_handle<> Outer
  ) noexcept {
    continuation = Outer;
  // This logic is necessary because we submitted all child tasks before the
  // parent suspended. Allowing parent to be resumed before it suspends
  // would be UB. Therefore we need to block the resumption until here.
  // WARNING: We can use fetch_sub here because we know this bit wasn't set.
  // It generates xadd instruction which is slightly more efficient than
  // fetch_or. But not safe to use if the bit might already be set.
  TRY_SUSPEND:
    auto resumeState =
      sync_flags.fetch_sub(detail::task_flags::EACH, std::memory_order_acq_rel);
    assert((resumeState & detail::task_flags::EACH) != 0);
    auto readyBits = resumeState & ~detail::task_flags::EACH;
    if (readyBits == 0) {
      return true; // we suspended and no tasks were ready
    }
    // A result became ready, so try to resume immediately.
    auto resumeState2 =
      sync_flags.fetch_or(detail::task_flags::EACH, std::memory_order_acq_rel);
    bool didResume = (resumeState2 & detail::task_flags::EACH) == 0;
    if (!didResume) {
      return true; // Another thread already resumed
    }
    auto readyBits2 = resumeState2 & ~detail::task_flags::EACH;
    if (readyBits2 == 0) {
      // We resumed but another thread already consumed all the results
      goto TRY_SUSPEND;
    }
    if (continuation_executor != nullptr &&
        !detail::this_thread::exec_is(continuation_executor)) {
      // Need to resume on a different executor
      detail::post_checked(
        continuation_executor, std::move(Outer),
        detail::this_thread::this_task.prio
      );
      return true;
    }
    return false; // OK to resume inline
  }

  /// Returns the value provided by the wrapped tasks.
  /// Each task has a slot in the tuple. If the task would return void, its
  /// slot is represented by a std::monostate.
  inline size_t await_resume() noexcept {
    if (remaining_count == 0) {
      return end();
    }
    uint64_t resumeState = sync_flags.load(std::memory_order_acquire);
    assert((resumeState & detail::task_flags::EACH) != 0);
    // High bit is set, because we are resuming
    uint64_t slots = resumeState & ~detail::task_flags::EACH;
    assert(slots != 0);
#ifdef _MSC_VER
    size_t slot = static_cast<size_t>(_tzcnt_u64(slots));
#else
    size_t slot = static_cast<size_t>(__builtin_ctzll(slots));
#endif
    --remaining_count;
    sync_flags.fetch_sub(1ULL << slot, std::memory_order_release);
    return slot;
  }

  /// Provides a sentinel value that can be compared against the value returned
  /// from co_await.
  inline size_t end() noexcept { return Count + 1; }

  // Gets the ready result at the given index.
  template <size_t I>
  inline std::tuple_element_t<I, result_tuple>& get() noexcept {
    return std::get<I>(result);
  }
};

template <typename... Result>
using aw_spawned_task_tuple_each = aw_spawned_task_tuple_each_impl<Result...>;

} // namespace tmc
