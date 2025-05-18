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
class auto_reset_event;

class aw_auto_reset_event {
  tmc::detail::waiter_list_node me;
  auto_reset_event& parent;

  friend class auto_reset_event;

  inline aw_auto_reset_event(auto_reset_event& Parent) noexcept
      : parent(Parent) {}

public:
  bool await_ready() noexcept;

  bool await_suspend(std::coroutine_handle<> Outer) noexcept;

  inline void await_resume() noexcept {}

  // Cannot be moved or copied due to holding intrusive list pointer
  aw_auto_reset_event(aw_auto_reset_event const&) = delete;
  aw_auto_reset_event& operator=(aw_auto_reset_event const&) = delete;
  aw_auto_reset_event(aw_auto_reset_event&&) = delete;
  aw_auto_reset_event& operator=(aw_auto_reset_event&&) = delete;
};

class [[nodiscard(
  "You must co_await aw_auto_reset_event_co_unlock for it to have any effect."
)]] aw_auto_reset_event_co_set : tmc::detail::AwaitTagNoGroupAsIs {
  auto_reset_event& parent;

  friend class auto_reset_event;

  inline aw_auto_reset_event_co_set(auto_reset_event& Parent) noexcept
      : parent(Parent) {}

public:
  inline bool await_ready() noexcept { return false; }

  std::coroutine_handle<> await_suspend(std::coroutine_handle<> Outer) noexcept;

  inline void await_resume() noexcept {}

  // Copy/move constructors *could* be implemented, but why?
  aw_auto_reset_event_co_set(aw_auto_reset_event_co_set const&) = delete;
  aw_auto_reset_event_co_set&
  operator=(aw_auto_reset_event_co_set const&) = delete;
  aw_auto_reset_event_co_set(aw_auto_reset_event&&) = delete;
  aw_auto_reset_event_co_set& operator=(aw_auto_reset_event_co_set&&) = delete;
};

/// An async version of Windows AutoResetEvent.
class auto_reset_event {
  tmc::detail::waiter_list waiters;
  // Low half bits are the auto_reset_event value.
  // High half bits are the number of waiters.
  std::atomic<size_t> value;

  friend class aw_auto_reset_event;
  friend class aw_auto_reset_event_co_set;

  // The implementation of this class is similar to tmc::semaphore, but
  // saturates the state / count to a maximum of 1.

  // Called after increasing Count or WaiterCount.
  // If Count > 0 && WaiterCount > 0, this will try to wake some number of
  // awaiters.
  void maybe_wake(size_t v) noexcept;

public:
  /// The Ready parameter controls the initial state.
  inline auto_reset_event(bool Ready) noexcept : value(Ready ? 1 : 0) {}

  /// The initial state will be not-set / not-ready.
  inline auto_reset_event() noexcept : auto_reset_event(false) {}

  /// Returns true if the state is set / ready.
  inline bool is_set() noexcept {
    return 0 !=
           (tmc::detail::HALF_MASK & value.load(std::memory_order_relaxed));
  }

  /// Any future awaiters will suspend.
  /// If the event state is already reset, this will do nothing.
  void reset() noexcept;

  /// Makes the event state set. If there are any awaiters, the state will be
  /// immediately reset and one awaiter will be resumed.
  /// If the event state is already set, this will do nothing.
  /// Does not symmetric transfer; awaiters will be posted to their executors.
  void set() noexcept;

  /// Makes the event state set. If there are any awaiters, the state will be
  /// immediately reset and one awaiter will be resumed.
  /// If the event state is already set, this will do nothing.
  /// The awaiter may be resumed by symmetric transfer if it is eligible
  /// (it resumes on the same executor and priority as the caller).
  inline aw_auto_reset_event_co_set co_set() noexcept {
    return aw_auto_reset_event_co_set(*this);
  }

  /// If the event state is set, resumes immediately.
  /// Otherwise, waits until set() is called.
  inline aw_auto_reset_event operator co_await() noexcept {
    return aw_auto_reset_event(*this);
  }

  /// On destruction, any awaiters will be resumed.
  ~auto_reset_event();
};

namespace detail {
template <> struct awaitable_traits<tmc::auto_reset_event> {
  static constexpr configure_mode mode = WRAPPER;

  using result_type = void;
  using self_type = tmc::auto_reset_event;
  using awaiter_type = tmc::aw_auto_reset_event;

  static awaiter_type get_awaiter(self_type& Awaitable) noexcept {
    return Awaitable.operator co_await();
  }
};
} // namespace detail
} // namespace tmc

#ifdef TMC_IMPL
#include "tmc/detail/auto_reset_event.ipp"
#endif
