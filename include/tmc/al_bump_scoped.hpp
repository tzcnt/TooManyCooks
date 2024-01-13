#pragma once
#include <cstddef>
#include <cstdlib>

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
  al_bump_scoped(al_bump_scoped&& Other) {
    mem_begin = Other.mem_begin;
    mem_end = Other.mem_end;
    mem_curr = Other.mem_curr;
    chunk_count = Other.chunk_count;
    Other.mem_begin = nullptr;
  }
  al_bump_scoped& operator=(al_bump_scoped&& Other) {
    mem_begin = Other.mem_begin;
    mem_end = Other.mem_end;
    mem_curr = Other.mem_curr;
    chunk_count = Other.chunk_count;
    Other.mem_begin = nullptr;
    return *this;
  }

  void stack_init(size_t BytesCount) {
    mem_begin = (std::byte*)malloc(BytesCount);
    mem_end = mem_begin + BytesCount;
    mem_curr = mem_end;
  }

  void* group_first(size_t ChunkSize) {
    mem_begin = (std::byte*)malloc(ChunkSize * chunk_count);
    mem_end = mem_begin + ChunkSize * chunk_count;
    mem_curr = mem_end - ChunkSize;
    detail::this_thread::dealloc = detail::this_thread::bump_alloc_free;
    return mem_curr;
  }

  void* group_next(size_t ChunkSize) {
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

  // Cannot deallocate in random order; must pop from top of stack
  void dealloc(void* ptr, size_t sz) {
    if (ptr == mem_curr) {
      mem_curr += sz;
    } else {
      free(ptr);
    }
  }

  ~al_bump_scoped() {
    // comment
    free(mem_begin);
  }
};
} // namespace tmc
