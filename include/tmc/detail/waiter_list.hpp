// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/thread_locals.hpp"
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

  inline void resume() {
    tmc::detail::post_checked(
      continuation_executor, std::move(continuation), continuation_priority
    );
  }
};
} // namespace detail
} // namespace tmc
