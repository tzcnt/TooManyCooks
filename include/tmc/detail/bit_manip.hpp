// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

// Atomic and non-atomic bit manipulation functions.
// Needed because:
// - Clang and GCC may not generate "lock bt*" instructions
// - std:: BMI operations don't take size_t, requiring static_cast everywhere

#include "tmc/detail/compat.hpp"

#include <atomic>
#include <bit>

namespace tmc {
namespace detail {

// Sets the bit at BitIndex.
// Returns true if the bit was 1 before modification.
inline bool atomic_bit_set_test(std::atomic<size_t>& Addr, size_t BitIndex) {
#if defined(TMC_CPU_X86)
  if constexpr (TMC_PLATFORM_BITS == 64) {
#if defined(_MSC_VER) && !defined(__clang__)
    return static_cast<bool>(_interlockedbittestandset64(
      reinterpret_cast<__int64*>(&Addr), static_cast<__int64>(BitIndex)
    ));
#else
    bool result;
    asm volatile("lock btsq %2, %0"
                 : "+m"(Addr), "=@ccc"(result)
                 : "r"(BitIndex)
                 : "memory", "cc");
    return result;
#endif
  } else {
    return 0 !=
           (Addr.fetch_or(TMC_ONE_BIT << BitIndex) & (TMC_ONE_BIT << BitIndex));
  }
#else
  return 0 !=
         (Addr.fetch_or(TMC_ONE_BIT << BitIndex) & (TMC_ONE_BIT << BitIndex));
#endif
}

// Sets the bit at BitIndex.
inline void atomic_bit_set(std::atomic<size_t>& Addr, size_t BitIndex) {
  Addr.fetch_or(TMC_ONE_BIT << BitIndex);
}

// Clears the bit at BitIndex.
// Returns true if the bit was 1 before modification.
inline bool atomic_bit_reset_test(std::atomic<size_t>& Addr, size_t BitIndex) {
#if defined(TMC_CPU_X86)
  if constexpr (TMC_PLATFORM_BITS == 64) {
#if defined(_MSC_VER) && !defined(__clang__)
    return static_cast<bool>(_interlockedbittestandreset64(
      reinterpret_cast<__int64*>(&Addr), static_cast<__int64>(BitIndex)
    ));
#else
    bool result;
    asm volatile("lock btrq %2, %0"
                 : "+m"(Addr), "=@ccc"(result)
                 : "r"(BitIndex)
                 : "memory", "cc");
    return result;
#endif
  } else {
    return 0 != (Addr.fetch_and(~(TMC_ONE_BIT << BitIndex)) &
                 (TMC_ONE_BIT << BitIndex));
  }
#else
  return 0 != (Addr.fetch_and(~(TMC_ONE_BIT << BitIndex)) &
               (TMC_ONE_BIT << BitIndex));
#endif
}

// Clears the bit at BitIndex.
inline void atomic_bit_reset(std::atomic<size_t>& Addr, size_t BitIndex) {
  Addr.fetch_and(~(TMC_ONE_BIT << BitIndex));
}

// // It is not clear whether _interlockedbittestandcomplement64 actually exists
// // on MSVC, and we don't currently need this function.
// inline bool atomic_lock_btc(size_t& Addr, long long BitIndex) {
// #if defined(TMC_CPU_X86)
//   if constexpr (TMC_PLATFORM_BITS == 64) {
// #if defined(_MSC_VER) && !defined(__clang__)

//     return static_cast<bool>(_interlockedbittestandcomplement64(
//       static_cast<__int64*>(&Addr), static_cast<__int64>(BitIndex)
//     ));
// #else
//     bool result;
//     asm volatile("lock btcq %2, %0"
//                          : "+m"(Addr), "=@ccc"(result)
//                          : "r"(BitIndex)
//                          : "memory", "cc");
//     return result;
// #endif
//   } else {
//    return 0 !=
//           (Addr.fetch_xor(TMC_ONE_BIT << BitIndex) & (TMC_ONE_BIT <<
//           BitIndex));
//   }
// #else
// return 0 !=
//       (Addr.fetch_xor(TMC_ONE_BIT << BitIndex) & (TMC_ONE_BIT << BitIndex));
// #endif
// }

// Make sure that we really have the latest value of a variable.
// Slow, but useful in certain contexts where we don't have any other
// happens-before guarantees to lean on (e.g. destructors).
inline size_t atomic_load_latest(std::atomic<size_t>& Addr) {
  size_t result = 0;
  Addr.compare_exchange_strong(
    result, result, std::memory_order_seq_cst, std::memory_order_seq_cst
  );
  return result;
}

/************** Non-atomic operations  ****************/

/// Clear the lowest set bit
inline size_t blsr(size_t v) { return (v - 1) & v; }

/// Isolate the lowest set bit. All other bits are cleared.
inline size_t blsi(size_t v) { return v & ~blsr(v); }

/// Count the number of leading zero bits.
inline size_t lzcnt(size_t v) {
  return static_cast<size_t>(std::countl_zero(v));
}

/// Count the number of trailing zero bits.
inline size_t tzcnt(size_t v) {
  return static_cast<size_t>(std::countr_zero(v));
}

/// Count the number of set bits.
inline size_t popcnt(size_t v) { return static_cast<size_t>(std::popcount(v)); }

} // namespace detail
} // namespace tmc
