// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/thread_locals.hpp"
#include "tmc/detail/waiter_list.hpp"

#include <cassert>
#include <cstddef>

namespace tmc {
namespace detail {
void waiter_list_node::resume() noexcept {
  tmc::detail::post_checked(
    continuation_executor, std::move(continuation), continuation_priority
  );
}

void waiter_list::add_waiter(tmc::detail::waiter_list_node& w) noexcept {
  auto h = head.load(std::memory_order_acquire);
  do {
    w.next = h;
  } while (!head.compare_exchange_strong(
    h, &w, std::memory_order_acq_rel, std::memory_order_acquire
  ));
}

void waiter_list::wake_all() noexcept {
  auto curr = head.exchange(nullptr, std::memory_order_acq_rel);
  while (curr != nullptr) {
    auto next = curr->next;
    curr->resume();
    curr = next;
  }
}

waiter_list_node* waiter_list::take_all() noexcept {
  return head.exchange(nullptr, std::memory_order_acq_rel);
}

void waiter_list::must_wake_n(size_t n) noexcept {
  auto toWake = head.load(std::memory_order_acquire);
  for (size_t i = 0; i < n; ++i) {
    do {
      // should be guaranteed to see at least wakeCount waiters
      assert(toWake != nullptr);
    } while (!head.compare_exchange_strong(
      toWake, toWake->next, std::memory_order_acq_rel, std::memory_order_acquire
    ));
    toWake->resume();
  }
}
} // namespace detail
} // namespace tmc
