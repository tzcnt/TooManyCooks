// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/compat.hpp"
#include "tmc/ex_any.hpp"
#include "tmc/work_item.hpp"

#include <atomic>
#include <cassert>

namespace tmc {
namespace detail {

// The default executor that is used by post_checked / post_bulk_checked
// when the current (non-TMC) thread's executor == nullptr.
// Its value can be populated by calling tmc::set_default_executor().
inline constinit std::atomic<tmc::ex_any*> g_ex_default = nullptr;

inline std::atomic<size_t> never_yield = TMC_ALL_ONES;
struct running_task_data {
  size_t prio;
  // pointer to single element
  // this is used both for explicit yielding, and checked to determine whether
  // operations may symmetric transfer
  std::atomic<size_t>* yield_priority;
};

namespace this_thread { // namespace reserved for thread_local variables
inline constinit thread_local tmc::ex_any* executor = nullptr;
inline constinit thread_local size_t thread_index = TMC_ALL_ONES;
inline constinit thread_local running_task_data this_task = {0, &never_yield};
inline constinit thread_local void* producers = nullptr;

inline bool exec_is(ex_any const* const Executor) noexcept {
  return Executor == executor;
}
inline bool prio_is(size_t const Priority) noexcept {
  return Priority == this_task.prio;
}
inline bool
exec_prio_is(ex_any const* const Executor, size_t const Priority) noexcept {
  return Executor == executor && Priority == this_task.prio;
}

} // namespace this_thread

inline void post_checked(
  tmc::ex_any* executor, work_item&& Item, size_t Priority = 0,
  size_t ThreadHint = NO_HINT
) noexcept {
  if (executor == nullptr) {
    executor = g_ex_default.load(std::memory_order_acquire);
  }
  assert(
    executor != nullptr && "either submit work from a TMC thread or call "
                           "set_default_executor() beforehand"
  );
  executor->post(std::move(Item), Priority, ThreadHint);
}
inline void post_bulk_checked(
  tmc::ex_any* executor, work_item* Items, size_t Count, size_t Priority = 0,
  size_t ThreadHint = NO_HINT
) noexcept {
  if (Count == 0) {
    return;
  }
  if (executor == nullptr) {
    executor = g_ex_default.load(std::memory_order_acquire);
  }
  assert(
    executor != nullptr && "either submit work from a TMC thread or call "
                           "set_default_executor() beforehand"
  );
  executor->post_bulk(Items, Count, Priority, ThreadHint);
}

} // namespace detail
} // namespace tmc
