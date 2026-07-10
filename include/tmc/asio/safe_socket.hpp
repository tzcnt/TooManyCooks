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
#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/system/error_code.hpp>
#else
#include <asio/buffer.hpp>
#include <asio/error.hpp>
#include <asio/error_code.hpp>
#include <asio/ip/tcp.hpp>
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

// Type that serializes socket operations so they can be initiated safely from
// different coroutines. The tiny_mutex is released automatically when the
// coroutine next suspends.
class SafeSocket {
public:
  using socket_type = tmc::detail::asio_impl::ip::tcp::socket;
  using endpoint_type = tmc::detail::asio_impl::ip::tcp::endpoint;
#ifdef TMC_USE_BOOST_ASIO
  using error_code = boost::system::error_code;
#else
  using error_code = asio::error_code;
#endif

private:
  socket_type socket_;
  tmc::tiny_mutex mut_;

public:
  explicit SafeSocket(socket_type socket) : socket_(std::move(socket)) {}

  socket_type& socket_unsafe() noexcept { return socket_; }
  const socket_type& socket_unsafe() const noexcept { return socket_; }

  bool is_open() const noexcept { return socket_.is_open(); }

  tmc::task<std::tuple<error_code>> async_connect(endpoint_type endpoint) {
    co_await mut_;

    co_return co_await socket_.async_connect(endpoint, tmc::aw_asio);
  }

  // Reads until the buffer sequence is full, EOF, or another error occurs
  // (like `asio::async_read`). Unlike the asio composed operation - whose
  // intermediate re-initiations would run on the I/O executor, outside the
  // mutex - this is implemented as a loop of single-shot reads, so every
  // initiation is serialized against other operations on this object. A
  // concurrent cancel() or shutdown_full() takes effect at a chunk boundary.
  //
  // Only one read may be in flight at a time (the usual asio stream contract).
  template <typename MutableBufferSequence>
  tmc::task<std::tuple<error_code, std::size_t>>
  async_read(MutableBufferSequence buffers) {
    std::size_t total = 0;
    auto it = tmc::detail::asio_impl::buffer_sequence_begin(buffers);
    auto end = tmc::detail::asio_impl::buffer_sequence_end(buffers);
    for (; it != end; ++it) {
      tmc::detail::asio_impl::mutable_buffer b = *it;
      while (b.size() != 0) {
        co_await mut_;
        auto [ec, n] = co_await socket_.async_read_some(b, tmc::aw_asio);
        total += n;
        b += n;
        if (ec) {
          co_return std::tuple{ec, total};
        }
      }
    }
    co_return std::tuple{error_code{}, total};
  }

  template <typename MutableBufferSequence>
  tmc::task<std::tuple<error_code, std::size_t>>
  async_read_some(MutableBufferSequence buffers) {
    co_await mut_;

    co_return co_await socket_.async_read_some(std::move(buffers), tmc::aw_asio);
  }

  // Writes the entire buffer sequence unless an error occurs (like
  // `asio::async_write`). Unlike the asio composed operation - whose
  // intermediate re-initiations would run on the I/O executor, outside the
  // mutex - this is implemented as a loop of single-shot writes, so every
  // initiation is serialized against other operations on this object. A
  // concurrent cancel() or shutdown_full() takes effect at a chunk boundary.
  //
  // Only one write may be in flight at a time (the usual asio stream contract).
  template <typename ConstBufferSequence>
  tmc::task<std::tuple<error_code, std::size_t>>
  async_write(ConstBufferSequence buffers) {
    std::size_t total = 0;
    auto it = tmc::detail::asio_impl::buffer_sequence_begin(buffers);
    auto end = tmc::detail::asio_impl::buffer_sequence_end(buffers);
    for (; it != end; ++it) {
      tmc::detail::asio_impl::const_buffer b = *it;
      while (b.size() != 0) {
        co_await mut_;
        auto [ec, n] = co_await socket_.async_write_some(b, tmc::aw_asio);
        total += n;
        b += n;
        if (ec) {
          co_return std::tuple{ec, total};
        }
      }
    }
    co_return std::tuple{error_code{}, total};
  }

  template <typename ConstBufferSequence>
  tmc::task<std::tuple<error_code, std::size_t>>
  async_write_some(ConstBufferSequence buffers) {
    co_await mut_;

    co_return co_await socket_.async_write_some(std::move(buffers), tmc::aw_asio);
  }

  tmc::task<error_code> cancel() {
    co_await mut_;

    error_code ec;
    socket_.cancel(ec);

    // Manual unlock is required since this coro didn't suspend
    co_await mut_.co_unlock_return(ec);
    TMC_UNREACHABLE;
  }

  // tmc::task<error_code>
  // shutdown(boost::asio::ip::tcp::socket::shutdown_type how) {
  //   co_await mut_;

  //   error_code ec;
  //   socket_.shutdown(how, ec);

  //   // Manual unlock is required since this coro didn't suspend
  //   co_await mut_.co_unlock();

  //   co_return ec;
  // }

  // tmc::task<error_code> close() {
  //   co_await mut_;

  //   error_code ec;
  //   socket_.close(ec);

  //   // Manual unlock is required since this coro didn't suspend
  //   co_await mut_.co_unlock();

  //   co_return ec;
  // }

  // Cancels pending operations, sends FIN, and closes the socket. Returns the
  // first error encountered, except that `not_connected` from shutdown() is
  // treated as success (the peer already closed the connection).
  tmc::task<error_code> shutdown_full() {
    co_await mut_;

    // TODO - may be useful to split this into cancel + shutdown_send, then
    // drain the rest of incoming messages and then shutdown_recv and close(),
    // if necessary to make the protocol shutdown properly graceful. Linux
    // kernel may send RST if there is read pending data after the shutdown was
    // processed.

    // The most graceful shutdown:
    // On the close initiator side:
    // 1. Wait until current write operation completes (this doesn't means that
    // data are sent).
    // 2. Call shutdown(SD_SEND/SHUT_WR).
    // 3. Wait until read operation returns EOF.
    // 4. Close socket.

    // On the other side:
    // 1. Close starts if EOF is received.
    // 2. Wait until current write operation completes.
    // 3. Call shutdown(SD_SEND/SHUT_WR).
    // 4. EOF was already received, so we can close the socket.

    error_code ec;

    if (socket_.is_open()) {
      // cancels pending operations (shutdown doesn't do this)
      // this cannot be called on a closed socket, hence the is_open() check
      socket_.cancel(ec);
      // send FIN packet to other end
      error_code shutdownEc;
      socket_.shutdown(socket_type::shutdown_both, shutdownEc);
      if (shutdownEc == tmc::detail::asio_impl::error::not_connected) {
        shutdownEc = {};
      }
      if (!ec) {
        ec = shutdownEc;
      }
      // destroy the connection
      error_code closeEc;
      socket_.close(closeEc);
      if (!ec) {
        ec = closeEc;
      }
    }

    // Manual unlock is required since this coro didn't suspend
    co_await mut_.co_unlock_return(ec);
    TMC_UNREACHABLE;
  }
};

} // namespace tmc
