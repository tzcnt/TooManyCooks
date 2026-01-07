// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

// tmc::detail::qu_inbox is a fixed-size MPMC queue used to push data directly
// to a thread group. Stores and restores the priority by function parameters,
// but does not actually implement a priority queue - it's just FIFO.

// At the moment it uses a single block, thus Capacity == BlockSize.

#include "tmc/detail/compat.hpp"

#include <array>
#include <atomic>
#include <cstddef>

namespace tmc {
namespace detail {

template <typename T, size_t BlockSize> class qu_inbox {
  struct element {
    T data;
    size_t prio;
    std::atomic<size_t> flags;
    // Currently not using padding as this inbox is always allocated for every
    // thread group, but is used in a limited fashion.
    // static constexpr size_t UNPADLEN = sizeof(size_t) + sizeof(T);
    // static constexpr size_t PADLEN = UNPADLEN < 64 ? (64 - UNPADLEN) : 0;
    // char pad[PADLEN];
  };

public:
  static_assert(
    BlockSize && ((BlockSize & (BlockSize - 1)) == 0),
    "BlockSize must be a power of 2"
  );

  // Ensure that the subtraction of unsigned offsets always results in a value
  // that can be represented as a signed integer.
  static_assert(
    BlockSize <= (TMC_ONE_BIT << (TMC_PLATFORM_BITS - 1)),
    "BlockSize must not be larger than half the max value that can be "
    "represented by a platform word"
  );

  static inline constexpr size_t BlockSizeMask = BlockSize - 1;

  // If write_offset == read_offset, queue is empty
  // If write_offset == read_offset - 1, queue is full
  // 1 element of capacity is wasted to make this work
  std::atomic<size_t> write_offset;
  char pad0[64 - 1 * (sizeof(size_t))];
  std::atomic<size_t> read_offset;
  char pad1[64 - 1 * (sizeof(size_t))];
  std::array<element, BlockSize> elements;

  qu_inbox() {
    for (size_t i = 0; i < elements.size(); ++i) {
      elements[i].flags.store(i, std::memory_order_relaxed);
    }
    write_offset.store(0, std::memory_order_release);
    read_offset.store(0, std::memory_order_release);
  }

public:
  // Try to enqueue up to Count elements. Returns the number of
  // elements that were successfully enqueued. The iterator will be advanced
  // that number of elements. The purpose of this function (as opposed to
  // calling try_push() in a loop) is to avoid dereferencing the iterator (*it)
  // multiple times at the same index in the case of failures.
  template <typename It>
  size_t try_push_bulk(It&& it, size_t Count, size_t Prio) {
    size_t woff = write_offset.load(std::memory_order_relaxed);
    size_t i = 0;
    while (i < Count) {
      while (true) {
        size_t idx = woff & BlockSizeMask;
        element* elem = &elements[idx];
        size_t f = elem->flags.load(std::memory_order_acquire);
        ptrdiff_t diff = static_cast<ptrdiff_t>(f - woff);
        if (diff == 0) {
          // This element is clear. Try to get a ticket to write it.
          if (write_offset.compare_exchange_strong(
                woff, woff + 1, std::memory_order_relaxed,
                std::memory_order_relaxed
              )) {
            TMC_DISABLE_WARNING_PESSIMIZING_MOVE_BEGIN
            elem->data = std::move(*it);
            TMC_DISABLE_WARNING_PESSIMIZING_MOVE_END
            elem->prio = Prio;
            elem->flags.store(woff + 1, std::memory_order_release);
            ++it;
            ++i;
            break;
          }
        } else if (diff < 0) {
          // This element is still in use by the previous loop-around
          // (queue appears to be full)
          return i;
        } else {
          // Another thread already wrote this element
          woff = write_offset.load(std::memory_order_relaxed);
        }
      }
    }
    return Count;
  }

  // Returns true if the value was successfully enqueued.
  // If returns false, the value will not be moved (will be present at its
  // original reference).
  template <typename U> bool try_push(U&& t, size_t Prio) {
    size_t woff = write_offset.load(std::memory_order_relaxed);
    while (true) {
      size_t idx = woff & BlockSizeMask;
      element* elem = &elements[idx];
      size_t f = elem->flags.load(std::memory_order_acquire);
      ptrdiff_t diff = static_cast<ptrdiff_t>(f - woff);
      if (diff == 0) {
        // This element is clear. Try to get a ticket to write it.
        if (write_offset.compare_exchange_strong(
              woff, woff + 1, std::memory_order_relaxed,
              std::memory_order_relaxed
            )) {
          elem->data = std::forward<U>(t);
          elem->prio = Prio;
          elem->flags.store(woff + 1, std::memory_order_release);
          return true;
        }
      } else if (diff < 0) {
        // This element is still in use by the previous loop-around
        // (queue appears to be full)
        return false;
      } else {
        // Another thread already wrote this element
        woff = write_offset.load(std::memory_order_relaxed);
      }
    }
  }

  // Returns true if the value was successfully dequeued.
  template <typename U> bool try_pull(U& t, size_t& Prio) {
    size_t roff = read_offset.load(std::memory_order_relaxed);
    while (true) {
      size_t idx = roff & BlockSizeMask;
      element* elem = &elements[idx];
      size_t f = elem->flags.load(std::memory_order_acquire);
      ptrdiff_t diff = static_cast<ptrdiff_t>(f - (roff + 1));
      // Inbox is used infrequently, so expect it to be empty
      if (diff < 0) [[likely]] {
        // diff == -BlockSize : still in use by a previous loop-around
        // diff == -1 : no data ready. There may be no writer, or there may be a
        // writer with a ticket, but they haven't completed the write yet.
        // (queue appears empty in either case)
        return false;
      } else if (diff == 0) {
        // Data is ready at this element. Try to get a ticket to read it.
        if (read_offset.compare_exchange_strong(
              roff, roff + 1, std::memory_order_relaxed,
              std::memory_order_relaxed
            )) {
          // Data is ready in the queue
          t = std::move(elem->data);
          Prio = elem->prio;
          elem->flags.store(roff + BlockSize, std::memory_order_release);
          return true;
        }
      } else {
        // Another thread already read this element
        roff = read_offset.load(std::memory_order_relaxed);
      }
    }
  }

  bool empty() {
    size_t woff = write_offset.load(std::memory_order_relaxed);
    size_t roff = read_offset.load(std::memory_order_relaxed);
    return roff == woff;
  }
};
} // namespace detail
} // namespace tmc
