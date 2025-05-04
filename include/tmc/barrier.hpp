// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/concepts_awaitable.hpp"
#include "tmc/detail/waiter_list.hpp"

#include <atomic>
#include <coroutine>
#include <cstddef>

namespace tmc {
class barrier;

class aw_barrier {
  tmc::detail::waiter_list_node me;
  barrier* parent;

  friend class barrier;

  inline aw_barrier(barrier* Parent) noexcept : parent(Parent) {}

public:
  inline bool await_ready() noexcept {
    // Don't bother optimistically checking if we're ready in a barrier.
    // The majority of the time it will not be ready.
    return false;
  }
  inline void await_resume() noexcept {}

  bool await_suspend(std::coroutine_handle<> Outer) noexcept;

  // Cannot be moved or copied due to holding intrusive list pointer
  aw_barrier(aw_barrier const&) = delete;
  aw_barrier& operator=(aw_barrier const&) = delete;
  aw_barrier(aw_barrier&&) = delete;
  aw_barrier& operator=(aw_barrier&&) = delete;
};

/// Similar semantics to std::barrier, although it only exposes one operation -
/// `co_await` is equivalent to `std::barrier::arrive_and_wait`.
/// Like std::barrier, the count will be automatically reset after all awaiters
/// arrive, and it may be reused afterward.
class barrier {
  tmc::detail::waiter_list waiters;
  std::atomic<ptrdiff_t> start_count;
  std::atomic<ptrdiff_t> done_count;

  friend class aw_barrier;

public:
  /// Sets the number of awaiters for the barrier. Setting this to zero or a
  /// negative number will cause awaiters to resume immediately.
  inline barrier(size_t Count) noexcept
      : start_count{static_cast<ptrdiff_t>(Count - 1)},
        done_count{static_cast<ptrdiff_t>(Count - 1)} {}

  /// Equivalent to `std::barrier::arrive_and_wait`. Decrements the barrier
  /// count, and if the count reaches 0, wakes all awaiters, and resets the
  /// count to the original maximum as specified in the constructor. Otherwise,
  /// suspends until Count awaiters have reached this point.
  inline aw_barrier operator co_await() noexcept { return aw_barrier(this); }

  /// On destruction, any awaiters will be resumed.
  ~barrier();
};
namespace detail {
template <> struct awaitable_traits<tmc::barrier> {
  static constexpr configure_mode mode = WRAPPER;

  using result_type = void;
  using self_type = tmc::barrier;
  using awaiter_type = tmc::aw_barrier;

  static awaiter_type get_awaiter(self_type& Awaitable) noexcept {
    return Awaitable.operator co_await();
  }
};
} // namespace detail
} // namespace tmc

#ifdef TMC_IMPL
#include "tmc/detail/barrier.ipp"
#endif
