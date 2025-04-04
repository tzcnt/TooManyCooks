// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/compat.hpp"

#include <atomic>
#include <cassert>
#include <limits>

// Macro hackery to enable defines TMC_WORK_ITEM=CORO / TMC_WORK_ITEM=FUNC, etc
// CORO will be the default if undefined
#ifndef TMC_WORK_ITEM
#define TMC_WORK_ITEM CORO
#endif

#define TMC_WORK_ITEM_CORO 0
#define TMC_WORK_ITEM_FUNC 1
#define TMC_WORK_ITEM_FUNCORO 2
#define TMC_CONCAT_impl(a, b) a##b
#define TMC_CONCAT(a, b) TMC_CONCAT_impl(a, b)
#define TMC_WORK_ITEM_IS_impl(WORK_ITEM_TYPE)                                  \
  TMC_CONCAT(TMC_WORK_ITEM_, TMC_WORK_ITEM) ==                                 \
    TMC_CONCAT(TMC_WORK_ITEM_, WORK_ITEM_TYPE)
#define TMC_WORK_ITEM_IS(WORK_ITEM_TYPE) TMC_WORK_ITEM_IS_impl(WORK_ITEM_TYPE)

#if TMC_WORK_ITEM_IS(CORO)
#include <coroutine>
namespace tmc {
using work_item = std::coroutine_handle<>;
}
#define TMC_WORK_ITEM_AS_STD_CORO(x) (x)
#elif TMC_WORK_ITEM_IS(FUNC)
#include <functional>
namespace tmc {
using work_item = std::function<void()>;
}
#define TMC_WORK_ITEM_AS_STD_CORO(x)                                           \
  (*x.template target<std::coroutine_handle<>>())
#elif TMC_WORK_ITEM_IS(FUNCORO)
#include "tmc/detail/coro_functor.hpp"
namespace tmc {
using work_item = tmc::coro_functor;
}
#define TMC_WORK_ITEM_AS_STD_CORO(x) (x.as_coroutine())
#endif

namespace tmc {

// A type-erased executor that may represent any kind of TMC executor.
class ex_any {
public:
  // Pointers to the real executor and its function implementations.
  void* executor;
  void (*s_post)(
    void* Erased, work_item&& Item, size_t Priority, size_t ThreadHint
  );
  void (*s_post_bulk)(
    void* Erased, work_item* Items, size_t Count, size_t Priority,
    size_t ThreadHint
  );

  // API functions that delegate to the real executor.
  inline void
  post(work_item&& Item, size_t Priority = 0, size_t ThreadHint = NO_HINT) {
    s_post(executor, std::move(Item), Priority, ThreadHint);
  }
  inline void post_bulk(
    work_item* Items, size_t Count, size_t Priority = 0,
    size_t ThreadHint = NO_HINT
  ) {
    s_post_bulk(executor, Items, Count, Priority, ThreadHint);
  }

  // A default constructor is offered so that other executors can initialize
  // this with their own function pointers.
  ex_any() {}

  // This constructor is used by TMC executors.
  template <typename T> ex_any(T* Executor) {
    executor = Executor;
    s_post =
      [](void* Erased, work_item&& Item, size_t Priority, size_t ThreadHint) {
        static_cast<T*>(Erased)->post(std::move(Item), Priority, ThreadHint);
      };
    s_post_bulk = [](
                    void* Erased, work_item* Items, size_t Count,
                    size_t Priority, size_t ThreadHint
                  ) {
      static_cast<T*>(Erased)->post_bulk(Items, Count, Priority, ThreadHint);
    };
  }
};
namespace detail {

// The default executor that is used by post_checked / post_bulk_checked
// when the current (non-TMC) thread's executor == nullptr.
// Its value can be populated by calling tmc::external::set_default_executor().
inline constinit std::atomic<tmc::ex_any*> g_ex_default = nullptr;

inline std::atomic<size_t> never_yield = std::numeric_limits<size_t>::max();
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
inline bool exec_is(ex_any const* const Executor) {
  return Executor == executor;
}
inline bool prio_is(size_t const Priority) {
  return Priority == this_task.prio;
}

} // namespace this_thread

inline void post_checked(
  tmc::ex_any* executor, work_item&& Item, size_t Priority = 0,
  size_t ThreadHint = NO_HINT
) {
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
) {
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

/// Returns a pointer to the current thread's type-erased executor.
/// Returns nullptr if this thread is not associated with an executor.
inline tmc::ex_any* current_executor() {
  return tmc::detail::this_thread::executor;
}

/// Returns the current thread's index within its executor.
/// Returns -1 if this thread is not associated with an executor.
inline size_t current_thread_index() {
  return tmc::detail::this_thread::thread_index;
}

/// Returns the current task's priority.
/// Returns 0 (highest priority) if this thread is not associated with an
/// executor.
inline size_t current_priority() {
  return tmc::detail::this_thread::this_task.prio;
}

} // namespace tmc
