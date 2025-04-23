// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

// Common implementations of .result_each() awaitable functions
// for spawn_many and spawn_tuple

#include "tmc/detail/awaitable_customizer.hpp"

#include <atomic>
#include <bit>
#include <cstddef>

namespace tmc {
namespace detail {

inline bool each_result_await_ready(
  ptrdiff_t remaining_count, std::atomic<size_t> const& sync_flags
) noexcept {
  if (remaining_count == 0) {
    return true;
  }
  auto resumeState = sync_flags.load(std::memory_order_acquire);
  // High bit is set, because we are running
  assert((resumeState & tmc::detail::task_flags::EACH) != 0);
  auto readyBits = resumeState & ~tmc::detail::task_flags::EACH;
  return readyBits != 0;
}

inline bool each_result_await_suspend(
  std::coroutine_handle<> Outer, std::coroutine_handle<>& continuation,
  tmc::ex_any* continuation_executor, std::atomic<size_t>& sync_flags
) noexcept {
  continuation = Outer;
// This logic is necessary because we submitted all child tasks before the
// parent suspended. Allowing parent to be resumed before it suspends
// would be UB. Therefore we need to block the resumption until here.
// WARNING: We can use fetch_sub here because we know this bit wasn't set.
// It generates xadd instruction which is slightly more efficient than
// fetch_or. But not safe to use if the bit might already be set.
TRY_SUSPEND:
  auto resumeState = sync_flags.fetch_sub(
    tmc::detail::task_flags::EACH, std::memory_order_acq_rel
  );
  assert((resumeState & tmc::detail::task_flags::EACH) != 0);
  auto readyBits = resumeState & ~tmc::detail::task_flags::EACH;
  if (readyBits == 0) {
    return true; // we suspended and no tasks were ready
  }
  // A result became ready, so try to resume immediately.
  auto resumeState2 = sync_flags.fetch_or(
    tmc::detail::task_flags::EACH, std::memory_order_acq_rel
  );
  bool didResume = (resumeState2 & tmc::detail::task_flags::EACH) == 0;
  if (!didResume) {
    return true; // Another thread already resumed
  }
  auto readyBits2 = resumeState2 & ~tmc::detail::task_flags::EACH;
  if (readyBits2 == 0) {
    // We resumed but another thread already consumed all the results
    goto TRY_SUSPEND;
  }
  if (continuation_executor != nullptr &&
      !tmc::detail::this_thread::exec_is(continuation_executor)) {
    // Need to resume on a different executor
    tmc::detail::post_checked(
      continuation_executor, std::move(Outer),
      tmc::detail::this_thread::this_task.prio
    );
    return true;
  }
  return false; // OK to resume inline
}

inline size_t each_result_await_resume(
  ptrdiff_t& remaining_count, std::atomic<size_t>& sync_flags
) noexcept {
  if (remaining_count == 0) {
    return 64;
  }
  size_t resumeState = sync_flags.load(std::memory_order_acquire);
  assert((resumeState & tmc::detail::task_flags::EACH) != 0);
  // High bit is set, because we are resuming
  size_t slots = resumeState & ~tmc::detail::task_flags::EACH;
  assert(slots != 0);
  size_t slot = std::countr_zero(slots);
  --remaining_count;
  sync_flags.fetch_sub(TMC_ONE_BIT << slot, std::memory_order_release);
  return slot;
}

} // namespace detail
} // namespace tmc
