// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

// Atomic bit manipulation in tmc::atomic namespace.
// Non-atomic bit manipulation in tmc::bit namespace.
// Needed because:
// - Clang and GCC may not generate "lock bt*" instructions
// - std:: BMI operations don't take size_t, requiring static_cast everywhere

#include "tmc/detail/compat.hpp"

#include <atomic>
#include <bit>

namespace tmc {
namespace atomic {

inline bool bit_set_test(std::atomic<size_t>& Addr, size_t BitIndex) {
  if constexpr (TMC_PLATFORM_BITS == 64) {
#if defined(_MSC_VER) && !defined(__clang__)
    return static_cast<bool>(_interlockedbittestandset64(
      reinterpret_cast<__int64*>(&Addr), static_cast<__int64>(BitIndex)
    ));
#else
    bool result;
    __asm__("lock btsq %2, %0"
            : "+m"(Addr), "=@ccc"(result)
            : "r"(BitIndex)
            : "memory", "cc");
    return result;
#endif
  } else {
    return Addr.fetch_or(TMC_ONE_BIT << BitIndex);
  }
}

// Sets the bit at BitIndex.
inline void bit_set(std::atomic<size_t>& Addr, size_t BitIndex) {
  Addr.fetch_or(TMC_ONE_BIT << BitIndex);
}

// Clears the bit at BitIndex.
// Returns true if the bit was 1 before modification.
inline bool bit_reset_test(std::atomic<size_t>& Addr, size_t BitIndex) {
  if constexpr (TMC_PLATFORM_BITS == 64) {
#if defined(_MSC_VER) && !defined(__clang__)
    return static_cast<bool>(_interlockedbittestandreset64(
      reinterpret_cast<__int64*>(&Addr), static_cast<__int64>(BitIndex)
    ));
#else
    bool result;
    __asm__ __volatile__("lock btrq %2, %0"
                         : "+m"(Addr), "=@ccc"(result)
                         : "r"(BitIndex)
                         : "memory", "cc");
    return result;
#endif
  } else {
    Addr.fetch_and(~(TMC_ONE_BIT << BitIndex));
  }
}

// Clears the bit at BitIndex.
inline void bit_reset(std::atomic<size_t>& Addr, size_t BitIndex) {
  Addr.fetch_and(~(TMC_ONE_BIT << BitIndex));
}

// // It is not clear whether _interlockedbittestandcomplement64 actually exists
// on MSVC, and we don't currently need this function.
// inline bool lock_btc(size_t& Addr, long long BitIndex) {
//   if constexpr (TMC_PLATFORM_BITS == 64) {
// #if defined(_MSC_VER) && !defined(__clang__)
//
//     return static_cast<bool>(_interlockedbittestandcomplement64(
//       static_cast<__int64*>(&Addr), static_cast<__int64>(BitIndex)
//     ));
// #else
//     bool result;
//     __asm__ __volatile__("lock btcq %2, %0"
//                          : "+m"(Addr), "=@ccc"(result)
//                          : "r"(BitIndex)
//                          : "memory", "cc");
//     return result;
// #endif
//   } else {
//     // emit regular std atomic operation here
//   }
// }

} // namespace atomic

namespace bit {

inline size_t blsr(size_t v) { return v - 1 & v; }

inline size_t blsi(size_t v) { return v & ~blsr(v); }

inline size_t lzcnt(size_t v) {
  return static_cast<size_t>(std::countl_zero(v));
}

inline size_t tzcnt(size_t v) {
  return static_cast<size_t>(std::countr_zero(v));
}

inline size_t popcnt(size_t v) { return static_cast<size_t>(std::popcount(v)); }

} // namespace bit
} // namespace tmc
