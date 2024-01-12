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
  size_t chunk_size;
  size_t chunk_count;

  allocator_manual_coro(size_t ChunkCount)
      : mem_begin(nullptr), chunk_count(ChunkCount) {}
  allocator_manual_coro(allocator_manual_coro& Other) = delete;
  allocator_manual_coro& operator=(allocator_manual_coro& Other) = delete;
  allocator_manual_coro(allocator_manual_coro&& Other) {
    mem_begin = Other.mem_begin;
    mem_end = Other.mem_end;
    mem_curr = Other.mem_curr;
    chunk_size = Other.chunk_size;
    chunk_count = Other.chunk_count;
    Other.mem_begin = nullptr;
  }
  allocator_manual_coro& operator=(allocator_manual_coro&& Other) {
    mem_begin = Other.mem_begin;
    mem_end = Other.mem_end;
    mem_curr = Other.mem_curr;
    chunk_size = Other.chunk_size;
    chunk_count = Other.chunk_count;
    Other.mem_begin = nullptr;
    return *this;
  }

  // void* operator()(size_t ChunkSize) {
  //   if (mem_begin == nullptr) {
  //     return first(ChunkSize);
  //   }
  //   return next(ChunkSize);
  // }

  void* first(size_t ChunkSize) {
    mem_begin = (std::byte*)malloc(ChunkSize * chunk_count);
    mem_curr = mem_begin + ChunkSize;
    mem_end = mem_begin + ChunkSize * chunk_count;
    chunk_size = ChunkSize;
    return mem_begin; // TODO bump downwards
  }

  void* next(size_t ChunkSize) {
    assert(ChunkSize == chunk_size);
    auto result = mem_curr;
    mem_curr += ChunkSize;
    // if (mem_curr > mem_end) {
    //   std::printf("overallocated");
    // }
    assert(mem_curr <= mem_end);
    return result;
  }

  ~allocator_manual_coro() {
    // comment
    free(mem_begin);
  }
};
} // namespace tmc