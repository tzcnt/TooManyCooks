#pragma once
#include <cassert>
#include <cinttypes>
#include <cstdio>
#include <memory>
namespace tmc {
struct allocator_manual_coro {
  std::byte* mem_begin;
  std::byte* mem_end;
  std::byte* mem_curr;
  size_t chunk_count;
  bool alloc_fallback;

  allocator_manual_coro(size_t ChunkCount)
      : mem_begin(nullptr), chunk_count(ChunkCount), alloc_fallback(false) {}
  allocator_manual_coro(allocator_manual_coro& Other) = delete;
  allocator_manual_coro& operator=(allocator_manual_coro& Other) = delete;
  allocator_manual_coro(allocator_manual_coro&& Other) {
    mem_begin = Other.mem_begin;
    mem_end = Other.mem_end;
    mem_curr = Other.mem_curr;
    chunk_count = Other.chunk_count;
    Other.mem_begin = nullptr;
  }
  allocator_manual_coro& operator=(allocator_manual_coro&& Other) {
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

  ~allocator_manual_coro() {
    // comment
    free(mem_begin);
  }
};
} // namespace tmc
