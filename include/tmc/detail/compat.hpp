// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include <atomic>
#include <cstddef>

#if defined(_MSC_VER)

#ifdef __has_cpp_attribute

#if __has_cpp_attribute(msvc::forceinline)
#define TMC_FORCE_INLINE [[msvc::forceinline]]
#else
#define TMC_FORCE_INLINE
#endif

#if __has_cpp_attribute(msvc::no_unique_address)
#define TMC_NO_UNIQUE_ADDRESS [[msvc::no_unique_address]]
#else
#define TMC_NO_UNIQUE_ADDRESS
#endif

#else // not __has_cpp_attribute
#define TMC_FORCE_INLINE [[msvc::forceinline]]
#define TMC_NO_UNIQUE_ADDRESS [[msvc::no_unique_address]]
#endif
#else // not _MSC_VER
#define TMC_FORCE_INLINE __attribute__((always_inline))
#define TMC_NO_UNIQUE_ADDRESS [[no_unique_address]]
#endif

#if !defined TMC_NO_EXCEPTIONS && __cpp_exceptions == 199711
#define TMC_HAS_EXCEPTIONS 1
#else
#define TMC_HAS_EXCEPTIONS 0
#endif

#ifdef __has_cpp_attribute

#if __has_cpp_attribute(clang::coro_await_elidable_argument) &&                \
  __has_cpp_attribute(clang::coro_await_elidable_argument)
#define TMC_CORO_AWAIT_ELIDABLE [[clang::coro_await_elidable]]
#define TMC_CORO_AWAIT_ELIDABLE_ARGUMENT [[clang::coro_await_elidable_argument]]
#else
#define TMC_CORO_AWAIT_ELIDABLE
#define TMC_CORO_AWAIT_ELIDABLE_ARGUMENT
#endif

#else // not __has_cpp_attribute
#define TMC_CORO_AWAIT_ELIDABLE
#define TMC_CORO_AWAIT_ELIDABLE_ARGUMENT
#endif

#if defined(__cpp_sized_deallocation)
#define TMC_SIZED_DEALLOCATION __cpp_sized_deallocation
#else
#define TMC_SIZED_DEALLOCATION 0
#endif

#if defined(__x86_64__) || defined(_M_AMD64) || defined(i386) ||               \
  defined(__i386__) || defined(__i386) || defined(_M_IX86)
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <immintrin.h>
#endif
#define TMC_CPU_X86
#define TMC_CPU_PAUSE _mm_pause
static inline size_t TMC_CPU_TIMESTAMP() noexcept {
  return static_cast<size_t>(__rdtsc());
}
// Assume a 3.5GHz CPU if we can't get the value (on x86).
// Yes, this is hacky. Getting the real RDTSC freq requires
// waiting for another time source (system timer) and then dividing by
// that duration. This takes real time and would have to be done on
// startup. Using a 3.5GHz default means that slower processors will appear to
// be running faster, and vice versa. For the current usage of this (the
// clustering threshold in tmc::channel) this seems like reasonable behavior
// anyway.
static inline const size_t TMC_CPU_FREQ = 3500000000;
#elif defined(__arm__) || defined(_M_ARM) || defined(_M_ARM64) ||              \
  defined(__aarch64__) || defined(__ARM_ACLE)
#define TMC_CPU_ARM
static inline void TMC_CPU_PAUSE() noexcept {
  // Clang defines __yield intrinsic, but GCC doesn't, so we use asm
  asm volatile("yield");
}
// Read the ARM "Virtual Counter" register.
// This ticks at a frequency independent of the processor frequency.
// https://developer.arm.com/documentation/ddi0406/cb/System-Level-Architecture/The-Generic-Timer/About-the-Generic-Timer/The-virtual-counter?lang=en
static inline size_t TMC_CPU_TIMESTAMP() noexcept {
  size_t count;
  asm volatile("mrs %0, cntvct_el0; " : "=r"(count)::"memory");
  return count;
}
// Read the ARM "Virtual Counter" frequency.
static inline size_t TMC_ARM_CPU_FREQ() noexcept {
  size_t freq;
  asm volatile("mrs %0, cntfrq_el0; isb; " : "=r"(freq)::"memory");
  return freq;
}
static inline const size_t TMC_CPU_FREQ = TMC_ARM_CPU_FREQ();
#endif

// clang-format tries to collapse the pragmas into one line...
// clang-format off
#if defined(__clang__)
#define TMC_DISABLE_WARNING_PADDED_BEGIN
#define TMC_DISABLE_WARNING_PADDED_END
#elif defined(__GNUC__)
#define TMC_DISABLE_WARNING_PADDED_BEGIN
#define TMC_DISABLE_WARNING_PADDED_END
#elif defined(_MSC_VER)
#define TMC_DISABLE_WARNING_PADDED_BEGIN \
_Pragma("warning(push)") \
_Pragma("warning(disable : 4324)")
#define TMC_DISABLE_WARNING_PADDED_END _Pragma("warning(pop)")
#else
#define TMC_DISABLE_WARNING_PADDED_BEGIN
#define TMC_DISABLE_WARNING_PADDED_END
#endif

#if defined(__clang__)
#define TMC_DISABLE_WARNING_PESSIMIZING_MOVE_BEGIN
#define TMC_DISABLE_WARNING_PESSIMIZING_MOVE_END
#elif defined(__GNUC__)
#define TMC_DISABLE_WARNING_PESSIMIZING_MOVE_BEGIN                             \
_Pragma("GCC diagnostic push")                                               \
_Pragma("GCC diagnostic ignored \"-Wpessimizing-move\"")
#define TMC_DISABLE_WARNING_PESSIMIZING_MOVE_END _Pragma("GCC diagnostic pop")
#elif defined(_MSC_VER)
#define TMC_DISABLE_WARNING_PESSIMIZING_MOVE_BEGIN
#define TMC_DISABLE_WARNING_PESSIMIZING_MOVE_END
#else
#define TMC_DISABLE_WARNING_PESSIMIZING_MOVE_BEGIN
#define TMC_DISABLE_WARNING_PESSIMIZING_MOVE_END
#endif

// This repo uses -Wswitch instead to require exhaustive switch
#if defined(__clang__)
#define TMC_DISABLE_WARNING_SWITCH_DEFAULT_BEGIN                             \
_Pragma("clang diagnostic push")                                               \
_Pragma("clang diagnostic ignored \"-Wswitch-default\"")
#define TMC_DISABLE_WARNING_SWITCH_DEFAULT_END _Pragma("clang diagnostic pop")
#elif defined(__GNUC__)
#define TMC_DISABLE_WARNING_SWITCH_DEFAULT_BEGIN
#define TMC_DISABLE_WARNING_SWITCH_DEFAULT_END
#elif defined(_MSC_VER)
#define TMC_DISABLE_WARNING_SWITCH_DEFAULT_BEGIN
#define TMC_DISABLE_WARNING_SWITCH_DEFAULT_END
#else
#define TMC_DISABLE_WARNING_SWITCH_DEFAULT_BEGIN
#define TMC_DISABLE_WARNING_SWITCH_DEFAULT_END
#endif
// clang-format on

namespace tmc::detail {
TMC_FORCE_INLINE inline void memory_barrier() noexcept {
#ifdef TMC_CPU_X86
  std::atomic<size_t> locker;
  locker.fetch_add(0, std::memory_order_seq_cst);
#else
  std::atomic_thread_fence(std::memory_order_seq_cst);
#endif
}
} // namespace tmc::detail

// `1 << 40` is undefined behavior on 64-bit systems because the type of `1`
// defaults to 32-bit `int`. So if we want to left-shift into a 64-bit mask, we
// need to cast the literal.
#define TMC_ONE_BIT static_cast<size_t>(1)

#define TMC_ALL_ONES static_cast<size_t>(-1)

#define NO_HINT static_cast<size_t>(-1)

static inline constexpr size_t TMC_PLATFORM_BITS =
  sizeof(size_t) * 8; // 32 or 64

static inline constexpr size_t TMC_MAX_PRIORITY_COUNT = 16;

#ifdef TMC_PRIORITY_COUNT
#define TMC_PRIORITY_CONSTEXPR constexpr
#else
#define TMC_PRIORITY_CONSTEXPR
#endif

namespace tmc {
namespace detail {
#ifdef __linux__
// Linux only efficiently implements std::atomic::wait() for 32 bits.
using atomic_wait_t = int;
#else
// Windows requires 64 bits. MacOS can use either.
using atomic_wait_t = ptrdiff_t;
#endif
} // namespace detail
} // namespace tmc
