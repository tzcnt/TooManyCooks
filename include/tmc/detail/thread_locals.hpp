// Copyright (c) 2023-2025 Logan McDougall
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
#ifdef _WIN32
extern std::atomic<tmc::ex_any*> g_ex_default;
#ifdef TMC_IMPL
constinit std::atomic<tmc::ex_any*> g_ex_default = nullptr;
#endif
#else
inline constinit std::atomic<tmc::ex_any*> g_ex_default = nullptr;
#endif

// See
// https://developercommunity.visualstudio.com/t/inline-variables-are-not-useful-in-dll-due-to-c249/1262233
// for full info about how to setup dllexport / dllimport
// However it turns out thread_local variables don't work correctly in DLLs
// anyway which is why I didn't finish this

#ifdef _WIN32
extern std::atomic<size_t> never_yield;
#ifdef TMC_IMPL
constinit std::atomic<size_t> never_yield = TMC_ALL_ONES;
#endif
#else
inline constinit std::atomic<size_t> never_yield = TMC_ALL_ONES;
#endif

struct running_task_data {
  size_t prio;
  // pointer to single element
  // this is used both for explicit yielding, and checked to determine whether
  // operations may symmetric transfer
  std::atomic<size_t>* yield_priority;
};

namespace this_thread { // namespace reserved for thread_local variables

#ifdef _WIN32
extern thread_local tmc::ex_any* executor;
extern thread_local size_t thread_index;
extern thread_local running_task_data this_task;
extern thread_local void* producers;
#ifdef TMC_IMPL
constinit thread_local tmc::ex_any* executor = nullptr;
constinit thread_local size_t thread_index = TMC_ALL_ONES;
constinit thread_local running_task_data this_task = {0, &never_yield};
constinit thread_local void* producers = nullptr;
#endif
#else
inline constinit thread_local tmc::ex_any* executor = nullptr;
inline constinit thread_local size_t thread_index = TMC_ALL_ONES;
inline constinit thread_local running_task_data this_task = {0, &never_yield};
inline constinit thread_local void* producers = nullptr;
#endif

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
