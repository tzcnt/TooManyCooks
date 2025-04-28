// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

// Common implementations of .result_each() awaitable functions
// for spawn_many and spawn_tuple

#include "tmc/ex_any.hpp"

#include <atomic>
#include <coroutine>
#include <cstddef>

namespace tmc {
namespace detail {

bool result_each_await_ready(
  ptrdiff_t remaining_count, std::atomic<size_t> const& sync_flags
) noexcept;

bool result_each_await_suspend(
  ptrdiff_t remaining_count, std::coroutine_handle<> Outer,
  std::coroutine_handle<>& continuation, tmc::ex_any* continuation_executor,
  std::atomic<size_t>& sync_flags
) noexcept;

size_t result_each_await_resume(
  ptrdiff_t& remaining_count, std::atomic<size_t>& sync_flags
) noexcept;

} // namespace detail
} // namespace tmc

#ifdef TMC_IMPL
#include "tmc/detail/result_each.ipp"
#endif
