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
#include <boost/asio/ip/tcp.hpp>
#include <boost/system/error_code.hpp>
#else
#include <asio/error_code.hpp>
#include <asio/ip/tcp.hpp>
#endif

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

// Type that serializes acceptor operations so they can be initiated safely from
// different coroutines. The tiny_mutex is released automatically when the
// coroutine next suspends.
class SafeAcceptor {
public:
  using acceptor_type = tmc::detail::asio_impl::ip::tcp::acceptor;
  using endpoint_type = tmc::detail::asio_impl::ip::tcp::endpoint;
  using protocol_type = tmc::detail::asio_impl::ip::tcp;
  using socket_type = tmc::detail::asio_impl::ip::tcp::socket;
#ifdef TMC_USE_BOOST_ASIO
  using error_code = boost::system::error_code;
#else
  using error_code = asio::error_code;
#endif

private:
  acceptor_type acceptor_;
  tmc::tiny_mutex mut_;

public:
  explicit SafeAcceptor(acceptor_type acceptor) : acceptor_(std::move(acceptor)) {}

  acceptor_type& acceptor_unsafe() noexcept { return acceptor_; }
  const acceptor_type& acceptor_unsafe() const noexcept { return acceptor_; }

  bool is_open() const noexcept { return acceptor_.is_open(); }

  tmc::task<error_code> open(protocol_type protocol) {
    co_await mut_;

    error_code ec;
    TMC_ASIO_SYNC_DISCARD(acceptor_.open(protocol, ec));

    // Manual unlock is required since this coro didn't suspend
    co_await mut_.co_unlock_return(ec);
    TMC_UNREACHABLE;
  }

  template <typename SettableSocketOption>
  tmc::task<error_code> set_option(SettableSocketOption option) {
    co_await mut_;

    error_code ec;
    TMC_ASIO_SYNC_DISCARD(acceptor_.set_option(option, ec));

    // Manual unlock is required since this coro didn't suspend
    co_await mut_.co_unlock_return(ec);
    TMC_UNREACHABLE;
  }

  tmc::task<error_code> bind(endpoint_type endpoint) {
    co_await mut_;

    error_code ec;
    TMC_ASIO_SYNC_DISCARD(acceptor_.bind(endpoint, ec));

    // Manual unlock is required since this coro didn't suspend
    co_await mut_.co_unlock_return(ec);
    TMC_UNREACHABLE;
  }

  tmc::task<error_code> listen(
    int backlog = tmc::detail::asio_impl::socket_base::max_listen_connections
  ) {
    co_await mut_;

    error_code ec;
    TMC_ASIO_SYNC_DISCARD(acceptor_.listen(backlog, ec));

    // Manual unlock is required since this coro didn't suspend
    co_await mut_.co_unlock_return(ec);
    TMC_UNREACHABLE;
  }

  tmc::task<std::tuple<error_code, socket_type>> async_accept() {
    co_await mut_;
    co_return co_await acceptor_.async_accept(tmc::aw_asio);
  }

  tmc::task<error_code> cancel() {
    co_await mut_;

    error_code ec;
    TMC_ASIO_SYNC_DISCARD(acceptor_.cancel(ec));

    // Manual unlock is required since this coro didn't suspend
    co_await mut_.co_unlock_return(ec);
    TMC_UNREACHABLE;
  }

  tmc::task<error_code> close() {
    co_await mut_;

    error_code ec;
    TMC_ASIO_SYNC_DISCARD(acceptor_.close(ec));

    // Manual unlock is required since this coro didn't suspend
    co_await mut_.co_unlock_return(ec);
    TMC_UNREACHABLE;
  }
};

} // namespace tmc
