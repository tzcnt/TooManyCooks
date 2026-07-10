// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once
#include "tmc/detail/impl.hpp" // IWYU pragma: keep

// Common implementations of awaitable functions for mux_many and mux_tuple

#include <atomic>
#include <coroutine>
#include <cstddef>

namespace tmc {
namespace detail {

TMC_DECL bool mux_await_suspend(
  ptrdiff_t remaining_count, std::coroutine_handle<> Outer,
  std::coroutine_handle<>& continuation, std::atomic<size_t>& sync_flags
) noexcept;

TMC_DECL size_t
mux_await_resume(ptrdiff_t& remaining_count, std::atomic<size_t>& sync_flags) noexcept;

TMC_DECL size_t
mux_poll(ptrdiff_t& remaining_count, std::atomic<size_t>& sync_flags) noexcept;

} // namespace detail
} // namespace tmc

#if !defined(TMC_STANDALONE_COMPILATION) || defined(TMC_IMPL)
#include "tmc/detail/mux_shared.ipp"
#endif
