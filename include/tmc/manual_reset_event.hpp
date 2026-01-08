// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/concepts_awaitable.hpp"
#include "tmc/detail/waiter_list.hpp"

#include <atomic>
#include <coroutine>

namespace tmc {
class manual_reset_event;

class aw_manual_reset_event {
  tmc::detail::waiter_list_node me;
  manual_reset_event& parent;

  friend class manual_reset_event;

  inline aw_manual_reset_event(manual_reset_event& Parent) noexcept
      : parent(Parent) {}

public:
  inline bool await_ready() noexcept { return false; }

  bool await_suspend(std::coroutine_handle<> Outer) noexcept;

  inline void await_resume() noexcept {}

  // Cannot be moved or copied due to holding intrusive list pointer
  aw_manual_reset_event(aw_manual_reset_event const&) = delete;
  aw_manual_reset_event& operator=(aw_manual_reset_event const&) = delete;
  aw_manual_reset_event(aw_manual_reset_event&&) = delete;
  aw_manual_reset_event& operator=(aw_manual_reset_event&&) = delete;
};

class [[nodiscard(
  "You must co_await aw_manual_reset_event_co_set for it to have any effect."
)]] aw_manual_reset_event_co_set : tmc::detail::AwaitTagNoGroupAsIs {
  manual_reset_event& parent;

  friend class manual_reset_event;

  inline aw_manual_reset_event_co_set(manual_reset_event& Parent) noexcept
      : parent(Parent) {}

public:
  inline bool await_ready() noexcept { return false; }

  std::coroutine_handle<> await_suspend(std::coroutine_handle<> Outer) noexcept;

  inline void await_resume() noexcept {}

  // Copy/move constructors *could* be implemented, but why?
  aw_manual_reset_event_co_set(aw_manual_reset_event_co_set const&) = delete;
  aw_manual_reset_event_co_set&
  operator=(aw_manual_reset_event_co_set const&) = delete;
  aw_manual_reset_event_co_set(aw_manual_reset_event_co_set&&) = delete;
  aw_manual_reset_event_co_set&
  operator=(aw_manual_reset_event_co_set&&) = delete;
};

/// An async version of Windows ManualResetEvent.
class manual_reset_event {
  // 3 states:
  // 0 if not ready and no waiters
  // 1 if ready
  // (waiter_list_node*)(list_head) if not ready with waiters
  std::atomic<uintptr_t> head;

  static inline constexpr uintptr_t NOT_READY = 0;
  static inline constexpr uintptr_t READY = 1;

  friend class aw_manual_reset_event;
  friend class aw_manual_reset_event_co_set;

public:
  /// The Ready parameter controls the initial state.
  inline manual_reset_event(bool Ready) noexcept
      : head(Ready ? READY : NOT_READY) {}

  /// The initial state will be not-set / not-ready.
  inline manual_reset_event() noexcept : manual_reset_event(false) {}

  /// Returns true if the state is set / ready.
  inline bool is_set() noexcept {
    return manual_reset_event::READY == head.load(std::memory_order_acquire);
  }

  /// Any future awaiters will suspend.
  /// If the event state is already reset, this will do nothing.
  inline void reset() noexcept {
    auto expected = READY;
    head.compare_exchange_strong(
      expected, NOT_READY, std::memory_order_acq_rel
    );
    // Don't need to check the result of the operation - it becomes not ready
    // in any case. If there were already waiters, they will not be removed from
    // the list.
  }

  /// All current awaiters will be resumed.
  /// Any future awaiters will resume immediately.
  /// If the event state is already set, this will do nothing.
  /// Does not symmetric transfer; awaiters will be posted to their executors.
  void set() noexcept;

  /// All current awaiters will be resumed.
  /// Any future awaiters will resume immediately.
  /// If the event state is already set, this will do nothing.
  /// Up to one awaiter may be resumed by symmetric transfer if it is eligible
  /// (it resumes on the same executor and priority as the caller).
  inline aw_manual_reset_event_co_set co_set() noexcept {
    return aw_manual_reset_event_co_set(*this);
  }

  /// If the event state is set, resumes immediately.
  /// Otherwise, waits until set() is called.
  inline aw_manual_reset_event operator co_await() noexcept {
    return aw_manual_reset_event(*this);
  }

  /// On destruction, any awaiters will be resumed.
  ~manual_reset_event() noexcept;
};

namespace detail {
template <> struct awaitable_traits<tmc::manual_reset_event> {
  static constexpr configure_mode mode = WRAPPER;

  using result_type = void;
  using self_type = tmc::manual_reset_event;
  using awaiter_type = tmc::aw_manual_reset_event;

  static awaiter_type get_awaiter(self_type& Awaitable) noexcept {
    return Awaitable.operator co_await();
  }
};
} // namespace detail
} // namespace tmc

#ifdef TMC_IMPL
#include "tmc/detail/manual_reset_event.ipp"
#endif
