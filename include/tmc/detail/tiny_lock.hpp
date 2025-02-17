// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/compat.hpp"

#include <atomic>

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
      do {
        TMC_CPU_PAUSE();
      } while (m_is_locked.test(std::memory_order_relaxed));
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
