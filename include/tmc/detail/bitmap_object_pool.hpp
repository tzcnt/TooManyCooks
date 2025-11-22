// Copyright (c) 2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/bit_manip.hpp"
#include "tmc/detail/compat.hpp"

#include <array>
#include <atomic>

namespace tmc {
namespace detail {

/// Object pool that holds an unlimited number of objects.
/// Objects are lazily initialized; if all objects are currently checked out, a
/// new one will be created and returned. Objects are checked out in a LIFO
/// manner, so that the most-frequently used objects will remain hot in cache.
///
/// Usage:
/// 1. Call acquire_scoped(), which returns an object that wraps a reference to
/// a pool object, and automatically returns that object to the pool when it
/// goes out of scope.
/// 2. access the .value property of the scoped object to use it.
/// 3. Manually release() the object - this is required for the internal type to
/// be copyable out of get_hazard_ptr.
///
/// In either case, the references returned are directly to the objects stored
/// in the pool. Be careful not to accidentally move or copy this object, as
/// the original object is what will be returned to the pool afterward.
template <typename T, typename Derived> class BitmapObjectPoolImpl {
  // std::optional-like type that allocates space for an object
  // without managing its lifetime. 64-aligned to prevent false sharing.
  union alignas(64) pool_opt {
    T value;

    operator T&() & { return value; }
    operator const T&() const& { return value; }
    operator T&&() && { return static_cast<T&&>(value); }

    // Don't construct the contained object; the pool will do it.
    pool_opt() {}

    // Don't destroy the contained object; the pool will do it.
    ~pool_opt() {}

    pool_opt(const pool_opt&) = delete;
    pool_opt& operator=(const pool_opt&) = delete;
    pool_opt(pool_opt&&) = delete;
    pool_opt& operator=(pool_opt&&) = delete;
    TMC_DISABLE_WARNING_PADDED_BEGIN
  };
  TMC_DISABLE_WARNING_PADDED_END

  struct pool_block {
    std::array<pool_opt, TMC_PLATFORM_BITS> objects;
    std::atomic<size_t> available_bits;
    std::atomic<pool_block*> next;
    pool_block() : available_bits{0}, next{nullptr} {}

    T& get(size_t idx) { return objects[idx].value; }
  };

  pool_block* data;
  std::atomic<size_t> count;
  std::atomic<size_t> constructed_count;

  // Get or construct the next block.
  pool_block* next_block(pool_block* block) {
    pool_block* next = block->next.load(std::memory_order_acquire);
    if (next == nullptr) {
      pool_block* newBlock = new pool_block;
      if (block->next.compare_exchange_strong(
            next, newBlock, std::memory_order_acq_rel, std::memory_order_acquire
          )) {
        next = newBlock;
      } else {
        delete newBlock;
      }
    }
    return next;
  }

public:
  /// Constructs a new, empty object pool.
  BitmapObjectPoolImpl()
      : data{new pool_block}, count{0}, constructed_count{0} {}

  /// Destroy any objects that were created by the pool.
  virtual ~BitmapObjectPoolImpl() {
    size_t i = 0;
    pool_block* block = data;
    auto max = tmc::detail::atomic_load_latest(count);
    auto bits = block->available_bits.load();
    while (i < max) {
      auto bitIdx = i % TMC_PLATFORM_BITS;
      // Wait for any objects that are in use or under construction
      while ((bits & (TMC_ONE_BIT << bitIdx)) == 0) {
        TMC_CPU_PAUSE();
        bits = block->available_bits.load();
      }
      block->get(bitIdx).~T();
      ++i;
      if (i % TMC_PLATFORM_BITS == 0) {
        block = block->next.load();
        if (block == nullptr) {
          break;
        }
        bits = block->available_bits.load();
      }
    }

    block = data;
    while (block != nullptr) {
      auto next = block->next.load();
      delete block;
      block = next;
    }
  }

  /// Wrapper to an object pool reference (the .value field).
  /// When this goes out of scope, the object will be returned to the pool.
  class ScopedPoolObject {
    friend BitmapObjectPoolImpl;

  public:
    T& value;

  private:
    pool_block* block;
    size_t bit_idx;
    ScopedPoolObject(T& Value, pool_block* Block, size_t BitIdx)
        : value{Value}, block{Block}, bit_idx{BitIdx} {}

  public:
    // Manual release() is required so that this type can be trivially copyable
    void release() {
      tmc::detail::atomic_bit_set(block->available_bits, bit_idx);
    }
  };

  template <typename... Args>
  ScopedPoolObject
  new_object(pool_block* block, size_t blockEnd, Args&&... args) {
    auto idx = count.fetch_add(1);
    // We've now committed to constructing an object, but the count may have
    // been advanced by another thread. Ensure we are on the right block.
    while (idx >= blockEnd) [[unlikely]] {
      block = next_block(block);
      blockEnd += TMC_PLATFORM_BITS;
    }

    auto bitIdx = idx % TMC_PLATFORM_BITS;

    // Derived class implementation (using CRTP) constructs object in-place
    static_cast<Derived*>(this)->initialize(
      static_cast<void*>(&block->get(bitIdx)), static_cast<Args&&>(args)...
    );

    constructed_count.fetch_add(1);

    return ScopedPoolObject{block->get(bitIdx), block, bitIdx};
  }

  // Checks out an object from the pool, and returns a wrapper holding a
  // reference to that pool object, which can be accessed via the `.value`
  // field. When the wrapper goes out of scope, it will release the held
  // reference back to the pool.
  //
  // If all objects are in use, constructs a new one and adds it to the pool
  // before returning it. Any args provided will be forwarded to the
  // `initialize()` function of the derived class. In the default
  // implementation `BitmapObjectPool`, these args are forwarded to the
  // new object's constructor.
  template <typename... Args> ScopedPoolObject acquire_scoped(Args&&... args) {
    pool_block* block = data;
    size_t blockEnd = 0;
    auto bits = block->available_bits.load(std::memory_order_relaxed);
    while (true) {
      // Try to an object from the current block
      while (bits != 0) {
        auto newBits = tmc::detail::blsr(bits);
        auto bitIdx = tmc::detail::tzcnt(bits);

        // Try to take ownership of the lowest set bit (by clearing it).
        if (block->available_bits.compare_exchange_weak(
              bits, newBits, std::memory_order_seq_cst,
              std::memory_order_relaxed
            )) {
          return ScopedPoolObject{block->get(bitIdx), block, bitIdx};
        }
      }

      // Advance to the next block and try again
      blockEnd += TMC_PLATFORM_BITS;
      auto currCount = count.load(std::memory_order_relaxed);
      if (currCount >= blockEnd) {
        block = next_block(block);
        bits = block->available_bits.load(std::memory_order_relaxed);
      } else {
        // No elements remain. Construct one.
        return new_object(block, blockEnd, static_cast<Args&&>(args)...);
      }
    }
  }

  // acquire_scoped "with forward progress guarantee"
  // After InitialAttempts failures due to contention, we switch to an advancing
  // algorithm that cannot retry the same bit again.
  template <size_t InitialAttempts = 1, typename... Args>
  ScopedPoolObject acquire_scoped_wfpg(Args&&... args) {
    pool_block* block = data;
    size_t blockEnd = 0;
    size_t attempts = 0;
    auto bits = block->available_bits.load(std::memory_order_relaxed);
    size_t maskedBits = bits;
    while (true) {
      // Try to an object from the current block
      while (maskedBits != 0) {
        auto bitIdx = tmc::detail::tzcnt(maskedBits);
        auto newBits = bits & ~(TMC_ONE_BIT << bitIdx);

        // Try to take ownership of the lowest set bit (by clearing it).
        if (block->available_bits.compare_exchange_strong(
              bits, newBits, std::memory_order_seq_cst,
              std::memory_order_relaxed
            )) {
          return ScopedPoolObject{block->get(bitIdx), block, bitIdx};
        } else if (attempts < InitialAttempts) {
          maskedBits = bits;
          // Failed cmpxchg means contention; consume an attempt.
          ++attempts;
          continue;
        } else {
          // Guarantee forward progress by masking off all bits that have
          // already been checked, or lower.
          maskedBits = bits & ((TMC_ALL_ONES - 1) << bitIdx);
        }
      }

      // Advance to the next block and try again
      blockEnd += TMC_PLATFORM_BITS;
      auto currCount = count.load(std::memory_order_relaxed);
      if (currCount >= blockEnd) {
        block = next_block(block);
        bits = block->available_bits.load(std::memory_order_relaxed);
        maskedBits = bits;
      } else {
        // No elements remain. Construct one.
        return new_object(block, blockEnd, static_cast<Args&&>(args)...);
      }
    }
  }

  // Acquire each currently available object of the list one-by-one and
  // call func(object). Objects that are currently in use by another thread
  // will not be processed.
  template <typename Fn> void for_each_available(Fn func) {
    auto max = count.load(std::memory_order_relaxed);
    size_t i = 0;
    pool_block* block = data;
    while (i < max) {
      auto bitIdx = i % TMC_PLATFORM_BITS;
      // Try to clear this bit to take ownership of the object.
      // If it was already clear, nothing happens.
      if (tmc::detail::atomic_bit_reset_test(block->available_bits, bitIdx)) {
        // We now own this object. Run the caller's functor on it.
        func(block->get(bitIdx));
        // Now release the object
        tmc::detail::atomic_bit_set(block->available_bits, bitIdx);
      }
      ++i;
      if (i % TMC_PLATFORM_BITS == 0) {
        block = block->next.load();
        if (block == nullptr) {
          return;
        }
      }
    }
  }

  // Call func on every element of the pool without taking ownership of it, even
  // if it's checked out by someone else.
  template <typename Fn> void for_each_unsafe(Fn func) {
    auto max = constructed_count.load(std::memory_order_relaxed);
    pool_block* block = data;
    size_t i = 0;
    while (i < max) {
      auto bitIdx = i % TMC_PLATFORM_BITS;
      func(block->get(bitIdx));
      ++i;
      if (i % TMC_PLATFORM_BITS == 0) {
        block = block->next.load();
        if (block == nullptr) {
          return;
        }
      }
    }
  }

  // Call func only on elements that are currently checked out.
  // Returns early if pred() returns false.
  template <typename Pred, typename Func>
  void for_each_in_use(Pred pred, Func func) noexcept {
    auto max = constructed_count.load(std::memory_order_relaxed);
    pool_block* block = data;
    size_t i = 0;
    while (i < max) {
      if (!pred()) {
        return;
      }
      auto bits = block->available_bits.load();
      auto bitIdx = i % TMC_PLATFORM_BITS;
      auto bit = TMC_ONE_BIT << bitIdx;
      if ((bits & bit) == 0) {
        func(block->get(bitIdx));
      }
      ++i;
      if (i % TMC_PLATFORM_BITS == 0) {
        block = block->next.load();
        if (block == nullptr) {
          return;
        }
      }
    }
  }
};

/// A default implementation of `BitmapObjectPoolImpl` is provided, which just
/// default-initializes objects when they are created in the pool.
///
/// You can also derive from `BitmapObjectPoolImpl` directly and implement
/// `initialize()` yourself, to customize how new pool objects are created.
/// Your implementation must at least construct the object at the provided
/// location using placement new.
template <typename T>
class BitmapObjectPool : public BitmapObjectPoolImpl<T, BitmapObjectPool<T>> {
  friend class BitmapObjectPoolImpl<T, BitmapObjectPool<T>>;
  template <typename... Args> void initialize(void* location, Args&&... args) {
    ::new (location) T(static_cast<Args&&>(args)...);
  }
};

} // namespace detail
} // namespace tmc
