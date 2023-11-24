#pragma once
#include <atomic>
#include <cassert>
#include <mutex>
#include <string>
#include <type_traits>

// Macro hackery to enable defines TMC_WORK_ITEM=CORO / TMC_WORK_ITEM=FUNC, etc
#define TMC_WORK_ITEM_CORO 0 // coro will be the default if undefined
#define TMC_WORK_ITEM_FUNC 1
#define TMC_WORK_ITEM_FUNCORO 2
#define TMC_WORK_ITEM_FUNCORO32 3
#define CONCAT_impl(a, b) a##b
#define CONCAT(a, b) CONCAT_impl(a, b)
#define WORK_ITEM_IS_impl(WORK_ITEM_TYPE)                                      \
  CONCAT(TMC_WORK_ITEM_, TMC_WORK_ITEM) ==                                     \
    CONCAT(TMC_WORK_ITEM_, WORK_ITEM_TYPE)
#define WORK_ITEM_IS(WORK_ITEM_TYPE) WORK_ITEM_IS_impl(WORK_ITEM_TYPE)

#if WORK_ITEM_IS(CORO)
#include <coroutine>
namespace tmc {
using work_item = std::coroutine_handle<>;
}
#elif WORK_ITEM_IS(FUNC)
#include <functional>
namespace tmc {
using work_item = std::function<void()>;
}
#elif WORK_ITEM_IS(FUNCORO)
#include "tmc/detail/coro_functor.hpp"
namespace tmc {
using work_item = tmc::coro_functor;
}
#elif WORK_ITEM_IS(FUNCORO32)
#include "tmc/detail/coro_functor32.hpp"
namespace tmc {
using work_item = tmc::coro_functor32;
}
#endif

namespace tmc {
namespace detail {
class type_erased_executor {
public:
  void* executor;
  void (*s_post)(void*, work_item&& item, size_t prio);
  void (*s_post_bulk)(void*, work_item* items, size_t prio, size_t count);
  type_erased_executor* parent;
  inline void post(work_item&& item, size_t prio) {
    s_post(executor, std::move(item), prio);
  }
  inline void post_bulk(work_item* items, size_t prio, size_t count) {
    s_post_bulk(executor, items, prio, count);
  }
  // type_erased_executor() : executor(nullptr), s_post(nullptr),
  // s_post_bulk(nullptr), parent(nullptr) {}
  template <typename T> type_erased_executor(T& ref) {
    executor = &ref;
    s_post = [](void* erased, work_item&& item, size_t prio) {
      static_cast<T*>(erased)->post(std::move(item), prio);
    };
    s_post_bulk = [](
                    void* erased, work_item* items, size_t prio, size_t count
                  ) { static_cast<T*>(erased)->post_bulk(items, prio, count); };
  }
};

struct running_task_data {
  size_t prio = 0;
  // pointer to single element
  // this is used both for yielding, and for determining whether spawn_many
  // tasks may symmetric transfer
  std::atomic<size_t>* yield_priority;
};
namespace this_thread { // namespace reserved for thread_local variables
inline thread_local type_erased_executor* executor = nullptr;
inline thread_local running_task_data this_task;
inline thread_local std::string thread_name;
} // namespace this_thread
} // namespace detail
} // namespace tmc
