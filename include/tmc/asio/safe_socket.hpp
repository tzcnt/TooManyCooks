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

/// Type that serializes Asio socket operations so they can be initiated safely from
/// different coroutines. Unlike a strand, this only serializes the initiation of
/// operations; it does not serialize full handlers.
///
/// It does not prevent overlapping reads and writes. The user is responsible
/// for ensuring at most 1 read and 1 write are active at any time, per the usual Asio
/// contract.
///
/// Methods behave exactly as the underlying object's methods with the same name, except:
/// - methods are thread-safe
/// - methods implicitly use the `tmc::aw_asio` completion
///
/// The safe_socket must outlive every task that uses it, including any task still
/// waiting to acquire its mutex. Destroying it while such tasks exist is a
/// use-after-free.
class safe_socket {
public:
  using socket_type = tmc::detail::asio_impl::ip::tcp::socket;
  using endpoint_type = tmc::detail::asio_impl::ip::tcp::endpoint;
  using protocol_type = socket_type::protocol_type;
  using executor_type = socket_type::executor_type;
  using native_handle_type = socket_type::native_handle_type;
  using wait_type = socket_type::wait_type;
  using message_flags = socket_type::message_flags;
#ifdef TMC_USE_BOOST_ASIO
  using error_code = boost::system::error_code;
#else
  using error_code = asio::error_code;
#endif

private:
  socket_type socket_;
  tmc::tiny_mutex mut_;

  // Set true while a composed async_read()/async_write() is running, and
  // cleared by cancel(). This closes the window where a cancel() lands between
  // the composed operation's single-shot reads/writes - when nothing is pending
  // on the socket for socket_.cancel() to hit. Only touched under mut_.
  bool read_active_ = false;
  bool write_active_ = false;

public:
  /// Constructs this from an Asio ip::tcp::socket.
  explicit safe_socket(socket_type socket) : socket_(std::move(socket)) {}

  /// Allows access to the underlying (unsynchronized) Asio object.
  socket_type& socket_unsafe() noexcept { return socket_; }

  /// Allows access to the underlying (unsynchronized) Asio object.
  const socket_type& socket_unsafe() const noexcept { return socket_; }

  tmc::task<bool> is_open() noexcept {
    co_await mut_;
    // Manual unlock is required since this coro didn't suspend
    co_await mut_.co_unlock_return(socket_.is_open());
    TMC_UNREACHABLE;
  }

  tmc::task<std::tuple<error_code>> async_connect(endpoint_type endpoint) {
    co_await mut_;

    co_return co_await socket_.async_connect(endpoint, tmc::aw_asio);
  }

  // Reads until the buffer sequence is full, EOF, or another error occurs
  // (like `asio::async_read`). Unlike the asio composed operation - whose
  // intermediate re-initiations would run on the I/O executor, outside the
  // mutex - this is implemented as a loop of single-shot reads, so every
  // initiation is serialized against other operations on this object.
  //
  // Only one read may be in flight at a time (the usual asio stream contract).
  template <typename MutableBufferSequence>
  tmc::task<std::tuple<error_code, std::size_t>>
  async_read(MutableBufferSequence buffers) {
    std::size_t total = 0;
    auto it = tmc::detail::asio_impl::buffer_sequence_begin(buffers);
    auto end = tmc::detail::asio_impl::buffer_sequence_end(buffers);
    bool started = false;
    for (; it != end; ++it) {
      tmc::detail::asio_impl::mutable_buffer b = *it;
      while (b.size() != 0) {
        co_await mut_;
        // A cancel() that landed between our single-shot reads (when nothing
        // was pending for socket_.cancel() to hit) cleared read_active_. Abort
        // here at the chunk boundary so the cancel is never silently dropped.
        if (started && !read_active_) {
          co_return std::tuple{
            error_code(tmc::detail::asio_impl::error::operation_aborted), total
          };
        }
        read_active_ = true;
        started = true;
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
  // initiation is serialized against other operations on this object.
  //
  // Only one write may be in flight at a time (the usual asio stream contract).
  template <typename ConstBufferSequence>
  tmc::task<std::tuple<error_code, std::size_t>>
  async_write(ConstBufferSequence buffers) {
    std::size_t total = 0;
    auto it = tmc::detail::asio_impl::buffer_sequence_begin(buffers);
    auto end = tmc::detail::asio_impl::buffer_sequence_end(buffers);
    bool started = false;
    for (; it != end; ++it) {
      tmc::detail::asio_impl::const_buffer b = *it;
      while (b.size() != 0) {
        co_await mut_;
        // A cancel() that landed between our single-shot writes (when nothing
        // was pending for socket_.cancel() to hit) cleared write_active_. Abort
        // here at the chunk boundary so the cancel is never silently dropped.
        if (started && !write_active_) {
          co_return std::tuple{
            error_code(tmc::detail::asio_impl::error::operation_aborted), total
          };
        }
        write_active_ = true;
        started = true;
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

    // Cancel any single-shot operation currently pending on the socket...
    error_code ec;
    TMC_ASIO_SYNC_DISCARD(socket_.cancel(ec));

    // ...and, in case a composed async_read()/async_write() is sitting between
    // its single-shot operations (where there is nothing pending for the call
    // above to hit), signal it to abort when it re-acquires the mutex.
    read_active_ = false;
    write_active_ = false;

    // Manual unlock is required since this coro didn't suspend
    co_await mut_.co_unlock_return(ec);
    TMC_UNREACHABLE;
  }

  tmc::task<error_code> shutdown(socket_type::shutdown_type how) {
    co_await mut_;

    error_code ec;
    TMC_ASIO_SYNC_DISCARD(socket_.shutdown(how, ec));

    // Manual unlock is required since this coro didn't suspend
    co_await mut_.co_unlock_return(ec);
    TMC_UNREACHABLE;
  }

  tmc::task<error_code> close() {
    co_await mut_;

    error_code ec;
    TMC_ASIO_SYNC_DISCARD(socket_.close(ec));

    // Manual unlock is required since this coro didn't suspend
    co_await mut_.co_unlock_return(ec);
    TMC_UNREACHABLE;
  }

  tmc::task<std::tuple<error_code>> async_wait(wait_type w) {
    co_await mut_;

    co_return co_await socket_.async_wait(w, tmc::aw_asio);
  }

  template <typename ConstBufferSequence>
  tmc::task<std::tuple<error_code, std::size_t>> async_send(ConstBufferSequence buffers) {
    co_await mut_;

    co_return co_await socket_.async_send(std::move(buffers), tmc::aw_asio);
  }

  template <typename ConstBufferSequence>
  tmc::task<std::tuple<error_code, std::size_t>>
  async_send(ConstBufferSequence buffers, message_flags flags) {
    co_await mut_;

    co_return co_await socket_.async_send(std::move(buffers), flags, tmc::aw_asio);
  }

  template <typename MutableBufferSequence>
  tmc::task<std::tuple<error_code, std::size_t>>
  async_receive(MutableBufferSequence buffers) {
    co_await mut_;

    co_return co_await socket_.async_receive(std::move(buffers), tmc::aw_asio);
  }

  template <typename MutableBufferSequence>
  tmc::task<std::tuple<error_code, std::size_t>>
  async_receive(MutableBufferSequence buffers, message_flags flags) {
    co_await mut_;

    co_return co_await socket_.async_receive(std::move(buffers), flags, tmc::aw_asio);
  }

  tmc::task<executor_type> get_executor() {
    co_await mut_;

    executor_type ex = socket_.get_executor();

    // Manual unlock is required since this coro didn't suspend
    co_await mut_.co_unlock_return(std::move(ex));
    TMC_UNREACHABLE;
  }

  tmc::task<error_code> open(protocol_type protocol) {
    co_await mut_;

    error_code ec;
    TMC_ASIO_SYNC_DISCARD(socket_.open(protocol, ec));

    // Manual unlock is required since this coro didn't suspend
    co_await mut_.co_unlock_return(ec);
    TMC_UNREACHABLE;
  }

  tmc::task<error_code> assign(protocol_type protocol, native_handle_type native_socket) {
    co_await mut_;

    error_code ec;
    TMC_ASIO_SYNC_DISCARD(socket_.assign(protocol, native_socket, ec));

    // Manual unlock is required since this coro didn't suspend
    co_await mut_.co_unlock_return(ec);
    TMC_UNREACHABLE;
  }

  tmc::task<std::tuple<error_code, native_handle_type>> release() {
    co_await mut_;

    error_code ec;
    native_handle_type handle = socket_.release(ec);

    // Manual unlock is required since this coro didn't suspend
    co_await mut_.co_unlock_return(std::tuple{ec, handle});
    TMC_UNREACHABLE;
  }

  tmc::task<error_code> bind(endpoint_type endpoint) {
    co_await mut_;

    error_code ec;
    TMC_ASIO_SYNC_DISCARD(socket_.bind(endpoint, ec));

    // Manual unlock is required since this coro didn't suspend
    co_await mut_.co_unlock_return(ec);
    TMC_UNREACHABLE;
  }

  template <typename SettableSocketOption>
  tmc::task<error_code> set_option(SettableSocketOption option) {
    co_await mut_;

    error_code ec;
    TMC_ASIO_SYNC_DISCARD(socket_.set_option(option, ec));

    // Manual unlock is required since this coro didn't suspend
    co_await mut_.co_unlock_return(ec);
    TMC_UNREACHABLE;
  }

  template <typename GettableSocketOption>
  tmc::task<std::tuple<error_code, GettableSocketOption>>
  get_option(GettableSocketOption option) {
    co_await mut_;

    error_code ec;
    TMC_ASIO_SYNC_DISCARD(socket_.get_option(option, ec));

    // Manual unlock is required since this coro didn't suspend
    co_await mut_.co_unlock_return(std::tuple{ec, std::move(option)});
    TMC_UNREACHABLE;
  }

  template <typename IoControlCommand>
  tmc::task<std::tuple<error_code, IoControlCommand>>
  io_control(IoControlCommand command) {
    co_await mut_;

    error_code ec;
    TMC_ASIO_SYNC_DISCARD(socket_.io_control(command, ec));

    // Manual unlock is required since this coro didn't suspend
    co_await mut_.co_unlock_return(std::tuple{ec, std::move(command)});
    TMC_UNREACHABLE;
  }

  /// Returns the non-blocking mode of the socket.
  tmc::task<bool> non_blocking() {
    co_await mut_;

    // Manual unlock is required since this coro didn't suspend
    co_await mut_.co_unlock_return(socket_.non_blocking());
    TMC_UNREACHABLE;
  }

  tmc::task<error_code> non_blocking(bool mode) {
    co_await mut_;

    error_code ec;
    TMC_ASIO_SYNC_DISCARD(socket_.non_blocking(mode, ec));

    // Manual unlock is required since this coro didn't suspend
    co_await mut_.co_unlock_return(ec);
    TMC_UNREACHABLE;
  }

  tmc::task<bool> native_non_blocking() {
    co_await mut_;

    // Manual unlock is required since this coro didn't suspend
    co_await mut_.co_unlock_return(socket_.native_non_blocking());
    TMC_UNREACHABLE;
  }

  tmc::task<error_code> native_non_blocking(bool mode) {
    co_await mut_;

    error_code ec;
    TMC_ASIO_SYNC_DISCARD(socket_.native_non_blocking(mode, ec));

    // Manual unlock is required since this coro didn't suspend
    co_await mut_.co_unlock_return(ec);
    TMC_UNREACHABLE;
  }

  tmc::task<std::tuple<error_code, endpoint_type>> local_endpoint() {
    co_await mut_;

    error_code ec;
    endpoint_type ep = socket_.local_endpoint(ec);

    // Manual unlock is required since this coro didn't suspend
    co_await mut_.co_unlock_return(std::tuple{ec, std::move(ep)});
    TMC_UNREACHABLE;
  }

  tmc::task<std::tuple<error_code, endpoint_type>> remote_endpoint() {
    co_await mut_;

    error_code ec;
    endpoint_type ep = socket_.remote_endpoint(ec);

    // Manual unlock is required since this coro didn't suspend
    co_await mut_.co_unlock_return(std::tuple{ec, std::move(ep)});
    TMC_UNREACHABLE;
  }

  tmc::task<std::tuple<error_code, bool>> at_mark() {
    co_await mut_;

    error_code ec;
    bool marked = socket_.at_mark(ec);

    // Manual unlock is required since this coro didn't suspend
    co_await mut_.co_unlock_return(std::tuple{ec, marked});
    TMC_UNREACHABLE;
  }

  tmc::task<std::tuple<error_code, std::size_t>> available() {
    co_await mut_;

    error_code ec;
    std::size_t n = socket_.available(ec);

    // Manual unlock is required since this coro didn't suspend
    co_await mut_.co_unlock_return(std::tuple{ec, n});
    TMC_UNREACHABLE;
  }

  tmc::task<native_handle_type> native_handle() {
    co_await mut_;

    // Manual unlock is required since this coro didn't suspend
    co_await mut_.co_unlock_return(socket_.native_handle());
    TMC_UNREACHABLE;
  }
};

} // namespace tmc
