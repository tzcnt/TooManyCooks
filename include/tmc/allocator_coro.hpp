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
    // chunk_size = Other.chunk_size;
    chunk_count = Other.chunk_count;
    Other.mem_begin = nullptr;
  }
  allocator_manual_coro& operator=(allocator_manual_coro&& Other) {
    mem_begin = Other.mem_begin;
    mem_end = Other.mem_end;
    mem_curr = Other.mem_curr;
    // chunk_size = Other.chunk_size;
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
    chunk_size = (ChunkSize + 63) & (-64); // round up to nearest 64
    mem_begin = (std::byte*)malloc(chunk_size * chunk_count);
    mem_end = mem_begin + chunk_size * chunk_count;
    mem_curr = mem_end - chunk_size;
    return mem_curr;
  }

  void* next(size_t ChunkSize) {
    // assert(ChunkSize == chunk_size);
    mem_curr -= chunk_size;
    if (mem_curr < mem_begin) {
      std::printf("overallocated");
    }
    assert(mem_curr >= mem_begin);
    return mem_curr;
  }

  ~allocator_manual_coro() {
    // comment
    free(mem_begin);
  }
};
} // namespace tmc