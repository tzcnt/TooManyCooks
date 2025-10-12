#pragma once
#include <cstddef>
#include <cstdlib>

namespace tmc {
// A bump allocator that creates space for a fixed number of chunks (coroutines)
// It must take ChunkCount first (from spawn_many), then ChunkSize (from the
// first coroutine constructor). It does not allow deallocating any individual
// element - all of the elements are deallocated at once when the allocator is
// destroyed.
struct al_bump_scoped {
  std::byte* mem_begin;
  std::byte* mem_end;
  std::byte* mem_curr;
  size_t chunk_count;

  // Calling code (spawn_many) can check this to determine if we had to fallback
  // to individual malloc for this chunk. If so, that coroutine also needs to
  // free its own memory when destroyed.
  bool alloc_fallback;

  al_bump_scoped(size_t ChunkCount)
      : mem_begin(nullptr), chunk_count(ChunkCount), alloc_fallback(false) {}
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

  void* first(size_t ChunkSize) {
    mem_begin = (std::byte*)malloc(ChunkSize * chunk_count);
    mem_end = mem_begin + ChunkSize * chunk_count;
    mem_curr = mem_end - ChunkSize;
    return mem_curr;
  }

  void* next(size_t ChunkSize) {
    mem_curr -= ChunkSize;
    // Assume that pointer subtraction won't underflow
    if (mem_curr < mem_begin) {
      // Ran out of space (inconsistent chunk sizes?)
      // Give this chunk its own allocation
      alloc_fallback = true;
      return malloc(ChunkSize);
    }
    return mem_curr;
  }

  ~al_bump_scoped() {
    // comment
    free(mem_begin);
  }
};
} // namespace tmc
