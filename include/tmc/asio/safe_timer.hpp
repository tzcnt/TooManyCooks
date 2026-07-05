// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/asio/aw_asio.hpp"
#include "tmc/task.hpp"

#include "tmc/detail/tiny_mutex.hpp"

#include <boost/asio/steady_timer.hpp>

#include <chrono>
#include <tuple>
#include <utility>

namespace tmc {

// Type that serializes timer operations so expiry updates and waits can be
// initiated safely from different coroutines. The tiny_mutex is released
// automatically when the coroutine next suspends.
class SafeTimer {
public:
  using timer_type = boost::asio::steady_timer;
  using error_code = boost::system::error_code;
  using duration = timer_type::duration;
  using time_point = timer_type::time_point;

private:
  timer_type timer_;
  tmc::tiny_mutex mut_;

public:
  explicit SafeTimer(timer_type timer) : timer_(std::move(timer)) {}

  timer_type& timer_unsafe() noexcept { return timer_; }
  const timer_type& timer_unsafe() const noexcept { return timer_; }

  tmc::task<std::tuple<error_code>> async_wait() {
    co_await mut_;

    co_return co_await timer_.async_wait(tmc::aw_asio);
  }

  template <typename Rep, typename Period>
  tmc::task<std::tuple<error_code>>
  async_wait_for(std::chrono::duration<Rep, Period> expiry) {
    co_await mut_;

    timer_.expires_after(expiry);
    co_return co_await timer_.async_wait(tmc::aw_asio);
  }

  tmc::task<std::tuple<error_code>> async_wait_until(time_point expiry) {
    co_await mut_;

    timer_.expires_at(expiry);
    co_return co_await timer_.async_wait(tmc::aw_asio);
  }

  tmc::task<error_code> cancel() {
    co_await mut_;

    error_code ec;
    timer_.cancel(ec);

    // Manual unlock is required since this coro didn't suspend
    co_await mut_.co_unlock_return(ec);
    std::unreachable();
  }
};

} // namespace tmc
