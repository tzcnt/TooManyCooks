// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/asio/aw_asio.hpp"
#include "tmc/detail/compat.hpp"
#include "tmc/task.hpp"

#include "tmc/detail/tiny_mutex.hpp"

#ifdef TMC_USE_BOOST_ASIO
#include <boost/asio/steady_timer.hpp>
#include <boost/system/error_code.hpp>
#else
#include <asio/error_code.hpp>
#include <asio/steady_timer.hpp>
#endif

#include <cstddef>
#include <tuple>
#include <utility>

namespace tmc {
namespace detail {
#ifdef TMC_USE_BOOST_ASIO
namespace asio_impl = ::boost::asio;
#else
namespace asio_impl = ::asio;
#endif
} // namespace detail

/// Type that serializes Asio timer operations so expiry updates and waits can be
/// initiated safely from different coroutines. Unlike a strand, this only serializes the
/// initiation of operations; it does not serialize full handlers.
///
/// Methods behave exactly as the underlying object's methods with the same name, except:
/// - methods are thread-safe
/// - methods implicitly use the `tmc::aw_asio` completion
///
/// The safe_timer must outlive every task that uses it,
/// including any task still waiting to acquire its mutex. Destroying it while such tasks
/// exist is a use-after-free.
class safe_timer {
public:
  using timer_type = tmc::detail::asio_impl::steady_timer;
#ifdef TMC_USE_BOOST_ASIO
  using error_code = boost::system::error_code;
#else
  using error_code = asio::error_code;
#endif
  using duration = timer_type::duration;
  using time_point = timer_type::time_point;

private:
  timer_type timer_;
  tmc::tiny_mutex mut_;

public:
  /// Constructs this from an Asio steady_timer.
  explicit safe_timer(timer_type timer) : timer_(std::move(timer)) {}

  /// Allows access to the underlying (unsynchronized) Asio object.
  timer_type& timer_unsafe() noexcept { return timer_; }
  /// Allows access to the underlying (unsynchronized) Asio object.
  const timer_type& timer_unsafe() const noexcept { return timer_; }

  /// Initiates a new wait. Does not modify the expiry or cancel outstanding waits.
  tmc::task<std::tuple<error_code>> async_wait() {
    co_await mut_;

    co_return co_await timer_.async_wait(tmc::aw_asio);
  }

  /// Resets the timer's expiry and then waits for it. Changing the expiry cancels
  /// every outstanding wait on this timer before the new wait begins; those
  /// cancelled waits complete with operation_aborted.
  tmc::task<std::tuple<error_code>> async_wait_for(duration expiry) {
    co_await mut_;

    timer_.expires_after(expiry);
    co_return co_await timer_.async_wait(tmc::aw_asio);
  }

  /// Sets the timer's expiry and then waits for it. Like async_wait_for, changing
  /// the expiry cancels every outstanding wait before the new wait begins; those
  /// cancelled waits complete with operation_aborted.
  tmc::task<std::tuple<error_code>> async_wait_until(time_point expiry) {
    co_await mut_;

    timer_.expires_at(expiry);
    co_return co_await timer_.async_wait(tmc::aw_asio);
  }

  /// Cancels any outstanding waits, which will complete with
  /// `operation_aborted`. Returns the number of waits that were cancelled.
  tmc::task<std::size_t> cancel() {
    co_await mut_;

    std::size_t count = timer_.cancel();

    // Manual unlock is required since this coro didn't suspend
    co_await mut_.co_unlock_return(count);
    TMC_UNREACHABLE;
  }
};

} // namespace tmc
