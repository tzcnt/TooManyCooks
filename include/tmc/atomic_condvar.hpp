// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

// An async implementation of std::atomic::wait().

#include "tmc/detail/compat.hpp"
#include "tmc/detail/concepts_awaitable.hpp"
#include "tmc/detail/thread_locals.hpp"
#include "tmc/detail/waiter_list.hpp"

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <mutex>
#include <vector>

namespace tmc {
template <typename T> class atomic_condvar;
template <typename T> class aw_atomic_condvar_co_notify;

/// The awaitable type returned by `atomic_condvar.await()`
template <typename T>
class [[nodiscard(
  "You must co_await aw_atomic_condvar for it to have any effect."
)]] aw_atomic_condvar : tmc::detail::AwaitTagNoGroupAsIs {
  T expected;
  atomic_condvar<T>& parent;
  tmc::detail::waiter_list_waiter waiter;

  friend class atomic_condvar<T>;
  friend class aw_atomic_condvar_co_notify<T>;

  inline aw_atomic_condvar(atomic_condvar<T>& Parent, T Expected) noexcept
      : expected(Expected), parent(Parent) {}

public:
  inline bool await_ready() noexcept {
    // The user has free access to this atomic variable and may execute SeqCst
    // stores. These stores aren't synchronized via the mutex if the setter is
    // on a different thread than the notifier. So if the user expects SeqCst in
    // this case then this is the only way to provide it.
    return parent.value.load(std::memory_order_seq_cst) != expected;
  }

  inline bool await_suspend(std::coroutine_handle<> Outer) noexcept {
    // Configure this awaiter
    waiter.continuation = Outer;
    waiter.continuation_executor = tmc::detail::this_thread::executor;
    waiter.continuation_priority = tmc::detail::this_thread::this_task.prio;

    std::scoped_lock<std::mutex> l{parent.waiters_lock};
    if (parent.value.load(std::memory_order_seq_cst) != expected) {
      return false;
    } else {
      parent.waiters.push_back(this);
      return true;
    }
  }

  inline void await_resume() noexcept {}

  // Cannot be moved or copied due to holding intrusive list pointer
  aw_atomic_condvar(aw_atomic_condvar const&) = delete;
  aw_atomic_condvar& operator=(aw_atomic_condvar const&) = delete;
  aw_atomic_condvar(aw_atomic_condvar&&) = delete;
  aw_atomic_condvar& operator=(aw_atomic_condvar&&) = delete;
};

/// The awaitable type returned by `atomic_condvar.co_notify_one()`,
/// `atomic_condvar.co_notify_n()`, and `atomic_condvar.co_notify_all()`.
template <typename T>
class [[nodiscard(
  "You must co_await aw_atomic_condvar_co_notify for it to have any effect."
)]] aw_atomic_condvar_co_notify : tmc::detail::AwaitTagNoGroupAsIs {
  atomic_condvar<T>& parent;
  size_t notify_count;

  friend class atomic_condvar<T>;

  inline aw_atomic_condvar_co_notify(
    atomic_condvar<T>& Parent, size_t NotifyCount
  ) noexcept
      : parent(Parent), notify_count(NotifyCount) {}

public:
  inline bool await_ready() noexcept { return notify_count == 0; }

  inline std::coroutine_handle<>
  await_suspend(std::coroutine_handle<> Outer) noexcept {
    std::vector<aw_atomic_condvar<T>*> wakeList =
      parent.get_n_waiters(notify_count);
    size_t sz = wakeList.size();
    if (sz == 0) {
      return Outer;
    }
    // Save the first / most recently added waiter for symmetric transfer.
    for (size_t i = 1; i < sz; ++i) {
      wakeList[i]->waiter.resume();
    }
    return wakeList[0]->waiter.try_symmetric_transfer(Outer);
  }

  inline void await_resume() noexcept {}

  // Copy/move constructors *could* be implemented, but why?
  aw_atomic_condvar_co_notify(aw_atomic_condvar_co_notify const&) = delete;
  aw_atomic_condvar_co_notify&
  operator=(aw_atomic_condvar_co_notify const&) = delete;
  aw_atomic_condvar_co_notify(atomic_condvar<T>&&) = delete;
  aw_atomic_condvar_co_notify&
  operator=(aw_atomic_condvar_co_notify&&) = delete;
};

/// Wraps an atomic integral type. Exposes async analogues to
/// `std::atomic<T>::wait()` and `std::atomic<T>::notify_*()`.
template <typename T> class atomic_condvar {
  std::mutex waiters_lock;
  std::vector<aw_atomic_condvar<T>*> waiters;
  std::atomic<T> value;

  friend class aw_atomic_condvar<T>;
  friend class aw_atomic_condvar_co_notify<T>;

  // Returns the most recently added waiter that matches the expected
  // condition.
  inline aw_atomic_condvar<T>* get_one_waiter() {
    aw_atomic_condvar<T>* toWake = nullptr;
    {
      std::scoped_lock<std::mutex> l{waiters_lock};
      auto v = value.load(std::memory_order_seq_cst);
      auto sz = waiters.size();
      for (size_t i = sz - 1; i != TMC_ALL_ONES; --i) {
        if (waiters[i]->expected != v) {
          toWake = waiters[i];
          waiters[i] = waiters[sz - 1];
          waiters.pop_back();
          break;
        }
      }
    }
    return toWake;
  }

  // Returns up to N waiters that match the expected condition. The most
  // recently added waiters are at the front of the returned list.
  inline std::vector<aw_atomic_condvar<T>*> get_n_waiters(size_t N) {
    std::vector<aw_atomic_condvar<T>*> wakeList;
    {
      std::scoped_lock<std::mutex> l{waiters_lock};
      auto v = value.load(std::memory_order_seq_cst);
      auto sz = waiters.size();
      for (size_t i = sz - 1; i != TMC_ALL_ONES; --i) {
        if (waiters[i]->expected != v) {
          wakeList.push_back(waiters[i]);
          waiters[i] = waiters[sz - 1];
          waiters.pop_back();
          --sz;
          --N;
          if (0 == N) {
            break;
          }
        }
      }
    }
    return wakeList;
  }

public:
  /// Sets the initial value of the contained atomic variable.
  inline atomic_condvar(T InitialValue) noexcept : value{InitialValue} {}

  /// Returns a reference to the contained atomic variable.
  inline std::atomic<T>& ref() noexcept { return value; }

  /// Wakes 1 awaiter that meet the criteria (expected != current value).
  /// The awaiter may be resumed by symmetric transfer if it is eligible
  /// (it resumes on the same executor and priority as the caller).
  inline aw_atomic_condvar_co_notify<T>
  co_notify_one(size_t NotifyCount = 1) noexcept {
    return aw_atomic_condvar_co_notify<T>(*this, NotifyCount);
  }

  /// Wakes up to NotifyCount awaiters that meet the criteria (expected !=
  /// current value).
  /// Up to one awaiter may be resumed by symmetric transfer if it is eligible
  /// (it resumes on the same executor and priority as the caller).
  inline aw_atomic_condvar_co_notify<T>
  co_notify_n(size_t NotifyCount = 1) noexcept {
    return aw_atomic_condvar_co_notify<T>(*this, NotifyCount);
  }

  /// Wakes all awaiters that meet the criteria (expected != current value).
  /// Up to one awaiter may be resumed by symmetric transfer if it is eligible
  /// (it resumes on the same executor and priority as the caller).
  inline aw_atomic_condvar_co_notify<T> co_notify_all() noexcept {
    return aw_atomic_condvar_co_notify<T>(*this, TMC_ALL_ONES);
  }

  /// Wakes 1 awaiter that meet the criteria (expected != current value).
  /// Does not symmetric transfer; the awaiter will be posted to its executor.
  inline void notify_one() {
    aw_atomic_condvar<T>* toWake = get_one_waiter();
    if (toWake != nullptr) {
      toWake->waiter.resume();
    }
  }

  /// Wakes up to NotifyCount awaiters that meet the criteria (expected !=
  /// current value).
  /// Does not symmetric transfer; awaiters will be posted to their executors.
  inline void notify_n(size_t NotifyCount = 1) {
    if (NotifyCount == 0) {
      return;
    }
    std::vector<aw_atomic_condvar<T>*> wakeList = get_n_waiters(NotifyCount);
    for (size_t i = 0; i < wakeList.size(); ++i) {
      wakeList[i]->waiter.resume();
    }
  }

  /// Wakes all awaiters that meet the criteria (expected != current value).
  /// Does not symmetric transfer; awaiters will be posted to their executors.
  inline void notify_all() {
    std::vector<aw_atomic_condvar<T>*> wakeList = get_n_waiters(TMC_ALL_ONES);
    for (size_t i = 0; i < wakeList.size(); ++i) {
      wakeList[i]->waiter.resume();
    }
  }

  /// Suspends until Expected != current value. If this condition is already
  /// true, resumes immediately.
  inline aw_atomic_condvar<T> await(T Expected) noexcept {
    return aw_atomic_condvar<T>(*this, Expected);
  }

  /// On destruction, any awaiters will be resumed.
  inline ~atomic_condvar() {
    std::scoped_lock<std::mutex> l{waiters_lock};
    // No need to unlock before resuming here - it's not valid for resumers to
    // access the destroyed mutex anyway.
    std::vector<aw_atomic_condvar<T>*> wakeList;
    for (size_t i = 0; i < waiters.size(); ++i) {
      waiters[i]->waiter.resume();
    }
  }
};
} // namespace tmc
