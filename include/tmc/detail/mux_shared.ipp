// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/awaitable_customizer.hpp"
#include "tmc/detail/compat.hpp"
#include "tmc/detail/impl.hpp" // IWYU pragma: keep
#include "tmc/detail/mux_shared.hpp"

#include <atomic>
#include <bit>
#include <coroutine>
#include <cstddef>

namespace tmc {
namespace detail {

bool mux_await_suspend(
  ptrdiff_t remaining_count, std::coroutine_handle<> Outer,
  std::coroutine_handle<>& continuation, std::atomic<size_t>& sync_flags
) noexcept {
  continuation = Outer;
  if (remaining_count == 0) {
    return false;
  }
  // Children are submitted before the parent suspends. Allowing the parent to
  // be resumed before it suspends would be UB, so children are blocked from
  // resuming it by the EACH bit (lock bit), which is held for the entire time
  // the parent is running. It may only be released here, atomically with the
  // decision to suspend, and only if no results are already ready.
  // This release must be the final operation via CAS rather than speculatively, because
  // once released, another thread may resume this and destroy the coroutine frame.
  size_t state = sync_flags.load(std::memory_order_acquire);
  while (true) {
    assert(0 != (state & tmc::detail::task_flags::EACH));
    if (0 != (state & ~tmc::detail::task_flags::EACH)) {
      // A result is ready. Resume immediately. The EACH bit was never
      // released, so no child could have claimed the resumption.
      return false;
    }
    if (sync_flags.compare_exchange_weak(
          state, state & ~tmc::detail::task_flags::EACH, std::memory_order_acq_rel,
          std::memory_order_acquire
        )) {
      return true;
    }
    // CAS failure means a child just set its result bit; retry to consume it.
  }
}

size_t
mux_await_resume(ptrdiff_t& remaining_count, std::atomic<size_t>& sync_flags) noexcept {
  if (remaining_count == 0) {
    return TMC_PLATFORM_BITS;
  }
  size_t resumeState = sync_flags.load(std::memory_order_acquire);
  assert((resumeState & tmc::detail::task_flags::EACH) != 0);
  // High bit is set, because we are resuming
  size_t slots = resumeState & ~tmc::detail::task_flags::EACH;
  assert(slots != 0);
  size_t slot = static_cast<size_t>(std::countr_zero(slots));
  --remaining_count;
  sync_flags.fetch_sub(TMC_ONE_BIT << slot, std::memory_order_release);
  return slot;
}

size_t mux_poll(ptrdiff_t& remaining_count, std::atomic<size_t>& sync_flags) noexcept {
  if (remaining_count == 0) {
    // No submitted results remain: same terminal value as await_resume(),
    // which the caller compares against end().
    return TMC_PLATFORM_BITS;
  }
  // The owner holds the EACH (lock) bit while it is running, so children that
  // complete concurrently set their result bit but cannot resume us. The
  // acquire load synchronizes-with the child's release-store of that bit,
  // making its result visible.
  size_t state = sync_flags.load(std::memory_order_acquire);
  assert((state & tmc::detail::task_flags::EACH) != 0);
  size_t slots = state & ~tmc::detail::task_flags::EACH;
  if (slots == 0) {
    // Results are still pending, but none is ready yet.
    return TMC_PLATFORM_BITS + 1;
  }
  size_t slot = static_cast<size_t>(std::countr_zero(slots));
  --remaining_count;
  sync_flags.fetch_sub(TMC_ONE_BIT << slot, std::memory_order_release);
  return slot;
}

} // namespace detail
} // namespace tmc
