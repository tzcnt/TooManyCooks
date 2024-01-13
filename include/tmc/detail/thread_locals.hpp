#pragma once
#include <atomic>
#include <cstdlib>
#include <string>

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
// A bump allocator that creates space for a fixed number of chunks (coroutines)
// It must take ChunkCount first (from spawn_many), then ChunkSize (from the
// first coroutine constructor). It does not allow random access deallocation -
// only the top of the stack can be popped, or all items can be dropped when
// this goes out of scope.
struct al_bump_scoped {
  std::byte* mem_begin;
  std::byte* mem_end;
  std::byte* mem_curr;
  size_t chunk_count;

  al_bump_scoped(size_t ChunkCount)
      : mem_begin(nullptr), chunk_count(ChunkCount) {}
  al_bump_scoped(al_bump_scoped& Other) = delete;
  al_bump_scoped& operator=(al_bump_scoped& Other) = delete;
  al_bump_scoped(al_bump_scoped&& Other);
  al_bump_scoped& operator=(al_bump_scoped&& Other);

  void stack_init(size_t BytesCount);
  void* stack_next(size_t ChunkSize);

  void* group_first(size_t ChunkSize);

  void* group_next(size_t ChunkSize);

  // Cannot deallocate in random order; must pop from top of stack
  void dealloc(void* ptr, size_t sz);

  ~al_bump_scoped();
};
namespace detail {
class type_erased_executor {
public:
  void* executor;
  void (*s_post)(void* Erased, work_item&& Item, size_t Priority);
  void (*s_post_bulk)(
    void* Erased, work_item* Items, size_t Priority, size_t Count
  );
  type_erased_executor* parent;
  inline void post(work_item&& Item, size_t Priority) {
    s_post(executor, std::move(Item), Priority);
  }
  inline void post_bulk(work_item* Items, size_t Priority, size_t Count) {
    s_post_bulk(executor, Items, Priority, Count);
  }

  template <typename T> type_erased_executor(T& Executor) {
    executor = &Executor;
    s_post = [](void* Erased, work_item&& Item, size_t Priority) {
      static_cast<T*>(Erased)->post(std::move(Item), Priority);
    };
    s_post_bulk =
      [](void* Erased, work_item* Items, size_t Priority, size_t Count) {
        static_cast<T*>(Erased)->post_bulk(Items, Priority, Count);
      };
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
inline thread_local void* producers = nullptr;
inline thread_local tmc::al_bump_scoped* shared_buffer = nullptr;
inline void* group_first(size_t n) { return shared_buffer->group_first(n); }
inline void* group_next(size_t n) { return shared_buffer->group_next(n); }
inline void* stack_next(size_t n) { return shared_buffer->stack_next(n); }
inline void bump_alloc_free(void* ptr, size_t n) {
  // Cannot free in random order; must pop from top of stack
  shared_buffer->dealloc(ptr, n);
}
inline void dont_free(void* ptr, size_t sz) {}
inline void free_shim(void* ptr, size_t sz) { free(ptr); }
inline thread_local void* (*alloc)(size_t n) = malloc;
inline thread_local void (*dealloc)(void*, size_t) = free_shim;
} // namespace this_thread
} // namespace detail

void* al_bump_scoped::group_first(size_t ChunkSize) {
  mem_begin = (std::byte*)malloc(ChunkSize * chunk_count);
  mem_end = mem_begin + ChunkSize * chunk_count;
  mem_curr = mem_end - ChunkSize;
  detail::this_thread::dealloc = detail::this_thread::dont_free;
  return mem_curr;
}

void* al_bump_scoped::group_next(size_t ChunkSize) {
  auto newMemCurr = mem_curr - ChunkSize;
  // Assume that pointer subtraction won't underflow
  if (newMemCurr < mem_begin) {
    // Ran out of space (inconsistent chunk sizes?)
    // Give this chunk its own allocation
    detail::this_thread::dealloc = detail::this_thread::free_shim;
    return malloc(ChunkSize);
  } else {
    detail::this_thread::dealloc = detail::this_thread::dont_free;
    mem_curr = newMemCurr;
    return newMemCurr;
  }
}

void al_bump_scoped::stack_init(size_t BytesCount) {
  mem_begin = (std::byte*)malloc(BytesCount);
  mem_end = mem_begin + BytesCount;
  mem_curr = mem_end;
}

void* al_bump_scoped::stack_next(size_t ChunkSize) {
  auto newMemCurr = mem_curr - ChunkSize;
  // Assume that pointer subtraction won't underflow
  if (newMemCurr < mem_begin) {
    // Ran out of space (inconsistent chunk sizes?)
    // Give this chunk its own allocation
    detail::this_thread::dealloc = detail::this_thread::free_shim;
    return malloc(ChunkSize);
  } else {
    detail::this_thread::dealloc = detail::this_thread::bump_alloc_free;
    mem_curr = newMemCurr;
    return newMemCurr;
  }
}

void al_bump_scoped::dealloc(void* ptr, size_t sz) {
  if (ptr == mem_curr) {
    mem_curr += sz;
  } else {
    free(ptr);
  }
}

al_bump_scoped::~al_bump_scoped() {
  // comment
  free(mem_begin);
}

al_bump_scoped::al_bump_scoped(al_bump_scoped&& Other) {
  mem_begin = Other.mem_begin;
  mem_end = Other.mem_end;
  mem_curr = Other.mem_curr;
  chunk_count = Other.chunk_count;
  Other.mem_begin = nullptr;
}
al_bump_scoped& al_bump_scoped::operator=(al_bump_scoped&& Other) {
  mem_begin = Other.mem_begin;
  mem_end = Other.mem_end;
  mem_curr = Other.mem_curr;
  chunk_count = Other.chunk_count;
  Other.mem_begin = nullptr;
  return *this;
}
} // namespace tmc
