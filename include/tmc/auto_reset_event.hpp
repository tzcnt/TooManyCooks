// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/concepts_awaitable.hpp"
#include "tmc/detail/impl.hpp" // IWYU pragma: keep
#include "tmc/detail/waiter_list.hpp"

#include <atomic>
#include <coroutine>

namespace tmc {
class auto_reset_event;

class [[nodiscard(
  "You must co_await aw_auto_reset_event_co_set for it to have any effect."
)]] aw_auto_reset_event_co_set : tmc::detail::AwaitTagNoGroupAsIs {
  auto_reset_event& parent;

  friend class auto_reset_event;

  inline aw_auto_reset_event_co_set(auto_reset_event& Parent) noexcept
      : parent(Parent) {}

public:
  inline bool await_ready() noexcept { return false; }

  TMC_DECL std::coroutine_handle<>
  await_suspend(std::coroutine_handle<> Outer) noexcept;

  inline void await_resume() noexcept {}

  // Copy/move constructors *could* be implemented, but why?
  aw_auto_reset_event_co_set(aw_auto_reset_event_co_set const&) = delete;
  aw_auto_reset_event_co_set&
  operator=(aw_auto_reset_event_co_set const&) = delete;
  aw_auto_reset_event_co_set(aw_auto_reset_event_co_set&&) = delete;
  aw_auto_reset_event_co_set& operator=(aw_auto_reset_event_co_set&&) = delete;
};

/// An async version of Windows AutoResetEvent.
class auto_reset_event : protected tmc::detail::waiter_data_base {
  friend class aw_acquire;
  friend class aw_auto_reset_event_co_set;

public:
  /// The Ready parameter controls the initial state.
  inline auto_reset_event(bool Ready) noexcept { value = Ready ? 1 : 0; }

  /// The initial state will be not-set / not-ready.
  inline auto_reset_event() noexcept : auto_reset_event(false) {}

  /// Returns true if the state is set / ready.
  /// This value is not guaranteed to be consistent with any other operation.
  /// Even if this returns true, awaiting afterward may suspend.
  inline bool is_set() noexcept {
    return 0 !=
           (tmc::detail::HALF_MASK & value.load(std::memory_order_relaxed));
  }

  /// Any future awaiters will suspend.
  /// If the event state is already reset, this will do nothing.
  TMC_DECL void reset() noexcept;

  /// Makes the event state set. If there are any awaiters, the state will be
  /// immediately reset and one awaiter will be resumed.
  /// If the event state is already set, this will do nothing.
  /// Does not symmetric transfer; awaiters will be posted to their executors.
  TMC_DECL void set() noexcept;

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
  inline aw_acquire operator co_await() noexcept { return aw_acquire(*this); }

  /// On destruction, any awaiters will be resumed.
  TMC_DECL ~auto_reset_event();
};

namespace detail {
template <> struct awaitable_traits<tmc::auto_reset_event> {
  static constexpr configure_mode mode = WRAPPER;

  using result_type = void;
  using self_type = tmc::auto_reset_event;
  using awaiter_type = tmc::aw_acquire;

  static awaiter_type get_awaiter(self_type& Awaitable) noexcept {
    return Awaitable.operator co_await();
  }
};
} // namespace detail
} // namespace tmc

#if !defined(TMC_STANDALONE_COMPILATION) || defined(TMC_IMPL)
#include "tmc/detail/auto_reset_event.ipp"
#endif
