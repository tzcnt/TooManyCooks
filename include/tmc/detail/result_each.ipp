// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#include "tmc/detail/awaitable_customizer.hpp"
#include "tmc/detail/thread_locals.hpp"
#include "tmc/ex_any.hpp"

#include <atomic>
#include <bit>
#include <coroutine>
#include <cstddef>

namespace tmc {
namespace detail {

bool result_each_await_ready(
  ptrdiff_t remaining_count, std::atomic<size_t> const& sync_flags
) noexcept {
  // Always suspends, due to the possibility to resume on another executor.
  return false;
}

bool result_each_await_suspend(
  ptrdiff_t remaining_count, std::coroutine_handle<> Outer,
  std::coroutine_handle<>& continuation, tmc::ex_any* continuation_executor,
  std::atomic<size_t>& sync_flags
) noexcept {
  continuation = Outer;
  if (remaining_count != 0) {
    // This logic is necessary because we submitted all child tasks before the
    // parent suspended. Allowing parent to be resumed before it suspends
    // would be UB. Therefore we need to block the resumption until here.
    // WARNING: We can use fetch_sub here because we know this bit wasn't set.
    // It generates xadd instruction which is slightly more efficient than
    // fetch_and. But not safe to use if the bit might already be set.
    size_t resumeState;
    do {
      resumeState = sync_flags.fetch_sub(
        tmc::detail::task_flags::EACH, std::memory_order_acq_rel
      );
      assert(0 != (resumeState & tmc::detail::task_flags::EACH));
      if (0 == (resumeState & ~tmc::detail::task_flags::EACH)) {
        return true; // we suspended and no tasks were ready
      }
      // A result became ready, so try to resume immediately.
      resumeState = sync_flags.fetch_or(
        tmc::detail::task_flags::EACH, std::memory_order_acq_rel
      );
      if (0 != (resumeState & tmc::detail::task_flags::EACH)) {
        return true; // Another thread already resumed
      }
      // If we resumed, but another thread already consumed
      // all the results, try again to suspend
    } while (0 == (resumeState & ~tmc::detail::task_flags::EACH));
  }
  if (continuation_executor == nullptr ||
      tmc::detail::this_thread::exec_is(continuation_executor)) {
    return false;
  } else {
    // Need to resume on a different executor
    tmc::detail::post_checked(
      continuation_executor, std::move(Outer),
      tmc::detail::this_thread::this_task.prio
    );
    return true;
  }
}

size_t result_each_await_resume(
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
