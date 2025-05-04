// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/compat.hpp"
#include "tmc/detail/concepts_awaitable.hpp"
#include "tmc/detail/waiter_list.hpp"

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace tmc {
class semaphore;

class aw_semaphore {
  tmc::detail::waiter_list_node me;
  semaphore* parent;

  friend class semaphore;

  inline aw_semaphore(semaphore* Parent) noexcept : parent(Parent) {}

public:
  bool await_ready() noexcept;

  bool await_suspend(std::coroutine_handle<> Outer) noexcept;

  inline void await_resume() noexcept {}

  // Cannot be moved or copied due to holding intrusive list pointer
  aw_semaphore(aw_semaphore const&) = delete;
  aw_semaphore& operator=(aw_semaphore const&) = delete;
  aw_semaphore(aw_semaphore&&) = delete;
  aw_semaphore& operator=(aw_semaphore&&) = delete;
};

class semaphore {
  tmc::detail::waiter_list waiters;
  // Low half bits are the semaphore value.
  // High half bits are the number of waiters.
  std::atomic<size_t> value;

  friend class aw_semaphore;

  static inline constexpr size_t WAITERS_OFFSET = TMC_PLATFORM_BITS / 2;
  static inline constexpr size_t HALF_MASK =
    (TMC_ONE_BIT << (TMC_PLATFORM_BITS / 2)) - 1;

  static inline void unpack_value(
    size_t Value, size_t& Count_out, size_t& WaiterCount_out
  ) noexcept {
    Count_out = Value & HALF_MASK;
    WaiterCount_out = (Value >> WAITERS_OFFSET) & HALF_MASK;
  }

  static inline size_t pack_value(size_t Count, size_t WaiterCount) noexcept {
    return (WaiterCount << WAITERS_OFFSET) | Count;
  }

  // Called after increasing Count or WaiterCount.
  // If Count > 0 && WaiterCount > 0, this will try to wake some number of
  // awaiters.
  void maybe_wake(size_t v) noexcept;

public:
  /// The count is packed into half a machine word along with the awaiter count.
  /// Thus it is only allowed half a machine word of bits.
  using half_word =
    std::conditional_t<TMC_PLATFORM_BITS == 64, uint32_t, uint16_t>;

  /// Sets the initial number of resources available in the semaphore.
  /// This is only an initial value and not a maximum; by calling release(), the
  /// number of resources available may exceed this initial value.
  inline semaphore(half_word InitialCount) noexcept
      : value{static_cast<size_t>(InitialCount)} {}

  /// Returns an estimate of the current number of available resources.
  /// This value is not guaranteed to be consistent with any other operation.
  /// Even if this returns non-zero, awaiting afterward may suspend.
  /// Even if this returns zero, awaiting afterward may not suspend.
  inline size_t count() noexcept {
    return HALF_MASK & value.load(std::memory_order_relaxed);
  }

  /// Returns true if the count was non-zero and was successfully decremented.
  /// Returns false if the count was zero.
  bool try_acquire() noexcept;

  /// Increases the available resources by ReleaseCount. If there are waiting
  /// awaiters, they will be awoken until all resources have been consumed.
  void release(size_t ReleaseCount = 1) noexcept;

  /// Tries to acquire the semaphore, and if no resources are ready, will
  /// suspend until a resource becomes ready.
  inline aw_semaphore operator co_await() noexcept {
    return aw_semaphore(this);
  }

  /// On destruction, any waiting awaiters will be resumed.
  ~semaphore();
};

namespace detail {
template <> struct awaitable_traits<tmc::semaphore> {
  static constexpr configure_mode mode = WRAPPER;

  using result_type = void;
  using self_type = tmc::semaphore;
  using awaiter_type = tmc::aw_semaphore;

  static awaiter_type get_awaiter(self_type& Awaitable) noexcept {
    return Awaitable.operator co_await();
  }
};
} // namespace detail
} // namespace tmc

#ifdef TMC_IMPL
#include "tmc/detail/semaphore.ipp"
#endif
