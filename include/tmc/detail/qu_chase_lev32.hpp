// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/compat.hpp"
#include "tmc/detail/tsan.hpp"

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>
#include <vector>

namespace tmc {
namespace detail {

// A Chase-Lev work-stealing deque.
//
// The owner (single producer/consumer) calls push() and try_pop() at the tail.
// Any other thread may call steal() to take work from the head.
//
// Buffers can grow. When the owner detects its buffer is full, it allocates a
// new buffer of double the size, copies all elements over, and atomically
// publishes the new buffer. Old buffers are retained in a "leftovers" vector
// so that stealers that loaded the old buffer pointer can still access it
// safely. Leftovers are destroyed only when the deque itself is destroyed.
//
// T must be trivially copyable (and trivially destructible). This is sufficient
// for tmc::work_item (either std::coroutine_handle<> or tmc::coro_functor),
// both of which are non-owning handles whose underlying allocations are freed
// only by the unique copy that is actually executed.
//
// This version does not use any packing, so it is suitable for use on 32-bit.
template <typename T> class chase_lev_deque {
  static_assert(
    std::is_trivially_copyable_v<T>,
    "chase_lev_deque requires a trivially copyable element type"
  );
  static_assert(
    std::is_trivially_destructible_v<T>,
    "chase_lev_deque requires a trivially destructible element type"
  );

  struct buffer {
    size_t capacity;
    size_t mask;
    T* data;

    explicit buffer(size_t cap) : capacity(cap), mask(cap - 1) {
      assert((cap & (cap - 1)) == 0 && "capacity must be a power of two");
      data = static_cast<T*>(::operator new(sizeof(T) * cap));
    }

    ~buffer() { ::operator delete(data); }

    buffer(const buffer&) = delete;
    buffer& operator=(const buffer&) = delete;
  };

  // Top (head) - where steals take from. Owned by all threads.
  // Bottom (tail) - where push/pop happen. Owned by the single owner.
  // Storage is unsigned (defined wrap-around). All relational comparisons
  // between top and bottom are performed by computing (b - t) in uint32_t
  // and reinterpreting the difference as int32_t (the Chase-Lev signed-
  // difference trick), so the algorithm is correct across 32-bit
  // wraparound as long as |b - t| < 2^31.
  alignas(TMC_CACHE_LINE_SIZE) std::atomic<uint32_t> top_;
  std::atomic<uint32_t> bottom_;
  alignas(TMC_CACHE_LINE_SIZE) std::atomic<buffer*> buffer_;

  // Owner-private cached pointer to the current buffer. Avoids one atomic load
  // on the hot push path.
  alignas(TMC_CACHE_LINE_SIZE) buffer* owner_buffer_;

  // Owner-private cached value of bottom_. The owner is the only writer of
  // bottom_, so it can read this private copy instead of the atomic, avoiding
  // a load from the contended top_/bottom_ cache line on every push/pop.
  // Stays in sync with bottom_ via parallel updates in push/pop.
  uint32_t owner_bottom_;

  // Buffers retained until destruction so stealers always have a valid pointer.
  std::vector<buffer*> leftovers_;

  static constexpr size_t DEFAULT_INITIAL_CAPACITY = 64;

  // For T sizes that fit in a single hardware atomic word, we use
  // std::atomic_ref so that push/pop/steal slot accesses are well-defined under
  // the C++ memory model and TSAN-clean. The ordering of these atomics is
  // relaxed; the actual happens-before edge for slot visibility is provided by
  // the release fence / acquire-load on bottom_ / top_.
  //
  // For larger T (notably the 2-pointer coro_functor used when
  // TMC_WORK_ITEM=FUNCORO), std::atomic_ref<T> would either generate a
  // multi-word CAS or fall back to an internal lock. We instead use the classic
  // Chase-Lev "benign racy" approach which ignores invalid reads afterward, and
  // simply disable TSan for the helper.
  template <typename U>
  TMC_INLINE_OR_TSAN static void store_item(T* slot, U&& item) {
    if constexpr (sizeof(T) <= sizeof(size_t)) {
      T tmp(static_cast<U&&>(item));
      std::atomic_ref<T>(*slot).store(tmp, std::memory_order_relaxed);
    } else {
      new (slot) T(static_cast<U&&>(item));
    }
  }

  TMC_INLINE_OR_TSAN static void load_item(T& out, T* slot) {
    if constexpr (sizeof(T) <= sizeof(size_t)) {
      out = std::atomic_ref<T>(*slot).load(std::memory_order_relaxed);
    } else {
      std::memcpy(
        static_cast<void*>(&out), static_cast<const void*>(slot), sizeof(T)
      );
    }
  }

public:
  chase_lev_deque() : chase_lev_deque(DEFAULT_INITIAL_CAPACITY) {}

  explicit chase_lev_deque(size_t initialCapacity) {
    size_t cap = 1;
    while (cap < initialCapacity) {
      cap <<= 1;
    }
    auto* buf = new buffer(cap);
    owner_buffer_ = buf;
    owner_bottom_ = 0;
    buffer_.store(buf, std::memory_order_relaxed);
    top_.store(0, std::memory_order_relaxed);
    bottom_.store(0, std::memory_order_relaxed);
    leftovers_.push_back(buf);
  }

  ~chase_lev_deque() {
    for (buffer* b : leftovers_) {
      delete b;
    }
  }

  chase_lev_deque(const chase_lev_deque&) = delete;
  chase_lev_deque& operator=(const chase_lev_deque&) = delete;
  chase_lev_deque(chase_lev_deque&&) = delete;
  chase_lev_deque& operator=(chase_lev_deque&&) = delete;

  // Owner-only. Push an item at the bottom (tail) of the deque.
  // Grows the buffer if it is full.
  template <typename U> TMC_FORCE_INLINE void push(U&& item) {
    uint32_t b = owner_bottom_;
    uint32_t t = top_.load(std::memory_order_acquire);
    buffer* buf = owner_buffer_;
    // (b - t) is the current queue size, computed in uint32_t (defined
    // wraparound). Reinterpret as int32_t for the signed comparison
    // against capacity (the Chase-Lev signed-difference trick).
    if (static_cast<int32_t>(b - t) > static_cast<int32_t>(buf->capacity) - 1)
      [[unlikely]] {
      // Buffer full - grow.
      buffer* nb = new buffer(buf->capacity * 2);
      for (uint32_t i = t; i != b; ++i) {
        std::memcpy(
          static_cast<void*>(nb->data + (static_cast<size_t>(i) & nb->mask)),
          static_cast<const void*>(
            buf->data + (static_cast<size_t>(i) & buf->mask)
          ),
          sizeof(T)
        );
      }
      leftovers_.push_back(nb);
      owner_buffer_ = nb;
      buffer_.store(nb, std::memory_order_release);
      buf = nb;
    }
    store_item(
      buf->data + (static_cast<size_t>(b) & buf->mask), static_cast<U&&>(item)
    );
    bottom_.store(b + 1u, std::memory_order_release);
    owner_bottom_ = b + 1u;
  }

  // Owner-only. Push Count items at the bottom (tail) of the deque, taken
  // from the iterator It (incremented Count times). Grows the buffer if it
  // is full. Issues only a single release fence and a single store to
  // bottom_, regardless of Count.
  template <typename It>
  TMC_FORCE_INLINE void post_bulk(It&& Items, size_t Count) {
    if (Count == 0) [[unlikely]] {
      return;
    }
    uint32_t b = owner_bottom_;
    uint32_t t = top_.load(std::memory_order_acquire);
    buffer* buf = owner_buffer_;
    // Compute (queue_size + Count) entirely in uint32_t (defined wrap),
    // then reinterpret as int32_t for the signed comparison.
    uint32_t needed_u = (b - t) + static_cast<uint32_t>(Count);
    if (static_cast<int32_t>(needed_u) > static_cast<int32_t>(buf->capacity))
      [[unlikely]] {
      // Buffer too small - grow to fit.
      size_t newCap = buf->capacity * 2;
      size_t needed = static_cast<size_t>(needed_u);
      while (newCap < needed) {
        newCap <<= 1;
      }
      buffer* nb = new buffer(newCap);
      for (uint32_t i = t; i != b; ++i) {
        std::memcpy(
          static_cast<void*>(nb->data + (static_cast<size_t>(i) & nb->mask)),
          static_cast<const void*>(
            buf->data + (static_cast<size_t>(i) & buf->mask)
          ),
          sizeof(T)
        );
      }
      leftovers_.push_back(nb);
      owner_buffer_ = nb;
      buffer_.store(nb, std::memory_order_release);
      buf = nb;
    }
    It it = static_cast<It&&>(Items);
    for (size_t i = 0; i < Count; ++i) {
      store_item(
        buf->data +
          (static_cast<size_t>(b + static_cast<uint32_t>(i)) & buf->mask),
        static_cast<T&&>(*it)
      );
      ++it;
    }
    uint32_t newBottom = b + static_cast<uint32_t>(Count);
    bottom_.store(newBottom, std::memory_order_release);
    owner_bottom_ = newBottom;
  }

  // Owner-only. Pop an item from the bottom (tail) of the deque (LIFO).
  // Returns true if an item was popped, false if the deque was empty.
  TMC_FORCE_INLINE bool try_pop(T& out) {
    // (owner_bottom_ - 1u) wraps to UINT32_MAX when owner_bottom_ is 0;
    // that's the intended bit pattern - the (b - t) signed difference
    // below still computes the correct logical sign.
    uint32_t b = owner_bottom_ - 1u;
    buffer* buf = owner_buffer_;
    bottom_.store(b, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    uint32_t t = top_.load(std::memory_order_relaxed);
    // (b - t) is computed in uint32_t (defined wraparound). Reinterpret as
    // int32_t for the signed sign check (the Chase-Lev trick).
    int32_t diff = static_cast<int32_t>(b - t);
    if (diff >= 0) {
      // Non-empty.
      T* slot = buf->data + (static_cast<size_t>(b) & buf->mask);
      if (diff > 0) {
        // More than one element - safe to take without racing stealers.
        owner_bottom_ = b;
        load_item(out, slot);
        return true;
      }
      // Last element - race with stealers via CAS on top.
      bool won = top_.compare_exchange_strong(
        t, t + 1u, std::memory_order_seq_cst, std::memory_order_relaxed
      );
      bottom_.store(b + 1u, std::memory_order_relaxed);
      owner_bottom_ = b + 1u;
      if (!won) {
        return false;
      }
      load_item(out, slot);
      return true;
    } else {
      // Empty.
      bottom_.store(b + 1u, std::memory_order_relaxed);
      owner_bottom_ = b + 1u;
      return false;
    }
  }

  // Any thread. Steal an item from the top (head) of the deque (FIFO).
  // Returns true if an item was stolen, false otherwise (deque empty or lost
  // a race).
  TMC_FORCE_INLINE bool steal(T& out) {
    // Cheap empty-check first. If the deque appears empty, return without
    // issuing the expensive seq_cst fence or CAS. Both loads are acquire so
    // we still synchronize-with the owner's push, but we avoid the mfence
    // and the CAS-induced cache-line ping-pong on top_ in the common
    // "victim is empty" case.
    uint32_t t = top_.load(std::memory_order_acquire);
    uint32_t b = bottom_.load(std::memory_order_acquire);
    // (b - t) is computed in uint32_t (defined wraparound). Reinterpret as
    // int32_t for the signed sign check.
    if (static_cast<int32_t>(b - t) <= 0) {
      return false;
    }
    std::atomic_thread_fence(std::memory_order_seq_cst);
    b = bottom_.load(std::memory_order_acquire);
    if (static_cast<int32_t>(b - t) > 0) {
      buffer* buf = buffer_.load(std::memory_order_acquire);
      // Racy read - safe because T is trivially copyable and destructible. If
      // we lose the CAS below, the caller will ignore the out value.
      load_item(out, buf->data + (static_cast<size_t>(t) & buf->mask));
      if (!top_.compare_exchange_strong(
            t, t + 1u, std::memory_order_seq_cst, std::memory_order_relaxed
          )) {
        return false;
      }
      return true;
    }
    return false;
  }

  // Approximate size. Safe to call from any thread.
  size_t size_approx() const {
    uint32_t b = bottom_.load(std::memory_order_relaxed);
    uint32_t t = top_.load(std::memory_order_relaxed);
    // (b - t) is computed in uint32_t (defined wraparound) and
    // reinterpreted as int32_t for the signed sign check.
    int32_t s = static_cast<int32_t>(b - t);
    return s > 0 ? static_cast<size_t>(s) : 0;
  }

  // Approximate emptiness. Safe to call from any thread.
  bool empty() const {
    uint32_t b = bottom_.load(std::memory_order_relaxed);
    uint32_t t = top_.load(std::memory_order_relaxed);
    // (b - t) is computed in uint32_t (defined wraparound) and
    // reinterpreted as int32_t for the signed sign check.
    return static_cast<int32_t>(b - t) <= 0;
  }
};

} // namespace detail
} // namespace tmc
