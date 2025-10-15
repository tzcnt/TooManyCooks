#pragma once

#include "tmc/detail/compat.hpp"

#include <cstddef>
#include <cstdlib>
#include <exception>
#include <memory>
#include <type_traits>

namespace tmc {
// A bump allocator that creates space for a fixed number of chunks (coroutines)
// It must take ChunkCount first (from spawn_many), then ChunkSize (from the
// first coroutine constructor). It does not allow deallocating any individual
// element - all of the elements are deallocated at once when the allocator is
// destroyed.
struct al_bump_scoped {
  // shared_ptr mitigates the following race condition:
  // 1. child task completes, decrementing the done_count, but then it's thread
  // goes to sleep before destructor runs
  // 2. parent task completes because all children completed. al_bump_scoped is
  // destroyed.
  // 3. sleeping child task awakens and tries to run destructor (using memore
  // that's been freed already)
  std::shared_ptr<std::byte[]> mem_begin;
  std::byte* mem_end;
  std::byte* mem_curr;
  size_t chunk_count;

  // Calling code (spawn_many) can check this to determine if we had to fallback
  // to individual malloc for this chunk. If so, that coroutine also needs to
  // free its own memory when destroyed.
  bool alloc_fallback;

  al_bump_scoped(size_t ChunkCount)
      : mem_begin(nullptr), mem_end(nullptr), mem_curr(nullptr),
        chunk_count(ChunkCount), alloc_fallback(false) {}
  al_bump_scoped(al_bump_scoped& Other) = delete;
  al_bump_scoped& operator=(al_bump_scoped& Other) = delete;
  al_bump_scoped(al_bump_scoped&& Other) {
    mem_begin = Other.mem_begin;
    mem_end = Other.mem_end;
    mem_curr = Other.mem_curr;
    chunk_count = Other.chunk_count;
    alloc_fallback = Other.alloc_fallback;
    Other.mem_begin = nullptr;
  }
  al_bump_scoped& operator=(al_bump_scoped&& Other) {
    mem_begin = Other.mem_begin;
    mem_end = Other.mem_end;
    mem_curr = Other.mem_curr;
    chunk_count = Other.chunk_count;
    alloc_fallback = Other.alloc_fallback;
    Other.mem_begin = nullptr;
    return *this;
  }

  TMC_FORCE_INLINE void* alloc(size_t ChunkSize) {
    if (alloc_fallback) {
      return ::operator new(ChunkSize);
    } else if (mem_begin == nullptr) [[unlikely]] {
      mem_begin = std::make_shared<std::byte[]>(ChunkSize * chunk_count);
      if (mem_begin == nullptr) {
        alloc_fallback = true;
        return ::operator new(ChunkSize);
      }
      mem_end = mem_begin.get() + ChunkSize * chunk_count;
      mem_curr = mem_begin.get();
      auto mem_old = mem_curr;
      ChunkSize = (ChunkSize + 63) & -64;
      mem_curr += ChunkSize;
      return mem_old;
    } else {
      ChunkSize = (ChunkSize + 63) & -64;
      auto mem_old = mem_curr;
      mem_curr += ChunkSize;
      // Assume that pointer subtraction won't underflow
      if (mem_curr >= mem_end) [[unlikely]] {
        // Ran out of space (inconsistent chunk sizes?)
        // Give this chunk its own allocation
        alloc_fallback = true;
        return ::operator new(ChunkSize);
      }
      return mem_old;
    }
  }

  // TMC_FORCE_INLINE void* alloc(size_t ChunkSize) {
  //   if (alloc_fallback) {
  //     return malloc(ChunkSize);
  //   } else if (mem_begin == nullptr) [[unlikely]] {
  //     mem_begin = (std::byte*)malloc(ChunkSize * chunk_count);
  //     if (mem_begin == nullptr) {
  //       alloc_fallback = true;
  //       return malloc(ChunkSize);
  //     }
  //     mem_end = mem_begin + ChunkSize * chunk_count;
  //     mem_curr = mem_end - ChunkSize;
  //     return mem_curr;
  //   } else {
  //     mem_curr -= ChunkSize;
  //     // round down to next max_align_t
  //     mem_curr = reinterpret_cast<std::byte*>(
  //       reinterpret_cast<uintptr_t>(mem_curr) &
  //       ~reinterpret_cast<uintptr_t>((alignof(std::max_align_t) - 1))
  //     );
  //     // Assume that pointer subtraction won't underflow
  //     if (mem_curr < mem_begin) [[unlikely]] {
  //       // Ran out of space (inconsistent chunk sizes?)
  //       // Give this chunk its own allocation
  //       alloc_fallback = true;
  //       return malloc(ChunkSize);
  //     }
  //     return mem_curr;
  //   }
  // }

  // void* first(size_t ChunkSize) {
  //   mem_begin = (std::byte*)malloc(ChunkSize * chunk_count);
  //   mem_end = mem_begin + ChunkSize * chunk_count;
  //   mem_curr = mem_end - ChunkSize;
  //   return mem_curr;
  // }

  // void* next(size_t ChunkSize) {
  //   mem_curr -= ChunkSize;
  //   // Assume that pointer subtraction won't underflow
  //   if (mem_curr < mem_begin || mem_curr > mem_end) {
  //     // Ran out of space (inconsistent chunk sizes?)
  //     // Give this chunk its own allocation
  //     alloc_fallback = true;
  //     return malloc(ChunkSize);
  //   }
  //   return mem_curr;
  // }
};
} // namespace tmc
