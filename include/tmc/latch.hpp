// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/concepts_awaitable.hpp"
#include "tmc/manual_reset_event.hpp"

#include <atomic>
#include <cstddef>

namespace tmc {
/// Similar semantics to std::latch, exposing all operations except
/// `arrive_and_wait()`.
class latch {
  tmc::manual_reset_event event;
  std::atomic<ptrdiff_t> count;

public:
  /// Sets the initial value of Count. After `count_down()` has been called
  /// Count times, all future waiters will resume. Setting this to zero or a
  /// negative number will cause awaiters to resume immediately.
  inline latch(size_t Count) noexcept
      : event{static_cast<ptrdiff_t>(Count) <= 0},
        count{static_cast<ptrdiff_t>(Count) - 1} {
    // The internal counter is 1 less than the constructor counter so that all
    // of the comparison operations can be against 0 (which is cheap).
  }

  /// Returns true after `count_down()` has been called `Count` times. After
  /// this returns true, all subsequent calls to `co_await` will resume
  /// immediately. Equivalent to `std::latch::try_wait()`.
  inline bool is_ready() noexcept {
    auto remaining = count.load(std::memory_order_acquire);
    return remaining < 0;
  }

  /// Decrements the counter and returns the value of `is_ready()` after the
  /// counter is decremented. If this becomes ready after this call to
  /// `count_down()`, all awaiters will be resumed immediately.
  /// Equivalent to `std::latch::count_down()`.
  inline bool count_down() noexcept {
    auto remaining = count.fetch_sub(1, std::memory_order_acq_rel);
    bool ready = remaining <= 0;
    if (ready) {
      event.set();
    }
    return ready;
  }

  /// Waits until the counter becomes zero.
  /// Does not decrement the counter - you must call `count_down()` separately.
  /// Equivalent to `std::latch::wait()`.
  inline tmc::aw_manual_reset_event operator co_await() noexcept {
    return event.operator co_await();
  }

  /// On destruction, any awaiters will be resumed.
  ~latch() = default;
};
namespace detail {
template <> struct awaitable_traits<tmc::latch> {
  static constexpr configure_mode mode = WRAPPER;

  using result_type = void;
  using self_type = tmc::latch;
  using awaiter_type = tmc::aw_manual_reset_event;

  static awaiter_type get_awaiter(self_type& Awaitable) noexcept {
    return Awaitable.operator co_await();
  }
};
} // namespace detail
} // namespace tmc
