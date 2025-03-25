#pragma once

#include <atomic>

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

#if defined(__x86_64__) || defined(_M_AMD64) || defined(i386) ||               \
  defined(__i386__) || defined(__i386) || defined(_M_IX86)
#include <immintrin.h>
#define TMC_CPU_X86
#define TMC_CPU_PAUSE _mm_pause
#define TMC_CPU_TIMESTAMP __rdtsc
// Assume a 3GHz CPU if we can't get the value (on x86).
// Yes, this is hacky. Getting the real RDTSC freq requires
// waiting for another time source (system timer) and then dividing by
// that duration. This takes real time and would have to be done on
// startup. Using a 3GHz default means that slower processors will appear to be
// running faster, and vice versa. For the current usage of this (the clustering
// threshold in tmc::channel) this seems like reasonable behavior anyway.
static inline const size_t TMC_CPU_FREQ = 3000000000;
#elif defined(__arm__) || defined(_M_ARM) || defined(_M_ARM64) ||              \
  defined(__aarch64__) || defined(__ARM_ACLE)
#include <arm_acle.h>
#define TMC_CPU_ARM
#define TMC_CPU_PAUSE __yield
// Read the ARM "Virtual Counter" register.
// This ticks at a frequency independent of the processor frequency.
// https://developer.arm.com/documentation/ddi0406/cb/System-Level-Architecture/The-Generic-Timer/About-the-Generic-Timer/The-virtual-counter?lang=en
static inline size_t TMC_CPU_TIMESTAMP() {
  size_t count;
  asm volatile("mrs %0, cntvct_el0; " : "=r"(count)::"memory");
  return count;
}
// Read the ARM "Virtual Counter" frequency.
static inline size_t TMC_ARM_CPU_FREQ() {
  size_t freq;
  asm volatile("mrs %0, cntfrq_el0; isb; " : "=r"(freq)::"memory");
  return freq;
}
static inline const size_t TMC_CPU_FREQ = TMC_ARM_CPU_FREQ();
#endif

namespace tmc::detail {
TMC_FORCE_INLINE inline void memory_barrier() {
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

static inline constexpr size_t TMC_PLATFORM_BITS =
  sizeof(size_t) * 8; // 32 or 64
