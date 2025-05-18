// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/concepts_awaitable.hpp"
#include "tmc/detail/waiter_list.hpp"

#include <atomic>
#include <coroutine>
#include <cstddef>

namespace tmc {
class mutex;

class aw_mutex {
  tmc::detail::waiter_list_node me;
  mutex& parent;

  friend class mutex;

  inline aw_mutex(mutex& Parent) noexcept : parent(Parent) {}

public:
  bool await_ready() noexcept;

  void await_suspend(std::coroutine_handle<> Outer) noexcept;

  inline void await_resume() noexcept {}

  // Cannot be moved or copied due to holding intrusive list pointer
  aw_mutex(aw_mutex const&) = delete;
  aw_mutex& operator=(aw_mutex const&) = delete;
  aw_mutex(aw_mutex&&) = delete;
  aw_mutex& operator=(aw_mutex&&) = delete;
};

/// The mutex will be unlocked when this goes out of scope.
class [[nodiscard("The mutex will be unlocked when this goes out of scope."
)]] mutex_scope {
  mutex* parent;

  friend class aw_mutex_lock_scope;

  inline mutex_scope(mutex* Parent) noexcept : parent(Parent) {}

public:
  // Movable but not copyable
  mutex_scope(mutex_scope const&) = delete;
  mutex_scope& operator=(mutex_scope const&) = delete;
  inline mutex_scope(mutex_scope&& Other) {
    parent = Other.parent;
    Other.parent = nullptr;
  }
  mutex_scope& operator=(mutex_scope&& Other) = delete;

  /// Unlocks the mutex on destruction. Does not symmetric transfer.
  ~mutex_scope();
};

/// Same as aw_mutex but returns a nodiscard mutex_scope that unlocks the mutex
/// on destruction.
class [[nodiscard(
  "You must co_await aw_mutex_lock_scope for it to have any effect."
)]] aw_mutex_lock_scope : tmc::detail::AwaitTagNoGroupAsIs {
  tmc::detail::waiter_list_node me;
  mutex& parent;

  friend class mutex;

  inline aw_mutex_lock_scope(mutex& Parent) noexcept : parent(Parent) {}

public:
  bool await_ready() noexcept;

  void await_suspend(std::coroutine_handle<> Outer) noexcept;

  inline mutex_scope await_resume() noexcept { return mutex_scope(&parent); }

  // Cannot be moved or copied due to holding intrusive list pointer
  aw_mutex_lock_scope(aw_mutex_lock_scope const&) = delete;
  aw_mutex_lock_scope& operator=(aw_mutex_lock_scope const&) = delete;
  aw_mutex_lock_scope(aw_mutex_lock_scope&&) = delete;
  aw_mutex_lock_scope& operator=(aw_mutex_lock_scope&&) = delete;
};

class [[nodiscard(
  "You must co_await aw_mutex_co_unlock for it to have any effect."
)]] aw_mutex_co_unlock : tmc::detail::AwaitTagNoGroupAsIs {
  mutex& parent;

  friend class mutex;

  inline aw_mutex_co_unlock(mutex& Parent) noexcept : parent(Parent) {}

public:
  inline bool await_ready() noexcept { return false; }

  std::coroutine_handle<> await_suspend(std::coroutine_handle<> Outer) noexcept;

  inline void await_resume() noexcept {}

  // Copy/move constructors *could* be implemented, but why?
  aw_mutex_co_unlock(aw_mutex_co_unlock const&) = delete;
  aw_mutex_co_unlock& operator=(aw_mutex_co_unlock const&) = delete;
  aw_mutex_co_unlock(aw_mutex&&) = delete;
  aw_mutex_co_unlock& operator=(aw_mutex_co_unlock&&) = delete;
};

/// An async version of std::mutex.
class mutex {
  tmc::detail::waiter_list waiters;
  // Low half bits are the mutex value.
  // High half bits are the number of waiters.
  std::atomic<size_t> value;

  friend class aw_mutex;
  friend class aw_mutex_lock_scope;
  friend class aw_mutex_co_unlock;

  static inline constexpr tmc::detail::half_word LOCKED = 0;
  static inline constexpr tmc::detail::half_word UNLOCKED = 1;

public:
  /// Mutex begins in the unlocked state.
  inline mutex() noexcept : value{UNLOCKED} {}

  /// Returns true if some task is holding the mutex.
  inline bool is_locked() noexcept {
    return 0 ==
           (tmc::detail::HALF_MASK & value.load(std::memory_order_relaxed));
  }

  /// Unlocks the mutex. If there are any awaiters, an awaiter will be resumed
  /// and the lock will be re-locked and transferred to that awaiter.
  /// Does not symmetric transfer; the awaiter will be posted to its executor.
  void unlock() noexcept;

  /// Unlocks the mutex. If there are any awaiters, an awaiter will be resumed
  /// and the lock will be re-locked and transferred to that awaiter.
  /// The awaiter may be resumed by symmetric transfer if it is eligible
  /// (it resumes on the same executor and priority as the caller).
  inline aw_mutex_co_unlock co_unlock() noexcept {
    return aw_mutex_co_unlock(*this);
  }

  /// Tries to acquire the mutex. If it is locked by another task, will
  /// suspend until it can be locked by this task, then transfer the
  /// ownership to this task. Not re-entrant.
  inline aw_mutex operator co_await() noexcept { return aw_mutex(*this); }

  /// Tries to acquire the mutex. If it is locked by another task, will
  /// suspend until it can be locked by this task, then transfer the
  /// ownership to this task. Not re-entrant.
  /// Returns an object that will unlock the mutex (and resume an awaiter) when
  /// it goes out of scope.
  inline aw_mutex_lock_scope lock_scope() noexcept {
    return aw_mutex_lock_scope(*this);
  }

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
