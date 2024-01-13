#pragma once
#include <cstddef>
#include <cstdlib>

namespace tmc {
// Creates a "stack" for stackless coroutines to use.
struct al_stack {
  constexpr static size_t STACK_SIZE =
    8 * 1024 * 1024; // try 8MB for now - default stack size on linux
  std::byte* mem_begin;
  std::byte* mem_end;
  std::byte* mem_curr;

  // Calling code (spawn_many) can check this to determine if we had to fallback
  // to individual malloc for this chunk. If so, that coroutine also needs to
  // free its own memory when destroyed.
  bool alloc_fallback;

  al_stack() : mem_begin(nullptr) {}
  al_stack(al_stack& Other) = delete;
  al_stack& operator=(al_stack& Other) = delete;
  al_stack(al_stack&& Other) {
    mem_begin = Other.mem_begin;
    mem_end = Other.mem_end;
    mem_curr = Other.mem_curr;
    alloc_fallback = Other.alloc_fallback;
    Other.mem_begin = nullptr;
  }
  al_stack& operator=(al_stack&& Other) {
    mem_begin = Other.mem_begin;
    mem_end = Other.mem_end;
    mem_curr = Other.mem_curr;
    alloc_fallback = Other.alloc_fallback;
    Other.mem_begin = nullptr;
    return *this;
  }

  void* first(size_t ChunkSize) {
    if (ChunkSize > STACK_SIZE) {
      // Give this chunk its own allocation
      alloc_fallback = true;
      return malloc(ChunkSize);
    } else {
      alloc_fallback = false;
      mem_begin = (std::byte*)malloc(STACK_SIZE);
      mem_end = mem_begin + STACK_SIZE;
      mem_curr = mem_end - ChunkSize;
      return mem_curr;
    }
  }

  void* next(size_t ChunkSize) {
    auto newMemCurr = mem_curr - ChunkSize;
    // Assume that pointer subtraction won't underflow
    if (newMemCurr < mem_begin) {
      // Ran out of space (inconsistent chunk sizes?)
      // Give this chunk its own allocation
      alloc_fallback = true;
      return malloc(ChunkSize);
    } else {
      alloc_fallback = false;
      mem_curr = newMemCurr;
      return newMemCurr;
    }
  }

  void dealloc(void* ptr, size_t sz) {
    if (ptr == mem_curr) {
      mem_curr += sz;
    } else {
      free(ptr);
    }
  }

  ~al_stack() {
    // comment
    free(mem_begin);
  }
};
} // namespace tmc
