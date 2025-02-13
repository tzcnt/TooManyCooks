// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include <atomic>
#include <cassert>
#include <limits>

// Macro hackery to enable defines TMC_WORK_ITEM=CORO / TMC_WORK_ITEM=FUNC, etc
#define TMC_WORK_ITEM_CORO 0 // coro will be the default if undefined
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
namespace detail {

class type_erased_executor;

// The default executor that is used by post_checked / post_bulk_checked
// when the current (non-TMC) thread's executor == nullptr.
// Its value can be populated by calling tmc::external::set_default_executor().
inline constinit std::atomic<type_erased_executor*> g_ex_default = nullptr;

class type_erased_executor {
public:
  void* executor;
  void (*s_post)(void* Erased, work_item&& Item, size_t Priority);
  void (*s_post_bulk)(
    void* Erased, work_item* Items, size_t Count, size_t Priority
  );
  inline void post(work_item&& Item, size_t Priority) {
    s_post(executor, std::move(Item), Priority);
  }
  inline void post_bulk(work_item* Items, size_t Count, size_t Priority) {
    s_post_bulk(executor, Items, Count, Priority);
  }

  // A default constructor is offered so that other executors can initialize
  // this with their own function pointers.
  type_erased_executor() {}

  // This constructor is used by TMC executors.
  template <typename T> type_erased_executor(T* Executor) {
    executor = Executor;
    s_post = [](void* Erased, work_item&& Item, size_t Priority) {
      static_cast<T*>(Erased)->post(std::move(Item), Priority);
    };
    s_post_bulk =
      [](void* Erased, work_item* Items, size_t Count, size_t Priority) {
        static_cast<T*>(Erased)->post_bulk(Items, Count, Priority);
      };
  }
};

inline std::atomic<size_t> never_yield = std::numeric_limits<size_t>::max();
struct running_task_data {
  size_t prio;
  // pointer to single element
  // this is used both for yielding, and for determining whether spawn_many
  // tasks may symmetric transfer
  std::atomic<size_t>* yield_priority;
};
namespace this_thread { // namespace reserved for thread_local variables
inline constinit thread_local type_erased_executor* executor = nullptr;
inline constinit thread_local running_task_data this_task = {0, &never_yield};
inline constinit thread_local void* producers = nullptr;
inline bool exec_is(type_erased_executor const* const Executor) {
  return Executor == executor;
}
inline bool prio_is(size_t const Priority) {
  return Priority == this_task.prio;
}

} // namespace this_thread

inline void post_checked(
  tmc::detail::type_erased_executor* executor, work_item&& Item, size_t Priority
) {
  if (executor == nullptr) {
    executor = g_ex_default.load(std::memory_order_acquire);
  }
  assert(
    executor != nullptr && "either submit work from a TMC thread or call "
                           "set_default_executor() beforehand"
  );
  executor->post(std::move(Item), Priority);
}
inline void post_bulk_checked(
  tmc::detail::type_erased_executor* executor, work_item* Items, size_t Count,
  size_t Priority
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
  executor->post_bulk(Items, Count, Priority);
}

} // namespace detail
} // namespace tmc
