// Copyright (c) 2023-2024 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include <atomic>
#include <limits>

// Macro hackery to enable defines TMC_WORK_ITEM=CORO / TMC_WORK_ITEM=FUNC, etc
#define TMC_WORK_ITEM_CORO 0 // coro will be the default if undefined
#define TMC_WORK_ITEM_FUNC 1
#define TMC_WORK_ITEM_FUNCORO 2
#define TMC_WORK_ITEM_FUNCORO32 3
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
#elif TMC_WORK_ITEM_IS(FUNCORO32)
#include "tmc/detail/coro_functor32.hpp"
namespace tmc {
using work_item = tmc::coro_functor32;
}
#define TMC_WORK_ITEM_AS_STD_CORO(x) (x.as_coroutine())
#endif

namespace tmc {
namespace detail {
class type_erased_executor;

// The default executor that is used by post_checked / post_bulk_checked
// when the current (non-TMC) thread's executor == nullptr.
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
inline constinit thread_local int64_t alloc_count = 0;
inline constinit thread_local void* alloc_header = nullptr;

inline constinit thread_local int64_t alloc_cache_sizes[2] = {};
inline constinit thread_local void* alloc_cache_ptrs[2] = {};

inline void* cache_alloc(size_t n) {
  int64_t size = static_cast<int64_t>(n);
  if (size > alloc_cache_sizes[0]) {
    if (size > alloc_cache_sizes[1]) {
      return ::operator new(n);
    } else {
      alloc_cache_sizes[1] = 0;
      void* ret = alloc_cache_ptrs[1];
      alloc_cache_ptrs[1] = nullptr;
      return ret;
    }
  } else {
    if (size > alloc_cache_sizes[1]) {
      alloc_cache_sizes[0] = 0;
      void* ret = alloc_cache_ptrs[0];
      alloc_cache_ptrs[0] = nullptr;
      return ret;
    } else {
      int64_t diff0 = alloc_cache_sizes[0] - size;
      int64_t diff1 = alloc_cache_sizes[0] - size;
      if (diff1 < diff0) {
        alloc_cache_sizes[1] = 0;
        void* ret = alloc_cache_ptrs[1];
        alloc_cache_ptrs[1] = nullptr;
        return ret;
      } else {
        alloc_cache_sizes[0] = 0;
        void* ret = alloc_cache_ptrs[0];
        alloc_cache_ptrs[0] = nullptr;
        return ret;
      }
    }
  }
}

inline void cache_free(void* m, size_t n) {
  for (size_t i = 0; i < 2; ++i) {
    if (alloc_cache_sizes[i] == 0) {
      alloc_cache_sizes[i] = n;
      alloc_cache_ptrs[i] = m;
      return;
    }
  }
  void* to_free;
  if (n > alloc_cache_sizes[0]) {
    if (n > alloc_cache_sizes[1]) {
      if (alloc_cache_sizes[0] < alloc_cache_sizes[1]) {
        to_free = alloc_cache_ptrs[0];
        alloc_cache_sizes[0] = n;
        alloc_cache_ptrs[0] = m;
      } else {
        to_free = alloc_cache_ptrs[1];
        alloc_cache_sizes[1] = n;
        alloc_cache_ptrs[1] = m;
      }
    } else {
      to_free = alloc_cache_ptrs[0];
      alloc_cache_sizes[0] = n;
      alloc_cache_ptrs[0] = m;
    }
  } else {
    if (n > alloc_cache_sizes[1]) {
      to_free = alloc_cache_ptrs[1];
      alloc_cache_sizes[1] = n;
      alloc_cache_ptrs[1] = m;
    } else {
      to_free = m;
    }
  }
  // Can't use sized deallocation due to shrinking behavior of cache_alloc
  // The real allocation might be bigger than we know about
  // #ifdef __cpp_sized_deallocation
  //   ::operator delete(static_cast<void*>(m), n);
  // #else
  ::operator delete(static_cast<void*>(to_free));
  // #endif
}

inline bool exec_is(type_erased_executor const* const Executor) {
  return Executor == executor;
}
inline bool prio_is(size_t const Priority) {
  return Priority == this_task.prio;
}

} // namespace this_thread

inline void post_checked(
  detail::type_erased_executor* executor, work_item&& Item, size_t Priority
) {
  if (executor == nullptr) {
    executor = g_ex_default.load(std::memory_order_acquire);
  }
  executor->post(std::move(Item), Priority);
}
inline void post_bulk_checked(
  detail::type_erased_executor* executor, work_item* Items, size_t Count,
  size_t Priority
) {
  if (executor == nullptr) {
    executor = g_ex_default.load(std::memory_order_acquire);
  }
  executor->post_bulk(Items, Count, Priority);
}

} // namespace detail
} // namespace tmc

#if defined(_MSC_VER) && !defined(__clang__)
#define TMC_FORCE_INLINE [[msvc::forceinline]]
#else
#define TMC_FORCE_INLINE __attribute__((always_inline))
#endif
