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
  inline aw_barrier(barrier* Parent) : parent(Parent) {}

  friend class barrier;

public:
  inline bool await_ready() noexcept { return false; }
  inline void await_resume() noexcept {}

  bool await_suspend(std::coroutine_handle<> Outer);

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
  std::atomic<tmc::detail::waiter_list_node*> waiters;
  std::atomic<ptrdiff_t> start_count;
  std::atomic<ptrdiff_t> done_count;

  friend class aw_barrier;

public:
  inline barrier(size_t Count)
      : waiters(nullptr), start_count{static_cast<ptrdiff_t>(Count - 1)},
        done_count{static_cast<ptrdiff_t>(Count - 1)} {}

  inline aw_barrier operator co_await() { return aw_barrier(this); }
};
namespace detail {
template <> struct awaitable_traits<tmc::barrier> {
  static constexpr configure_mode mode = WRAPPER;

  using result_type = void;
  using self_type = tmc::barrier;
  using awaiter_type = tmc::aw_barrier;

  static awaiter_type get_awaiter(self_type& Awaitable) {
    return Awaitable.operator co_await();
  }
};
} // namespace detail
} // namespace tmc

#ifdef TMC_IMPL
#include "tmc/detail/barrier.ipp"
#endif
