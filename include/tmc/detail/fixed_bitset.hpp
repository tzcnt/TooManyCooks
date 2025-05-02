// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

// A fixed-size bitset used to track thread states. Fixed size allows us to
// avoid extra indirections while still offering more than enough room for the
// core count in modern architectures (1024).

#include "tmc/detail/compat.hpp"

#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <cstddef>

namespace tmc {
namespace detail {

template <size_t MaxThreads> class fixed_atomic_bitset;

// Provides a snapshot of atomic data from a specific point in time
// returned by fixed_atomic_bitset::load()
template <size_t MaxThreads> class fixed_bitset {
  static_assert(0 == MaxThreads % TMC_PLATFORM_BITS);
  static inline constexpr size_t Capacity = MaxThreads / TMC_PLATFORM_BITS;
  // the number of active data elements (dword or qword), not the number of bits
  size_t size_ = 0;
  std::array<size_t, Capacity> data_;

  friend fixed_atomic_bitset<MaxThreads>;

public:
  size_t size() { return size_; }

  // Selects a single bit using a mask, but doesn't shift it down to the 0 index
  // position.
  size_t get(size_t Slot) {
    return data_[Slot / TMC_PLATFORM_BITS] &
           (TMC_ONE_BIT << (Slot & (TMC_PLATFORM_BITS - 1)));
  }

  inline fixed_bitset operator|(fixed_bitset const& Other) {
    fixed_bitset result;
    result.size_ = size_;
    for (size_t i = 0; i < Capacity; ++i) {
      result.data_[i] = data_[i] | Other.data_[i];
    }
    return result;
  }

  inline fixed_bitset operator~() {
    fixed_bitset result;
    result.size_ = size_;
    for (size_t i = 0; i < Capacity; ++i) {
      result.data_[i] = ~data_[i];
    }
    return result;
  }

  inline void set_bit(size_t Slot) {
    assert(Slot < size_ * TMC_PLATFORM_BITS);
    data_[Slot / TMC_PLATFORM_BITS] |=
      (TMC_ONE_BIT << (Slot & (TMC_PLATFORM_BITS - 1)));
  }

  inline void clr_bit(size_t Slot) {
    assert(Slot < size_ * TMC_PLATFORM_BITS);
    data_[Slot / TMC_PLATFORM_BITS] &=
      (~(TMC_ONE_BIT << (Slot & (TMC_PLATFORM_BITS - 1))));
  }

  inline size_t popcount() {
    size_t count = 0;
    for (size_t i = 0; i < size_; ++i) {
      count += std::popcount(data_[i]);
    }
    return count;
  }

  inline size_t countr_zero() {
    for (size_t i = 0; i < size_; ++i) {
      size_t idx = std::countr_zero(data_[0]);
      if (idx != TMC_PLATFORM_BITS) {
        return idx + i * TMC_PLATFORM_BITS;
      }
    }
    return size_ * TMC_PLATFORM_BITS;
  }
};

template <size_t MaxThreads> class fixed_atomic_bitset {
  static_assert(0 == MaxThreads % TMC_PLATFORM_BITS);
  static inline constexpr size_t Capacity = MaxThreads / TMC_PLATFORM_BITS;
  // the number of active data elements (dword or qword), not the number of bits
  size_t size_ = 0;
  std::array<std::atomic<size_t>, Capacity> data_;

public:
  size_t size() { return size_; }

  void init(size_t BitCount, bool Value) {
    size_ = 1 + BitCount / TMC_PLATFORM_BITS;
    if (0 == BitCount % TMC_PLATFORM_BITS) {
      --size_;
    }
    for (size_t i = 0; i < Capacity; ++i) {
      data_[i] = 0;
    }
    if (true == Value) {
      size_t count = 0;
      // ptrdiff_t count = static_cast<ptrdiff_t>(BitCount);
      size_t i = 0;
      while (count + TMC_PLATFORM_BITS <= BitCount) {
        data_[i] = TMC_ALL_ONES;
        count += TMC_PLATFORM_BITS;
        ++i;
      }
      if (count < BitCount) {
        count = BitCount - count;
        data_[i] =
          (TMC_ONE_BIT << (count - 1)) | ((TMC_ONE_BIT << (count - 1)) - 1);
        ++i;
      }
      // Unnecessary - the rest were already zeroed
      // for (; i < Capacity; ++i) {
      //   data_[i] = 0;
      // }
    }
  }

  void teardown() { size_ = 0; }

  inline void
  set_bit(size_t Slot, std::memory_order mo = std::memory_order_relaxed) {
    assert(Slot < size_ * TMC_PLATFORM_BITS);
    data_[Slot / TMC_PLATFORM_BITS].fetch_or(
      TMC_ONE_BIT << (Slot & (TMC_PLATFORM_BITS - 1)), mo
    );
  }

  inline void
  clr_bit(size_t Slot, std::memory_order mo = std::memory_order_relaxed) {
    assert(Slot < size_ * TMC_PLATFORM_BITS);
    data_[Slot / TMC_PLATFORM_BITS].fetch_and(
      ~(TMC_ONE_BIT << (Slot & (TMC_PLATFORM_BITS - 1))), mo
    );
  }

  inline size_t popcount(std::memory_order mo = std::memory_order_relaxed) {
    size_t count = 0;
    for (size_t i = 0; i < size_; ++i) {
      count += std::popcount(data_[i].load(mo));
    }
    return count;
  }

  inline size_t countr_zero(std::memory_order mo = std::memory_order_relaxed) {
    for (size_t i = 0; i < size_; ++i) {
      size_t idx = std::countr_zero(data_[0].load(mo));
      if (idx != TMC_PLATFORM_BITS) {
        return idx + i * TMC_PLATFORM_BITS;
      }
    }
    return size_ * TMC_PLATFORM_BITS;
  }

  fixed_bitset<MaxThreads>
  load(std::memory_order mo = std::memory_order_relaxed) {
    fixed_bitset<MaxThreads> result;
    result.size_ = size_;
    for (size_t i = 0; i < Capacity; ++i) {
      result.data_[i] = data_[i].load(mo);
    }
    return result;
  }
};
} // namespace detail
} // namespace tmc
