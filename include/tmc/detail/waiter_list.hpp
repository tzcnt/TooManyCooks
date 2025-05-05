// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/ex_any.hpp"

#include <coroutine>
#include <cstddef>

namespace tmc {
namespace detail {
struct waiter_list_node {
  waiter_list_node* next;
  std::coroutine_handle<> continuation;
  tmc::ex_any* continuation_executor;
  size_t continuation_priority;

  // Submits this to the executor to be resumed
  void resume() noexcept;

  // If this expects to resume on the same executor and priority as the Outer
  // task, then returns this and posts the outer task to the exectutor. If this
  // expects to resume on a different executor or priority, then posts this to
  // its executor and returns the outer task.
  [[nodiscard]] std::coroutine_handle<>
  try_symmetric_transfer(std::coroutine_handle<> Outer) noexcept;
};

class waiter_list {
  std::atomic<waiter_list_node*> head;

public:
  waiter_list() : head{nullptr} {}

  // Adds w to list. Head becomes w.
  void add_waiter(waiter_list_node& w) noexcept;

  // Wakes all waiters. Head becomes nullptr.
  void wake_all() noexcept;

  // Returns head. Head becomes nullptr.
  [[nodiscard]] waiter_list_node* take_all() noexcept;

  /// Assumes at least n waiters are in the list.
  /// Wakes n waiters.
  void must_wake_n(size_t n) noexcept;

  /// Assumes at least 1 waiters is in the list.
  /// Takes 1 waiter.
  [[nodiscard]] waiter_list_node* must_take_1() noexcept;
};

} // namespace detail
} // namespace tmc

#ifdef TMC_IMPL
#include "tmc/detail/waiter_list.ipp"
#endif
