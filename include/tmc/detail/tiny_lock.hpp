#pragma once
// MIT License

// Copyright (c) 2023 Logan McDougall

// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include <atomic>
#if defined(__x86_64__) || defined(_M_AMD64)
#include <immintrin.h>
#else
#include <arm_acle.h>
#endif

namespace tmc {
/// A tiny lock with no syscalls. Used by tmc::ex_braid.
class tiny_lock {
  std::atomic_flag m_is_locked;

public:
  inline tiny_lock() { m_is_locked.clear(); }

  inline bool try_lock() {
    // if (is_locked.test(std::memory_order_relaxed)) {
    //   return false;
    // }
    return !m_is_locked.test_and_set(std::memory_order_acquire);
  }

  inline void spin_lock() {
    while (m_is_locked.test_and_set(std::memory_order_acquire)) {
      while (m_is_locked.test(std::memory_order_relaxed)) {
#if defined(__x86_64__) || defined(_M_AMD64)
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

class [[nodiscard]] tiny_lock_guard {
  tiny_lock& lock;

public:
  [[nodiscard]] inline tiny_lock_guard(tiny_lock& Lock) : lock{Lock} {
    lock.spin_lock();
  }
  inline ~tiny_lock_guard() { lock.unlock(); }
};
} // namespace tmc
