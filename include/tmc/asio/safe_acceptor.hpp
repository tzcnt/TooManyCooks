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

/// Type that serializes Asio acceptor operations so they can be initiated safely from
/// different coroutines. Unlike a strand, this only serializes the initiation of
/// operations; it does not serialize full handlers.
///
/// Methods behave exactly as the underlying object's methods with the same name, except:
/// - methods are thread-safe
/// - methods implicitly use the `tmc::aw_asio` completion
///
/// The safe_acceptor must outlive every task that uses it, including any task still
/// waiting to acquire its mutex. Destroying it while such tasks exist is a
/// use-after-free.
///
/// Templated on the underlying Asio acceptor type so it can serialize acceptors
/// for any AcceptableProtocol (e.g. `ip::tcp`, `local::stream_protocol`). All
/// associated types are derived from `Acceptor`.
template <typename Acceptor = tmc::detail::asio_impl::ip::tcp::acceptor>
class basic_safe_acceptor {
public:
  using acceptor_type = Acceptor;
  using protocol_type = typename Acceptor::protocol_type;
  using endpoint_type = typename Acceptor::endpoint_type;
  using executor_type = typename Acceptor::executor_type;
  using native_handle_type = typename Acceptor::native_handle_type;
  using wait_type = typename Acceptor::wait_type;
  // The socket type produced by `async_accept()`: the protocol's socket rebound
  // to the acceptor's executor (matches Asio's own accept-return type).
  using socket_type =
    typename protocol_type::socket::template rebind_executor<executor_type>::other;
#ifdef TMC_USE_BOOST_ASIO
  using error_code = boost::system::error_code;
#else
  using error_code = asio::error_code;
#endif

private:
  acceptor_type acceptor_;
  tmc::tiny_mutex mut_;

public:
  /// Constructs this from an Asio acceptor.
  explicit basic_safe_acceptor(acceptor_type acceptor) : acceptor_(std::move(acceptor)) {}

  /// Allows access to the underlying (unsynchronized) Asio object.
  acceptor_type& acceptor_unsafe() noexcept { return acceptor_; }
  /// Allows access to the underlying (unsynchronized) Asio object.
  const acceptor_type& acceptor_unsafe() const noexcept { return acceptor_; }

  tmc::task<bool> is_open() noexcept {
    co_await mut_;
    // Manual unlock is required since this coro didn't suspend
    co_await mut_.co_unlock_return(acceptor_.is_open());
    TMC_UNREACHABLE;
  }

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

  tmc::task<error_code>
  listen(int backlog = tmc::detail::asio_impl::socket_base::max_listen_connections) {
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

  tmc::task<std::tuple<error_code, socket_type, endpoint_type>> async_accept_endpoint() {
    co_await mut_;
    // `peer` is filled before the completion handler runs; it lives in this
    // coroutine frame, so it outlives the async operation.
    endpoint_type peer;
    auto [ec, sock] = co_await acceptor_.async_accept(peer, tmc::aw_asio);
    co_return std::tuple{ec, std::move(sock), std::move(peer)};
  }

  tmc::task<std::tuple<error_code>> async_wait(wait_type w) {
    co_await mut_;

    co_return co_await acceptor_.async_wait(w, tmc::aw_asio);
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

  tmc::task<executor_type> get_executor() {
    co_await mut_;

    executor_type ex = acceptor_.get_executor();

    // Manual unlock is required since this coro didn't suspend
    co_await mut_.co_unlock_return(std::move(ex));
    TMC_UNREACHABLE;
  }

  tmc::task<error_code>
  assign(protocol_type protocol, native_handle_type native_acceptor) {
    co_await mut_;

    error_code ec;
    TMC_ASIO_SYNC_DISCARD(acceptor_.assign(protocol, native_acceptor, ec));

    // Manual unlock is required since this coro didn't suspend
    co_await mut_.co_unlock_return(ec);
    TMC_UNREACHABLE;
  }

  tmc::task<std::tuple<error_code, native_handle_type>> release() {
    co_await mut_;

    error_code ec;
    native_handle_type handle = acceptor_.release(ec);

    // Manual unlock is required since this coro didn't suspend
    co_await mut_.co_unlock_return(std::tuple{ec, handle});
    TMC_UNREACHABLE;
  }

  template <typename GettableSocketOption>
  tmc::task<std::tuple<error_code, GettableSocketOption>>
  get_option(GettableSocketOption option) {
    co_await mut_;

    error_code ec;
    TMC_ASIO_SYNC_DISCARD(acceptor_.get_option(option, ec));

    // Manual unlock is required since this coro didn't suspend
    co_await mut_.co_unlock_return(std::tuple{ec, std::move(option)});
    TMC_UNREACHABLE;
  }

  template <typename IoControlCommand>
  tmc::task<std::tuple<error_code, IoControlCommand>>
  io_control(IoControlCommand command) {
    co_await mut_;

    error_code ec;
    TMC_ASIO_SYNC_DISCARD(acceptor_.io_control(command, ec));

    // Manual unlock is required since this coro didn't suspend
    co_await mut_.co_unlock_return(std::tuple{ec, std::move(command)});
    TMC_UNREACHABLE;
  }

  tmc::task<bool> non_blocking() {
    co_await mut_;

    // Manual unlock is required since this coro didn't suspend
    co_await mut_.co_unlock_return(acceptor_.non_blocking());
    TMC_UNREACHABLE;
  }

  tmc::task<error_code> non_blocking(bool mode) {
    co_await mut_;

    error_code ec;
    TMC_ASIO_SYNC_DISCARD(acceptor_.non_blocking(mode, ec));

    // Manual unlock is required since this coro didn't suspend
    co_await mut_.co_unlock_return(ec);
    TMC_UNREACHABLE;
  }

  tmc::task<bool> native_non_blocking() {
    co_await mut_;

    // Manual unlock is required since this coro didn't suspend
    co_await mut_.co_unlock_return(acceptor_.native_non_blocking());
    TMC_UNREACHABLE;
  }

  tmc::task<error_code> native_non_blocking(bool mode) {
    co_await mut_;

    error_code ec;
    TMC_ASIO_SYNC_DISCARD(acceptor_.native_non_blocking(mode, ec));

    // Manual unlock is required since this coro didn't suspend
    co_await mut_.co_unlock_return(ec);
    TMC_UNREACHABLE;
  }

  tmc::task<std::tuple<error_code, endpoint_type>> local_endpoint() {
    co_await mut_;

    error_code ec;
    endpoint_type ep = acceptor_.local_endpoint(ec);

    // Manual unlock is required since this coro didn't suspend
    co_await mut_.co_unlock_return(std::tuple{ec, std::move(ep)});
    TMC_UNREACHABLE;
  }

  tmc::task<native_handle_type> native_handle() {
    co_await mut_;

    // Manual unlock is required since this coro didn't suspend
    co_await mut_.co_unlock_return(acceptor_.native_handle());
    TMC_UNREACHABLE;
  }
};

} // namespace tmc
