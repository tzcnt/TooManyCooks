// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/asio/aw_asio.hpp"
#include "tmc/task.hpp"

#include "tmc/detail/tiny_mutex.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <cstddef>
#include <tuple>
#include <utility>

namespace tmc {

// Type that serializes socket operations so they can be initiated safely from
// different coroutines. The tiny_mutex is released automatically when the
// coroutine next suspends.
class SafeSocket {
public:
  using socket_type = boost::asio::ip::tcp::socket;
  using error_code = boost::system::error_code;

private:
  socket_type socket_;
  tmc::tiny_mutex mut_;

public:
  explicit SafeSocket(socket_type socket) : socket_(std::move(socket)) {}

  socket_type& socket_unsafe() noexcept { return socket_; }
  const socket_type& socket_unsafe() const noexcept { return socket_; }

  bool is_open() const noexcept { return socket_.is_open(); }

  tmc::task<std::tuple<error_code>>
  async_connect(boost::asio::ip::tcp::endpoint endpoint) {
    co_await mut_;

    co_return co_await socket_.async_connect(endpoint, tmc::aw_asio);
  }

  template <typename MutableBufferSequence>
  tmc::task<std::tuple<error_code, std::size_t>>
  async_read(MutableBufferSequence buffers) {
    co_await mut_;

    co_return co_await boost::asio::async_read(socket_, std::move(buffers), tmc::aw_asio);
  }

  template <typename MutableBufferSequence>
  tmc::task<std::tuple<error_code, std::size_t>>
  async_read_some(MutableBufferSequence buffers) {
    co_await mut_;

    co_return co_await socket_.async_read_some(std::move(buffers), tmc::aw_asio);
  }

  template <typename ConstBufferSequence>
  tmc::task<std::tuple<error_code, std::size_t>>
  async_write(ConstBufferSequence buffers) {
    co_await mut_;

    co_return co_await boost::asio::async_write(
      socket_, std::move(buffers), tmc::aw_asio
    );
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
    std::unreachable();
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
      socket_.cancel();
      // send FIN packet to other end
      socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
      // destroy the connection
      socket_.close(ec);
    }

    // Manual unlock is required since this coro didn't suspend
    co_await mut_.co_unlock_return(ec);
    std::unreachable();
  }
};

} // namespace tmc
