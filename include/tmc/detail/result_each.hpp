// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once
#include "tmc/detail/impl.hpp" // IWYU pragma: keep

// Common implementations of .result_each() awaitable functions
// for spawn_many and spawn_tuple

#include <atomic>
#include <coroutine>
#include <cstddef>

namespace tmc {
namespace detail {

TMC_DECL bool result_each_await_suspend(
  ptrdiff_t remaining_count, std::coroutine_handle<> Outer,
  std::coroutine_handle<>& continuation, std::atomic<size_t>& sync_flags
) noexcept;

TMC_DECL size_t result_each_await_resume(
  ptrdiff_t& remaining_count, std::atomic<size_t>& sync_flags
) noexcept;

// Non-suspending variant of result_each_await_resume(). If a result is ready,
// consumes it and returns its slot index. Returns 64 (the end() sentinel) when
// no submitted results remain, or 65 (the none() sentinel) when results are still pending
// but none is ready.
TMC_DECL size_t
result_each_poll(ptrdiff_t& remaining_count, std::atomic<size_t>& sync_flags) noexcept;

} // namespace detail
} // namespace tmc

#if !defined(TMC_STANDALONE_COMPILATION) || defined(TMC_IMPL)
#include "tmc/detail/result_each.ipp"
#endif
