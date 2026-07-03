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
#if defined(_MSC_VER) && defined(_M_ARM64)
#define TMC_ARM64_SYSREG(op0, op1, crn, crm, op2)                               \
  (((op0 & 1) << 14) | ((op1 & 7) << 11) | ((crn & 15) << 7) |                  \
   ((crm & 15) << 3) | ((op2 & 7) << 0))
static inline size_t TMC_CPU_TIMESTAMP() noexcept {
  return static_cast<size_t>(_ReadStatusReg(TMC_ARM64_SYSREG(3, 3, 14, 0, 2)));
}
static inline size_t TMC_ARM_CPU_FREQ() noexcept {
  return static_cast<size_t>(_ReadStatusReg(TMC_ARM64_SYSREG(3, 3, 14, 0, 0)));
}
#undef TMC_ARM64_SYSREG
#elif defined(_MSC_VER) && defined(_M_ARM)
static inline size_t TMC_CPU_TIMESTAMP() noexcept {
  return static_cast<size_t>(_MoveFromCoprocessor64(15, 1, 14));
}
static inline size_t TMC_ARM_CPU_FREQ() noexcept {
  return static_cast<size_t>(_MoveFromCoprocessor(15, 0, 14, 0, 0));
}
#elif defined(__aarch64__) || defined(_M_ARM64)
// AArch64: the generic timer is read through dedicated system registers.
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
#elif defined(__ARM_ARCH) && __ARM_ARCH >= 7 && defined(__ARM_ARCH_PROFILE) &&           \
  (__ARM_ARCH_PROFILE == 'A' || __ARM_ARCH_PROFILE == 'R')
#if defined(__linux__)
#include <asm/hwcap.h>
#include <sys/auxv.h>
#endif
// AArch32 (e.g. armhf): the AArch64 `cntvct_el0` / `cntfrq_el0` system
// registers don't exist here. The ARMv7-A Generic Timer exposes the same
// counters through CP15 coprocessor accesses instead.
// https://developer.arm.com/documentation/ddi0406/cb/System-Level-Architecture/The-Generic-Timer/Register-descriptions?lang=en
// The Generic Timer is optional on ARMv7-A, and user-mode access is controlled
// by the OS. On Linux, HWCAP_EVTSTRM indicates that the kernel configured the
// architected timer event stream; if that is absent, fall back to inert timing
// rather than trapping on an undefined/privileged CP15 access.
static inline bool TMC_ARM_HAS_GENERIC_TIMER_IMPL() noexcept {
#if defined(__linux__) && defined(HWCAP_EVTSTRM)
  return (getauxval(AT_HWCAP) & HWCAP_EVTSTRM) != 0;
#else
  return false;
#endif
}
static inline const bool TMC_ARM_HAS_GENERIC_TIMER = TMC_ARM_HAS_GENERIC_TIMER_IMPL();
// Read the 64-bit "Virtual Counter" (CNTVCT) via MRRC into a register pair.
// size_t is 32-bit here, so we return only the low word - matching the 32-bit
// x86 path, which likewise truncates __rdtsc(). The counter ticks at a
// frequency independent of the processor frequency.
static inline size_t TMC_CPU_TIMESTAMP() noexcept {
  if (!TMC_ARM_HAS_GENERIC_TIMER) {
    return 0;
  }
  uint32_t lo, hi;
  asm volatile("mrrc p15, 1, %0, %1, c14" : "=r"(lo), "=r"(hi)::"memory");
  (void)hi;
  return lo;
}
// Read the "Virtual Counter" frequency (CNTFRQ, a 32-bit register) via MRC.
static inline size_t TMC_ARM_CPU_FREQ() noexcept {
  if (!TMC_ARM_HAS_GENERIC_TIMER) {
    return 1000000000;
  }
  uint32_t freq;
  asm volatile("mrc p15, 0, %0, c14, c0, 0" : "=r"(freq)::"memory");
  return freq;
}
#else
static inline size_t TMC_CPU_TIMESTAMP() noexcept { return 0; }
static inline size_t TMC_ARM_CPU_FREQ() noexcept { return 1000000000; }
#endif
static inline const size_t TMC_CPU_FREQ = TMC_ARM_CPU_FREQ();
#elif defined(__loongarch__) && defined(__LP64__)
// Use some barrier instructions to generate a delay, as there's
// no instruction dedicated for the delay in spinlock :(.
static inline void TMC_CPU_PAUSE() noexcept {
  for (int i = 0; i < 32; i++)
    asm volatile("ibar 0");
}
// Read the LoongArch stable counter
static inline size_t TMC_CPU_TIMESTAMP() noexcept {
  size_t count;
  asm volatile("rdtime.d %0, $zero" : "=r"(count));
  return count;
}
// Calculate the LoongArch stable counter frequency
static inline size_t TMC_LOONGARCH_CPU_FREQ() noexcept {
  size_t cc_freq;
  asm("cpucfg %0, %1" : "=r"(cc_freq) : "r"(0x4));

  size_t cc_mul_div;
  asm("cpucfg %0, %1" : "=r"(cc_mul_div) : "r"(0x5));

  size_t cc_mul = cc_mul_div & 0xffff;
  size_t cc_div = (cc_mul_div >> 16) & 0xffff;

  return cc_freq * cc_mul / cc_div;
}

static inline const size_t TMC_CPU_FREQ = TMC_LOONGARCH_CPU_FREQ();
#elif defined(__riscv)
#if defined(__linux__)
#include <cstdio>
#endif
#define TMC_CPU_RISCV
static inline void TMC_CPU_PAUSE() noexcept {
#if defined(__riscv_zihintpause)
  asm volatile("pause" ::: "memory");
#else
  asm volatile("nop" ::: "memory");
#endif
}
static inline size_t TMC_RISCV_READ_TIMEBASE_FREQ() noexcept {
#if defined(__linux__)
  const char* paths[] = {
    "/proc/device-tree/cpus/timebase-frequency",
    "/sys/firmware/devicetree/base/cpus/timebase-frequency",
  };
  for (const char* path : paths) {
    auto* file = std::fopen(path, "rb");
    if (file == nullptr) {
      continue;
    }
    unsigned char bytes[8] = {};
    const auto size = std::fread(bytes, 1, sizeof(bytes), file);
    std::fclose(file);
    if (size == 4 || size == 8) {
      size_t result = 0;
      for (size_t i = 0; i != size; ++i) {
        result = (result << 8) | bytes[i];
      }
      if (result != 0) {
        return result;
      }
    }
  }
#endif
  // This is only used for heuristics if the real RISC-V timebase is unavailable.
  // Use the 2GHz clock frequency of SG2042-class server chips as a default.
  return 2000000000;
}
static inline size_t TMC_CPU_TIMESTAMP() noexcept {
  size_t count;
  asm volatile("rdtime %0" : "=r"(count));
  return count;
}
static inline const size_t TMC_CPU_FREQ = TMC_RISCV_READ_TIMEBASE_FREQ();
#else
// Fallback for unknown architectures
static inline void TMC_CPU_PAUSE() noexcept {}
static inline size_t TMC_CPU_TIMESTAMP() noexcept { return 0; }
static inline const size_t TMC_CPU_FREQ = 1000000000;
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
