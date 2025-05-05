// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/compat.hpp"
#include "tmc/detail/concepts_awaitable.hpp"
#include "tmc/detail/waiter_list.hpp"

#include <atomic>
#include <coroutine>
#include <cstddef>

namespace tmc {
class mutex;

class aw_mutex {
  tmc::detail::waiter_list_node me;
  mutex* parent;

  friend class mutex;

  inline aw_mutex(mutex* Parent) noexcept : parent(Parent) {}

public:
  bool await_ready() noexcept;

  bool await_suspend(std::coroutine_handle<> Outer) noexcept;

  inline void await_resume() noexcept {}

  // Cannot be moved or copied due to holding intrusive list pointer
  aw_mutex(aw_mutex const&) = delete;
  aw_mutex& operator=(aw_mutex const&) = delete;
  aw_mutex(aw_mutex&&) = delete;
  aw_mutex& operator=(aw_mutex&&) = delete;
};

class [[nodiscard(
  "You must co_await aw_mutex_co_unlock for it to have any effect."
)]] aw_mutex_co_unlock : tmc::detail::AwaitTagNoGroupAsIs {
  mutex* parent;

  friend class mutex;

  inline aw_mutex_co_unlock(mutex* Parent) noexcept : parent(Parent) {}

public:
  inline bool await_ready() noexcept { return false; };

  std::coroutine_handle<> await_suspend(std::coroutine_handle<> Outer) noexcept;

  inline void await_resume() noexcept {}

  // Cannot be moved or copied due to holding intrusive list pointer
  aw_mutex_co_unlock(aw_mutex_co_unlock const&) = delete;
  aw_mutex_co_unlock& operator=(aw_mutex_co_unlock const&) = delete;
  aw_mutex_co_unlock(aw_mutex&&) = delete;
  aw_mutex_co_unlock& operator=(aw_mutex_co_unlock&&) = delete;
};

class mutex {
  tmc::detail::waiter_list waiters;
  // Low half bits are the mutex value.
  // High half bits are the number of waiters.
  std::atomic<size_t> value;

  friend class aw_mutex;
  friend class aw_mutex_co_unlock;

  static inline constexpr size_t LOCKED = 0;
  static inline constexpr size_t UNLOCKED = 1;

  static inline constexpr size_t WAITERS_OFFSET = TMC_PLATFORM_BITS / 2;
  static inline constexpr size_t HALF_MASK =
    (TMC_ONE_BIT << (TMC_PLATFORM_BITS / 2)) - 1;

  static inline void unpack_value(
    size_t Value, size_t& State_out, size_t& WaiterCount_out
  ) noexcept {
    State_out = Value & HALF_MASK;
    WaiterCount_out = (Value >> WAITERS_OFFSET) & HALF_MASK;
  }

  static inline size_t pack_value(size_t State, size_t WaiterCount) noexcept {
    return (WaiterCount << WAITERS_OFFSET) | State;
  }

  // Called after increasing State or WaiterCount.
  // If State > 0 && WaiterCount > 0, this will try to wake some number of
  // awaiters.
  void maybe_wake(size_t v) noexcept;

public:
  /// Mutex begins in the unlocked state.
  inline mutex() noexcept : value{UNLOCKED} {}

  /// Returns true if some task is holding the mutex.
  inline bool is_locked() noexcept {
    return 0 == (HALF_MASK & value.load(std::memory_order_relaxed));
  }

  /// Returns true if the mutex was unlocked and the lock was successfully
  /// acquired. Returns false if the mutex was locked. Not re-entrant.
  bool try_lock() noexcept;

  /// Unlocks the mutex. If there are any awaiters, an awaiter will be resumed
  /// and the lock will be re-locked and transferred to that awaiter.
  /// Does not symmetric transfer; awaiter will be posted to its executor.
  void unlock() noexcept;

  /// Unlocks the mutex. If there are any awaiters, an awaiter will be resumed
  /// and the lock will be re-locked and transferred to that awaiter. The
  /// awaiter will be resumed by symmetric transfer if it should run on the same
  /// executor and priority as the current task. If the awaiter is resumed by
  /// symmetric transfer, the caller will be posted to its executor.
  inline aw_mutex_co_unlock co_unlock() noexcept {
    return aw_mutex_co_unlock(this);
  }

  /// Tries to acquire the mutex, and if no resources are ready, will
  /// suspend until a resource becomes ready. Not re-entrant.
  inline aw_mutex operator co_await() noexcept { return aw_mutex(this); }

  /// On destruction, any awaiters will be resumed.
  ~mutex();
};

namespace detail {
template <> struct awaitable_traits<tmc::mutex> {
  static constexpr configure_mode mode = WRAPPER;

  using result_type = void;
  using self_type = tmc::mutex;
  using awaiter_type = tmc::aw_mutex;

  static awaiter_type get_awaiter(self_type& Awaitable) noexcept {
    return Awaitable.operator co_await();
  }
};
} // namespace detail
} // namespace tmc

#ifdef TMC_IMPL
#include "tmc/detail/mutex.ipp"
#endif
