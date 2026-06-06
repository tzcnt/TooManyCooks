// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once
#include "tmc/detail/impl.hpp" // IWYU pragma: keep

// Common implementation bits of mutex, semaphore, auto_reset_event,
// manual_reset_event, and barrier.

#include "tmc/ex_any.hpp"

#include <atomic>
#include <coroutine>
#include <cstddef>

namespace tmc {
namespace detail {

class waiter_list;

struct waiter_list_waiter {
  std::coroutine_handle<> continuation;
  tmc::ex_any* continuation_executor;
  size_t continuation_priority;

  /// Submits this to the executor to be resumed
  TMC_DECL void resume() noexcept;

  /// If this expects to resume on the same executor and priority as the Outer
  /// task, then returns this and posts the outer task to the executor. If this
  /// expects to resume on a different executor or priority, then posts this to
  /// its executor and returns the outer task.
  [[nodiscard]] TMC_DECL std::coroutine_handle<>
  try_symmetric_transfer(std::coroutine_handle<> Outer) noexcept;
};

struct waiter_data_base;

struct waiter_list_node {
  waiter_list_node* next;
  waiter_list_waiter waiter;

  /// Used by mutex, semaphore, and auto_reset_event acquire awaitables.
  TMC_DECL void
  suspend(waiter_data_base* Parent, std::coroutine_handle<> Outer) noexcept;
};

/// Two-stack waiter queue with FIFO wake order.
///
/// `input` is a lock-free Treiber stack that producers push to via
/// `add_waiter` (LIFO). `output` is a private, single-consumer list of
/// waiters in FIFO order, only ever touched by the thread that currently
/// holds the `maybe_wake` critical section (or callers that the user has
/// otherwise serialized, such as the barrier's terminal arriver and
/// destructors). When `output` is empty and a consumer needs a waiter, the
/// input stack is atomically swapped out and reversed into `output`, which
/// converts the per-stack LIFO order into overall FIFO wake order.
class waiter_list {
  std::atomic<waiter_list_node*> input;
  waiter_list_node* output;

public:
  inline waiter_list() noexcept : input{nullptr}, output{nullptr} {}

  /// Adds w to the input stack. Lock-free.
  /// Thread-safe.
  TMC_DECL void add_waiter(waiter_list_node& w) noexcept;

  /// Wakes all waiters. Not guaranteed to wake in FIFO order (which doesn't
  /// matter since waking all waiters doesn't guarantee they get *processed* in
  /// FIFO order either after going through their executor queues). Both lists
  /// become empty. Thread-safe with concurrent `add_waiter`, but not with
  /// concurrent consumers (`maybe_wake`, `must_take_1`, `take_all`).
  TMC_DECL void wake_all() noexcept;

  /// Returns the number of waiters currently in the list (both `output` and
  /// `input`). For testing purposes. Safe to use concurrently with new
  /// waiters. Not safe to use concurrently with new wakers which may cause
  /// waiters to be resumed and then destroyed.
  [[nodiscard]] TMC_DECL size_t size() const noexcept;

  /// Returns a singly-linked chain of all waiters. Not guaranteed to take in
  /// FIFO order. Both lists become empty. Thread-safe with concurrent
  /// `add_waiter`, but not with concurrent consumers (`maybe_wake`,
  /// `must_take_1`, `take_all`).
  [[nodiscard]] TMC_DECL waiter_list_node* take_all() noexcept;

  /// Assumes at least 1 waiter is in the list.
  /// Takes the FIFO-oldest waiter.
  /// Not thread-safe: only call while holding the `maybe_wake` critical
  /// section (or another single-consumer guarantee).
  [[nodiscard]] TMC_DECL waiter_list_node* must_take_1() noexcept;

  /// Used by mutex and semaphore.
  /// Called after increasing Count or WaiterCount.
  /// If Count > 0 && WaiterCount > 0, this will try to wake some number of
  /// awaiters. If symmetric == true, may return a waiter for symmetric
  /// transfer. If symmetric == false, always returns nullptr.
  TMC_DECL waiter_list_waiter* maybe_wake(
    std::atomic<size_t>& value, size_t v, size_t old, bool symmetric
  ) noexcept;
};

// Utilities used by various awaitables that hold a waiter_list
// such as mutex, semaphore, and auto_reset_event.

static inline constexpr size_t WAITERS_OFFSET = TMC_PLATFORM_BITS / 2;
static inline constexpr size_t HALF_MASK =
  (TMC_ONE_BIT << (TMC_PLATFORM_BITS / 2)) - 1;

using half_word =
  std::conditional_t<TMC_PLATFORM_BITS == 64, uint32_t, uint16_t>;

static inline void unpack_value(
  size_t Value, half_word& Count_out, size_t& WaiterCount_out
) noexcept {
  Count_out = static_cast<half_word>(Value & HALF_MASK);
  WaiterCount_out = (Value >> WAITERS_OFFSET) & HALF_MASK;
}

static inline size_t pack_value(half_word Count, size_t WaiterCount) noexcept {
  return (WaiterCount << WAITERS_OFFSET) | static_cast<size_t>(Count);
}

/// Used by co_await of mutex, semaphore, and auto_reset_event.
/// Returns true if the count was non-zero and was successfully decremented.
/// Returns false if the count was zero.
TMC_DECL bool try_acquire(std::atomic<size_t>& Value) noexcept;

/// Inherited by mutex, semaphore, and auto_reset_event.
struct waiter_data_base {
  tmc::detail::waiter_list waiters;
  // Low half bits are the auto_reset_event value.
  // High half bits are the number of waiters.
  std::atomic<size_t> value;
};
} // namespace detail

/// Common awaitable type returned by `operator co_await()` of
/// mutex, semaphore, and auto_reset_event.
class aw_acquire {
  tmc::detail::waiter_list_node me;
  std::atomic<tmc::detail::waiter_data_base*> parent;

  friend class auto_reset_event;
  friend class mutex;
  friend class semaphore;

  inline aw_acquire(tmc::detail::waiter_data_base& Parent) noexcept
      : parent(&Parent) {}

public:
  TMC_DECL bool await_ready() noexcept;

  TMC_DECL void await_suspend(std::coroutine_handle<> Outer) noexcept;

  inline void await_resume() noexcept {}

  // Cannot be moved or copied due to holding intrusive list pointer
  aw_acquire(aw_acquire const&) = delete;
  aw_acquire& operator=(aw_acquire const&) = delete;
  aw_acquire(aw_acquire&&) = delete;
  aw_acquire& operator=(aw_acquire&&) = delete;
};
} // namespace tmc

#if !defined(TMC_STANDALONE_COMPILATION) || defined(TMC_IMPL)
#include "tmc/detail/waiter_list.ipp"
#endif
