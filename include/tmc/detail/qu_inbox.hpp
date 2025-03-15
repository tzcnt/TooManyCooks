// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

// tmc::detail::qu_inbox is a fixed-size MPMC queue used to push data directly
// to a specific thread. Other threads may occasionally steal from this inbox,
// but only as a last resort.

// At the moment it uses a single block, thus Capacity == BlockSize.

#include <array>
#include <atomic>

namespace tmc {
namespace detail {

template <typename T, size_t BlockSize = 64> class qu_inbox {

public:
  static_assert(
    BlockSize && ((BlockSize & (BlockSize - 1)) == 0),
    "BlockSize must be a power of 2"
  );
  static inline constexpr size_t BlockSizeMask = BlockSize - 1;

  // If write_offset == read_offset, queue is empty
  // If write_offset == read_offset - 1, queue is full
  // 1 element of capacity is wasted to make this work
  std::atomic<size_t> write_offset;
  char pad0[64 - 1 * (sizeof(size_t))];
  std::atomic<size_t> read_offset;
  char pad1[64 - 1 * (sizeof(size_t))];
  std::array<T, BlockSize> data;
  std::array<std::atomic<int>, BlockSize> flags;

  qu_inbox() : write_offset{0}, read_offset{0}, flags{} {}

public:
  // Returns true if the value was successfully enqueued.
  // If returns false, the value will not be moved (will be present at its
  // original reference). TODO how to exclude prvalues from this?
  template <typename U> bool try_push(U&& t) {
    size_t woff = write_offset.load(std::memory_order_acquire);
    size_t roff = read_offset.load(std::memory_order_acquire);
    while (woff - roff < BlockSizeMask) { // TODO handle index overflow
      // Queue isn't full, try to write
      if (write_offset.compare_exchange_strong(
            woff, woff + 1, std::memory_order_acq_rel, std::memory_order_relaxed
          )) {
        size_t idx = woff & BlockSizeMask;
        while (flags[idx].load(std::memory_order_acquire) != 0) {
          // Wait to see that the block is clear for our use
        }
        data[idx] = std::move(t);
        flags[idx].store(1, std::memory_order_release);
        return true;
      }
    }
    // queue is full
    return false;
  }

  // Returns true if the value was successfully dequeued.
  template <typename U> bool try_pull(U& t) {
    size_t woff = write_offset.load(std::memory_order_acquire);
    size_t roff = read_offset.load(std::memory_order_acquire);
    while (roff < woff) { // TODO handle index overflow
      // Queue isn't empty.
      if (read_offset.compare_exchange_strong(
            roff, roff + 1, std::memory_order_acq_rel, std::memory_order_relaxed
          )) {
        size_t idx = roff & BlockSizeMask;
        // Wait to see that the data has been written
        while (flags[idx].load(std::memory_order_acquire) != 1) {
        }

        // Data is ready in the queue
        t = std::move(data[idx]);
        flags[idx].store(0, std::memory_order_release);
        return true;
      } else {
        continue;
      }
    }
    // queue is empty
    return false;
  }

  bool empty() {
    size_t woff = write_offset.load(std::memory_order_acquire);
    size_t roff = read_offset.load(std::memory_order_acquire);
    return roff == woff;
  }
};
} // namespace detail
} // namespace tmc
