// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once
#include "tmc/detail/impl.hpp" // IWYU pragma: keep

#include "tmc/detail/awaitable_customizer.hpp"
#include "tmc/detail/concepts_awaitable.hpp"
#include "tmc/detail/waiter_list.hpp"

#include <atomic>
#include <cassert>
#include <coroutine>
#include <type_traits>

namespace tmc::tests {
class waiter_count_accessor;
}

namespace tmc {
class mutex;

/// The mutex will be unlocked when this goes out of scope.
class [[nodiscard("The mutex will be unlocked when this goes out of scope.")]]
mutex_scope {
  mutex* parent;

  friend class aw_mutex_lock_scope;

  inline mutex_scope(mutex* Parent) noexcept : parent(Parent) {}

public:
  // Movable but not copyable
  mutex_scope(mutex_scope const&) = delete;
  mutex_scope& operator=(mutex_scope const&) = delete;
  inline mutex_scope(mutex_scope&& Other) noexcept {
    parent = Other.parent;
    Other.parent = nullptr;
  }
  mutex_scope& operator=(mutex_scope&& Other) = delete;

  /// Unlocks the mutex on destruction. Does not symmetric transfer.
  TMC_DECL ~mutex_scope();
};

/// Same as aw_acquire but returns a nodiscard mutex_scope that unlocks the
/// mutex on destruction.
class [[nodiscard("You must co_await aw_mutex_lock_scope for it to have any effect.")]]
aw_mutex_lock_scope : tmc::detail::AwaitTagNoGroupAsIs {
  tmc::detail::waiter_list_node me;
  std::atomic<mutex*> parent;

  friend class mutex;

  inline aw_mutex_lock_scope(mutex& Parent) noexcept : parent(&Parent) {}

public:
  TMC_DECL bool await_ready() noexcept;

  TMC_DECL void await_suspend(std::coroutine_handle<> Outer) noexcept;

  inline mutex_scope await_resume() noexcept {
    return mutex_scope(parent.load(std::memory_order_relaxed));
  }

  // Cannot be moved or copied due to holding intrusive list pointer
  aw_mutex_lock_scope(aw_mutex_lock_scope const&) = delete;
  aw_mutex_lock_scope& operator=(aw_mutex_lock_scope const&) = delete;
  aw_mutex_lock_scope(aw_mutex_lock_scope&&) = delete;
  aw_mutex_lock_scope& operator=(aw_mutex_lock_scope&&) = delete;
};

class [[nodiscard("You must co_await aw_mutex_co_unlock for it to have any effect.")]]
aw_mutex_co_unlock : tmc::detail::AwaitTagNoGroupAsIs {
  mutex& parent;

  friend class mutex;

  inline aw_mutex_co_unlock(mutex& Parent) noexcept : parent(Parent) {}

public:
  inline bool await_ready() noexcept { return false; }

  TMC_DECL std::coroutine_handle<> await_suspend(std::coroutine_handle<> Outer) noexcept;

  inline void await_resume() noexcept {}

  // Copy/move constructors *could* be implemented, but why?
  aw_mutex_co_unlock(aw_mutex_co_unlock const&) = delete;
  aw_mutex_co_unlock& operator=(aw_mutex_co_unlock const&) = delete;
  aw_mutex_co_unlock(aw_mutex_co_unlock&&) = delete;
  aw_mutex_co_unlock& operator=(aw_mutex_co_unlock&&) = delete;
};

template <typename Result>
class [[nodiscard(
  "You must co_await aw_mutex_co_unlock_return for it to have any effect."
)]] aw_mutex_co_unlock_return : tmc::detail::AwaitTagNoGroupAsIs {
  mutex& parent;

  // Store lvalues by reference. Move rvalues into this.
  using ReturnValueStorage = std::conditional_t<
    std::is_lvalue_reference_v<Result>, Result, std::remove_cvref_t<Result>>;

  // Handle value return and void return.
  struct empty {};
  using ResultStorage =
    std::conditional_t<std::is_void_v<Result>, empty, ReturnValueStorage>;
  TMC_NO_UNIQUE_ADDRESS ResultStorage result;

  friend class mutex;

  // For result return
  template <typename ResultArg>
  inline aw_mutex_co_unlock_return(mutex& Parent, ResultArg&& ResultIn) noexcept
      : parent(Parent), result(static_cast<ResultArg&&>(ResultIn)) {}

  // For void return
  inline aw_mutex_co_unlock_return(mutex& Parent) noexcept : parent(Parent) {}

public:
  inline bool await_ready() noexcept { return false; }

  template <typename Promise>
  std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> Outer) noexcept;

  [[maybe_unused]] inline void await_resume() noexcept {}

  aw_mutex_co_unlock_return(aw_mutex_co_unlock_return const&) = delete;
  aw_mutex_co_unlock_return& operator=(aw_mutex_co_unlock_return const&) = delete;
  aw_mutex_co_unlock_return(aw_mutex_co_unlock_return&&) = delete;
  aw_mutex_co_unlock_return& operator=(aw_mutex_co_unlock_return&&) = delete;
};

/// An async version of std::mutex.
class mutex : protected tmc::detail::waiter_data_base {
  friend class aw_acquire;
  friend class aw_mutex_lock_scope;
  friend class aw_mutex_co_unlock;
  template <typename Result> friend class aw_mutex_co_unlock_return;
  friend class ::tmc::tests::waiter_count_accessor;

  static inline constexpr tmc::detail::half_word LOCKED = 0;
  static inline constexpr tmc::detail::half_word UNLOCKED = 1;

  // Returns the number of awaiters currently registered (suspended and
  // waiting to acquire) on this mutex. For testing purposes. Thread-safe.
  inline size_t waiter_count() noexcept {
    return (value.load(std::memory_order_acquire) >> tmc::detail::WAITERS_OFFSET) &
           tmc::detail::HALF_MASK;
  }

public:
  /// Mutex begins in the unlocked state.
  inline mutex() noexcept { value = UNLOCKED; }

  /// Returns true if some task is holding the mutex.
  /// This value is not guaranteed to be consistent with any other operation.
  /// Even if this returns false, awaiting afterward may suspend.
  inline bool is_locked() noexcept {
    return 0 == (tmc::detail::HALF_MASK & value.load(std::memory_order_relaxed));
  }

  /// Unlocks the mutex. If there are any awaiters, an awaiter will be resumed
  /// and the lock will be re-locked and transferred to that awaiter.
  /// Does not symmetric transfer; the awaiter will be posted to its executor.
  TMC_DECL void unlock() noexcept;

  /// Unlocks the mutex. If there are any awaiters, an awaiter will be resumed
  /// and the lock will be re-locked and transferred to that awaiter.
  /// The awaiter may be resumed by symmetric transfer if it is eligible
  /// (it resumes on the same executor and priority as the caller).
  inline aw_mutex_co_unlock co_unlock() noexcept { return aw_mutex_co_unlock(*this); }

  /// Unlocks the mutex. If there are any awaiters, an awaiter will be resumed
  /// and the lock will be re-locked and transferred to that awaiter. Also
  /// completes this coroutine immediately, returns the result back to its
  /// parent coroutine, and resumes the parent coroutine. Both the resuming
  /// awaiter and the parent coroutine will be checked for symmetric transfer
  /// eligibility; otherwise they will be posted back to their respective
  /// executors.
  ///
  /// This effectively contains a `co_return` statement, ending the current
  /// coroutine; nothing will be executed after it in the current scope.
  ///
  /// The purpose of this is to skip a round-trip through the executor when
  /// you want to unlock this mutex immediately before returning.
  ///
  /// ```
  /// // You can replace this:
  /// co_await mut.co_unlock();
  /// co_return result;
  ///
  /// // With this:
  /// co_await mut.co_unlock_return(result);
  /// std::unreachable();
  /// ```
  template <typename Result>
  inline aw_mutex_co_unlock_return<Result> co_unlock_return(Result&& result) noexcept {
    return aw_mutex_co_unlock_return<Result>(*this, static_cast<Result&&>(result));
  }

  /// Unlocks the mutex. If there are any awaiters, an awaiter will be resumed
  /// and the lock will be re-locked and transferred to that awaiter. Also
  /// completes this coroutine immediately and resumes the parent coroutine.
  /// Both the resuming awaiter and the parent coroutine will be checked for
  /// symmetric transfer eligibility; otherwise they will be posted back to
  /// their respective executors.
  ///
  /// This effectively contains a `co_return` statement, ending the current
  /// coroutine; nothing will be executed after it in the current scope.
  ///
  /// The purpose of this is to skip a round-trip through the executor when
  /// you want to unlock this mutex immediately before returning.
  ///
  /// ```
  /// // You can replace this:
  /// co_await mut.co_unlock();
  /// co_return;
  ///
  /// // With this:
  /// co_await mut.co_unlock_return();
  /// std::unreachable();
  /// ```
  inline aw_mutex_co_unlock_return<void> co_unlock_return() noexcept {
    return aw_mutex_co_unlock_return<void>(*this);
  }

  /// Tries to acquire the mutex without suspending. Returns true on success,
  /// or false if the mutex is already locked. Not re-entrant.
  inline bool try_lock() noexcept { return tmc::detail::try_acquire(value); }

  /// Tries to acquire the mutex. If it is locked by another task, will
  /// suspend until it can be locked by this task, then transfer the
  /// ownership to this task. Not re-entrant.
  inline aw_acquire operator co_await() noexcept { return aw_acquire(*this); }

  /// Tries to acquire the mutex. If it is locked by another task, will
  /// suspend until it can be locked by this task, then transfer the
  /// ownership to this task. Not re-entrant.
  /// Returns an object that will unlock the mutex (and resume an awaiter) when
  /// it goes out of scope.
  inline aw_mutex_lock_scope lock_scope() noexcept { return aw_mutex_lock_scope(*this); }

  /// On destruction, any awaiters will be resumed.
  TMC_DECL ~mutex();
};

template <typename Result>
template <typename Promise>
std::coroutine_handle<> aw_mutex_co_unlock_return<Result>::await_suspend(
  std::coroutine_handle<Promise> Outer
) noexcept {
  assert(parent.is_locked());
  if constexpr (std::is_void_v<Result>) {
    Outer.promise().return_void();
  } else {
    Outer.promise().return_value(static_cast<Result&&>(result));
  }

  // Unlock the mutex normally and capture the continuation
  size_t old = parent.value.fetch_or(mutex::UNLOCKED, std::memory_order_acq_rel);
  size_t v = mutex::UNLOCKED | old;
  auto toWake = parent.waiters.maybe_wake(parent.value, v, old, true);

  // Capture these values locally before destroying the coroutine frame
  auto& customizer = Outer.promise().customizer;
  void* continuationExecutor = customizer.continuation_executor;
  void* continuationPtr = customizer.continuation;
  void* doneCount = customizer.done_count;
  size_t flags = customizer.flags;

  // Destroy the coroutine *before* calling get_continuation, which could allow
  // the continuation to be stolen by another parent, which could then complete
  // and destroy the frame of this coroutine if it is HALO'd into that parent.
  // By destroying ourselves first, we avoid use-after-free. This is the same
  // protocol that tmc::task's final_suspend follows.
  Outer.destroy();

  std::coroutine_handle<> continuation =
    tmc::detail::awaitable_customizer_base::get_continuation(
      continuationExecutor, continuationPtr, doneCount, flags
    );

  size_t continuationPriority = flags & tmc::detail::task_flags::PRIORITY_MASK;
  return tmc::detail::try_symmetric_transfer2_waiter(
    toWake, continuation, static_cast<tmc::ex_any*>(continuationExecutor),
    continuationPriority
  );
}

namespace detail {
template <> struct awaitable_traits<tmc::mutex> {
  static constexpr configure_mode mode = WRAPPER;

  using result_type = void;
  using self_type = tmc::mutex;
  using awaiter_type = tmc::aw_acquire;

  static awaiter_type get_awaiter(self_type& Awaitable) noexcept {
    return Awaitable.operator co_await();
  }
};
} // namespace detail
} // namespace tmc

#if !defined(TMC_STANDALONE_COMPILATION) || defined(TMC_IMPL)
#include "tmc/detail/mutex.ipp"
#endif
