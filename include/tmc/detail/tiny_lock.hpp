// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/compat.hpp"

#include <atomic>
#include <cassert>

namespace tmc {
/// A tiny lock with no syscalls. Used by tmc::ex_braid.
class tiny_lock {
  std::atomic_flag m_is_locked;

public:
  inline tiny_lock() noexcept { m_is_locked.clear(); }

  inline bool try_lock() noexcept {
    return !m_is_locked.test_and_set(std::memory_order_acquire);
  }

  inline void spin_lock() noexcept {
    while (m_is_locked.test_and_set(std::memory_order_acquire)) {
      do {
        TMC_CPU_PAUSE();
      } while (m_is_locked.test(std::memory_order_relaxed));
    }
  }

  inline void unlock() noexcept {
    m_is_locked.clear(std::memory_order_release);
  }

  inline bool is_locked() const noexcept {
    return m_is_locked.test(std::memory_order_acquire);
  }
};

class [[nodiscard]] tiny_lock_guard {
  tiny_lock& lock;

public:
  [[nodiscard]] inline tiny_lock_guard(tiny_lock& Lock) noexcept : lock{Lock} {
    lock.spin_lock();
  }
  inline ~tiny_lock_guard() noexcept { lock.unlock(); }

  tiny_lock_guard(const tiny_lock_guard&) = delete;
  tiny_lock_guard& operator=(const tiny_lock_guard&) = delete;
  tiny_lock_guard(tiny_lock_guard&&) = delete;
  tiny_lock_guard& operator=(tiny_lock_guard&&) = delete;
};

class [[nodiscard]] concurrent_access_scope {
  tiny_lock& lock;

public:
  // The lock is acquired in the ASSERT_NO_CONCURRENT_ACCESS macro
  [[nodiscard]] inline concurrent_access_scope(tiny_lock& Lock) noexcept
      : lock{Lock} {}
  inline ~concurrent_access_scope() noexcept { lock.unlock(); }

  concurrent_access_scope(const concurrent_access_scope&) = delete;
  concurrent_access_scope& operator=(const concurrent_access_scope&) = delete;
  concurrent_access_scope(concurrent_access_scope&&) = delete;
  concurrent_access_scope& operator=(concurrent_access_scope&&) = delete;
};

#ifndef NDEBUG
#define NO_CONCURRENT_ACCESS_LOCK tmc::tiny_lock no_concurrent_access_lock_;
#else
#define NO_CONCURRENT_ACCESS_LOCK
#endif

// Used in debug builds to assert that no two threads
// attempt to use the protected resource at the same time.
// The assert is inline here, instead of in the concurrent_access_scope
// constructor, so that the displayed source line of the failed assert is that
// of the caller.
#ifndef NDEBUG
#define ASSERT_NO_CONCURRENT_ACCESS()                                          \
  assert(                                                                      \
    no_concurrent_access_lock_.try_lock() &&                                   \
    "Concurrent access to this object is not allowed."                         \
  );                                                                           \
  tmc::concurrent_access_scope concurrent_access_check_(                       \
    no_concurrent_access_lock_                                                 \
  )
#else
#define ASSERT_NO_CONCURRENT_ACCESS()
#endif
} // namespace tmc
