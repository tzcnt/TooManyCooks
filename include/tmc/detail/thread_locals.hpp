#pragma once
#include <atomic>
#include <limits>

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
  void (*s_post)(void* Erased, work_item&& Item, size_t Priority);
  void (*s_post_bulk)(
    void* Erased, work_item* Items, size_t Priority, size_t Count
  );
  inline void post(work_item&& Item, size_t Priority) {
    s_post(executor, std::move(Item), Priority);
  }
  inline void post_bulk(work_item* Items, size_t Priority, size_t Count) {
    s_post_bulk(executor, Items, Priority, Count);
  }

  template <typename T> type_erased_executor(T* Executor) {
    executor = Executor;
    s_post = [](void* Erased, work_item&& Item, size_t Priority) {
      static_cast<T*>(Erased)->post(std::move(Item), Priority);
    };
    s_post_bulk =
      [](void* Erased, work_item* Items, size_t Priority, size_t Count) {
        static_cast<T*>(Erased)->post_bulk(Items, Priority, Count);
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
} // namespace this_thread
} // namespace detail
} // namespace tmc
