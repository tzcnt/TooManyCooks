// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

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

#if __has_cpp_attribute(clang::coro_await_elidable) &&                                   \
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

#ifdef __has_cpp_attribute

#if __has_cpp_attribute(clang::lifetimebound)
#define TMC_LIFETIMEBOUND [[clang::lifetimebound]]
#else
#define TMC_LIFETIMEBOUND
#endif

#else // not __has_cpp_attribute
#define TMC_LIFETIMEBOUND
#endif

#if defined(__cpp_sized_deallocation)
#define TMC_SIZED_DEALLOCATION __cpp_sized_deallocation
#else
#define TMC_SIZED_DEALLOCATION 0
#endif

#if defined(__x86_64__) || defined(_M_AMD64) || defined(i386) || defined(__i386__) ||    \
  defined(__i386) || defined(_M_IX86)
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <immintrin.h>
#endif
#define TMC_CPU_X86
#define TMC_CPU_PAUSE _mm_pause
#elif defined(__arm__) || defined(_M_ARM) || defined(_M_ARM64) || defined(__aarch64__)
#if defined(__aarch64__) || defined(_M_ARM64) ||                                         \
  (defined(__ARM_ARCH_PROFILE) && __ARM_ARCH_PROFILE == 'A') ||                          \
  (defined(__ARM_ARCH_PROFILE) && __ARM_ARCH_PROFILE == 'R')
#define TMC_CPU_ARM
#endif
#ifdef _MSC_VER
#include <intrin.h>
#endif
static inline void TMC_CPU_PAUSE() noexcept {
  // Clang defines __yield intrinsic, but GCC doesn't, so we use asm where we
  // know the ARM yield instruction is available.
#if defined(__aarch64__) || defined(_M_ARM64) || (defined(__ARM_ARCH) && __ARM_ARCH >= 7)
#if defined(_MSC_VER)
  __yield();
#else
  asm volatile("yield");
#endif
#endif
}
#elif defined(__loongarch__) && defined(__LP64__)
// Use some barrier instructions to generate a delay, as there's
// no instruction dedicated for the delay in spinlock :(.
static inline void TMC_CPU_PAUSE() noexcept {
  for (int i = 0; i < 32; i++)
    asm volatile("ibar 0");
}
#elif defined(__riscv)
#define TMC_CPU_RISCV
static inline void TMC_CPU_PAUSE() noexcept {
#if defined(__riscv_zihintpause)
  asm volatile("pause" ::: "memory");
#else
  asm volatile("nop" ::: "memory");
#endif
}
#else
// Fallback for unknown architectures
static inline void TMC_CPU_PAUSE() noexcept {}
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

// The lifetime-safety suggestion engine proposes [[clang::lifetimebound]]
// per-instantiation. For generic forwarding functions (await_transform /
// get_awaiter), some instantiations bind the returned awaiter to the parameter
// (awaiters that reference the awaitable), while others do not (awaiters that
// take ownership of the awaitable, e.g. aw_task). A single annotation cannot
// satisfy both: applying it trips -Wlifetime-safety-lifetimebound-violation on
// the owning instantiations. Suppress the suggestion at those sites instead.
#if defined(__clang__)
#if __has_warning("-Wlifetime-safety-suggestions")
#define TMC_DISABLE_WARNING_LIFETIME_SUGGESTIONS_BEGIN \
_Pragma("clang diagnostic push") \
_Pragma("clang diagnostic ignored \"-Wlifetime-safety-suggestions\"")
#define TMC_DISABLE_WARNING_LIFETIME_SUGGESTIONS_END _Pragma("clang diagnostic pop")
#endif
#endif
#ifndef TMC_DISABLE_WARNING_LIFETIME_SUGGESTIONS_BEGIN
#define TMC_DISABLE_WARNING_LIFETIME_SUGGESTIONS_BEGIN
#define TMC_DISABLE_WARNING_LIFETIME_SUGGESTIONS_END
#endif

#if defined(__clang__)
#if __has_warning("-Wlifetime-safety-invalidation")
#define TMC_DISABLE_WARNING_LIFETIME_INVALIDATION_BEGIN \
_Pragma("clang diagnostic push") \
_Pragma("clang diagnostic ignored \"-Wlifetime-safety-invalidation\"")
#define TMC_DISABLE_WARNING_LIFETIME_INVALIDATION_END _Pragma("clang diagnostic pop")
#endif
#endif
#ifndef TMC_DISABLE_WARNING_LIFETIME_INVALIDATION_BEGIN
#define TMC_DISABLE_WARNING_LIFETIME_INVALIDATION_BEGIN
#define TMC_DISABLE_WARNING_LIFETIME_INVALIDATION_END
#endif

// We like to pack pointers and flags into single words for performance.
// This depends on the expectation that nullptr == 0.
#if defined(__clang__)
_Pragma("clang diagnostic push")                                               
_Pragma("clang diagnostic ignored \"-Wzero-as-null-pointer-constant\"")
  static_assert(nullptr == 0, "nullptr is not 0 on this platform");
_Pragma("clang diagnostic pop")
#elif defined(__GNUC__)
_Pragma("GCC diagnostic push")                                               
_Pragma("GCC diagnostic ignored \"-Wzero-as-null-pointer-constant\"")
  static_assert(nullptr == 0, "nullptr is not 0 on this platform");
_Pragma("GCC diagnostic pop")
#elif defined(_MSC_VER)
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

#if SIZE_MAX == 0xFFFFFFFFu
#define TMC_PLATFORM_BITS 32
#else
#define TMC_PLATFORM_BITS 64
#endif

inline constexpr size_t TMC_MAX_PRIORITY_COUNT = 16;

#ifdef TMC_PRIORITY_COUNT
#define TMC_PRIORITY_CONSTEXPR constexpr
#else
#define TMC_PRIORITY_CONSTEXPR
#endif

// Apple M-series have 128-byte cache lines.
// This is not properly represented in
// `std::hardware_destructive_interference_size`
// on the current latest version of Apple Clang.
// https://stackoverflow.com/questions/79579748/is-stdhardware-destructive-interference-size-right-for-macs
#if defined(__aarch64__) && defined(__APPLE__)
inline constexpr size_t TMC_CACHE_LINE_SIZE = 128;
#else
// GCC warns if we use std::hardware_destructive_interference_size here, so just
// use 64 instead. This is correct for the foreseeable future.
inline constexpr size_t TMC_CACHE_LINE_SIZE = 64;
#endif

namespace tmc {
namespace detail {
#ifdef __linux__
// Linux only efficiently implements std::atomic::wait() for 32 bits.
using atomic_wait_t = int;
// Unsigned value to have defined overflow behavior
using atomic_waker_t = unsigned int;
#else
// Windows requires 64 bits. MacOS can use either.
using atomic_wait_t = ptrdiff_t;
using atomic_waker_t = size_t;
#endif
} // namespace detail
} // namespace tmc

static_assert(std::atomic<void*>::is_always_lock_free);
static_assert(std::atomic<uintptr_t>::is_always_lock_free);
static_assert(std::atomic<size_t>::is_always_lock_free);
static_assert(std::atomic<tmc::detail::atomic_wait_t>::is_always_lock_free);
static_assert(std::atomic<tmc::detail::atomic_waker_t>::is_always_lock_free);

#ifdef TMC_NODISCARD_AWAIT
#define TMC_AWAIT_RESUME [[nodiscard]]
#else
#define TMC_AWAIT_RESUME
#endif
