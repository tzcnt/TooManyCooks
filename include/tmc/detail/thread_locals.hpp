// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/impl.hpp"

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
#ifdef TMC_WINDOWS_DLL
TMC_DECL extern std::atomic<tmc::ex_any*> g_ex_default;
#ifdef TMC_IMPL
TMC_DECL constinit std::atomic<tmc::ex_any*> g_ex_default = nullptr;
#endif
#else
inline constinit std::atomic<tmc::ex_any*> g_ex_default = nullptr;
#endif

struct running_task_data {
  size_t prio;
  // pointer to single element
  // this is used both for explicit yielding, and checked to determine whether
  // operations may symmetric transfer
  std::atomic<size_t>* yield_priority;
};

#ifdef TMC_WINDOWS_DLL

TMC_DECL extern std::atomic<size_t> never_yield;
#ifdef TMC_IMPL
TMC_DECL constinit std::atomic<size_t> never_yield = TMC_ALL_ONES;
#endif

#else
inline constinit std::atomic<size_t> never_yield = TMC_ALL_ONES;
#endif

namespace this_thread {

#ifdef TMC_WINDOWS_DLL

struct tls_state {
  tmc::ex_any* executor;
  size_t thread_index;
  running_task_data this_task;
  void* producers;
};

TMC_DECL tls_state& tls() noexcept;

#ifdef TMC_IMPL
TMC_DECL
#ifndef __clang__
// MSVC in Release build appears to incorrectly inline this function even when
// it's dllimported, which results in different copies of the TLS data between
// the DLL and consuming code. Clang doesn't have this problem.
__declspec(noinline)
#endif
tls_state&
tls() noexcept {
  static thread_local tls_state s{
    nullptr, TMC_ALL_ONES, {0, &never_yield}, nullptr
  };
  return s;
}
#endif

inline tmc::ex_any*& executor() noexcept { return tls().executor; }
inline size_t& thread_index() noexcept { return tls().thread_index; }
inline running_task_data& this_task() noexcept { return tls().this_task; }
inline void*& producers() noexcept { return tls().producers; }

#else // !TMC_WINDOWS_DLL

inline tmc::ex_any*& executor() noexcept {
  static constinit thread_local tmc::ex_any* val = nullptr;
  return val;
}
inline size_t& thread_index() noexcept {
  static constinit thread_local size_t val = TMC_ALL_ONES;
  return val;
}
inline running_task_data& this_task() noexcept {
  static constinit thread_local running_task_data val = {0, &never_yield};
  return val;
}
inline void*& producers() noexcept {
  static constinit thread_local void* val = nullptr;
  return val;
}

#endif // TMC_WINDOWS_DLL

// Used by awaiters. If the awaitable has already completed, this should be
// checked to determine whether we are on the correct executor to resume inline.
// Checking priority is not necessary since the awaiter's priority cannot
// be modified.
inline bool exec_is(ex_any const* const Executor) noexcept {
  return Executor == executor();
}

inline bool prio_is(size_t const Priority) noexcept {
  return Priority == this_task().prio;
}

// Used by awaitables. When the awaitable completes, it must check both the
// executor and priority to determing if it can resume inline. This is because a
// child awaitable's priority may be different from the awaiter's.
inline bool
exec_prio_is(ex_any const* const Executor, size_t const Priority) noexcept {
  return Executor == executor() && Priority == this_task().prio;
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
