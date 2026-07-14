// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/impl.hpp" // IWYU pragma: keep

#include "tmc/barrier.hpp"
#include "tmc/detail/thread_locals.hpp"
#include "tmc/detail/waiter_list.hpp"

#include <atomic>
#include <coroutine>

namespace tmc {
bool aw_barrier::await_suspend(std::coroutine_handle<> Outer) noexcept {
  // Configure this awaiter and add it to the waiter list
  parent.waiters.configure_and_add_waiter(me, Outer);

  // Note: We don't run the risk of use-after-resume-and-destroy, because of the
  // invariant that the number of waiters equals the initial count.

  // Decrement and check the barrier count
  auto remaining = parent.done_count.fetch_sub(1, std::memory_order_acq_rel);
  if (remaining > 0) {
    return true;
  }

  // If the waiters are resumed before this is reset, they may immediately
  // await this again, and resume immediately. To prevent this, we must get the
  // list of waiters to be resumed, then set this to the not-ready state,
  // then resume the waiters.

  // Get the waiters
  auto curr = parent.waiters.take_all();

  // Reset this
  parent.done_count = parent.start_count.load();

  // This coroutine resumes by symmetric transfer (returning false), so unlink
  // `me` from the chain before waking the others in batches. `me` is
  // guaranteed to be in the chain since it was added above.
  tmc::detail::waiter_list_node head;
  head.next = curr;
  auto prev = &head;
  while (prev->next != &me) {
    prev = prev->next;
  }
  prev->next = me.next;

  tmc::detail::wake_waiters_in_batches(head.next);
  return false;
}

barrier::~barrier() { waiters.wake_all(); }
} // namespace tmc
