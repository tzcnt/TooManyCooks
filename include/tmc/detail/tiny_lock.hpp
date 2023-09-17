#pragma once
#include <atomic>
#if defined(__x86_64__)
#include <immintrin.h>
#else
#include <arm_acle.h>
#endif

namespace tmc {
struct tiny_lock {
  std::atomic_flag m_is_locked;
  inline tiny_lock() : m_is_locked(false) {}
  inline bool try_lock() {
    // if (is_locked.test(std::memory_order_relaxed)) {
    //   return false;
    // }
    return !m_is_locked.test_and_set(std::memory_order_acquire);
  }
  inline void spin_lock() {
    while (m_is_locked.test_and_set(std::memory_order_acquire)) {
      while (m_is_locked.test(std::memory_order_relaxed)) {
#if defined(__x86_64__)
        _mm_pause();
#endif
#if defined(__arm__) || defined(_M_ARM) || defined(__aarch64__) ||             \
    defined(__ARM_ACLE)
        __yield();
#endif
      }
    }
  }
  inline void unlock() { m_is_locked.clear(std::memory_order_release); }
  inline bool is_locked() {
    return m_is_locked.test(std::memory_order_acquire);
  }
};
} // namespace tmc
