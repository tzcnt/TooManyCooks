// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/thread_locals.hpp"
#include "tmc/ex_any.hpp"

namespace tmc {

/// Returns the current task's priority.
/// Returns 0 (highest priority) if this thread is not associated with an
/// executor.
inline size_t current_priority() noexcept {
  return tmc::detail::this_thread::this_task.prio;
}

/// Returns a pointer to the current thread's type-erased executor.
/// Returns nullptr if this thread is not associated with an executor.
inline tmc::ex_any* current_executor() noexcept {
  return tmc::detail::this_thread::executor;
}

/// Returns the current thread's index within its executor.
/// Each executor's threads are numbered independently, starting from 0.
/// Returns -1 if this thread is not associated with an executor.
inline size_t current_thread_index() noexcept {
  return tmc::detail::this_thread::thread_index;
}

} // namespace tmc
