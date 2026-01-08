// Copyright (c) 2023-2026 Logan McDougall
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
class semaphore;

/// The semaphore will be released when this goes out of scope.
class [[nodiscard(
  "The semaphore will be released when this goes out of scope."
)]] semaphore_scope {
  semaphore* parent;

  friend class aw_semaphore_acquire_scope;

  inline semaphore_scope(semaphore* Parent) noexcept : parent(Parent) {}

public:
  // Movable but not copyable
  semaphore_scope(semaphore_scope const&) = delete;
  semaphore_scope& operator=(semaphore_scope const&) = delete;
  inline semaphore_scope(semaphore_scope&& Other) noexcept {
    parent = Other.parent;
    Other.parent = nullptr;
  }
  semaphore_scope& operator=(semaphore_scope&& Other) = delete;

  /// Releases the semaphore on destruction. Does not symmetric transfer.
  ~semaphore_scope();
};

/// Same as aw_acquire but returns a nodiscard semaphore_scope that releases
/// the semaphore on destruction.
class [[nodiscard(
  "You must co_await aw_semaphore_acquire_scope for it to have any effect."
)]] aw_semaphore_acquire_scope : tmc::detail::AwaitTagNoGroupAsIs {
  tmc::detail::waiter_list_node me;
  semaphore& parent;

  friend class semaphore;

  inline aw_semaphore_acquire_scope(semaphore& Parent) noexcept
      : parent(Parent) {}

public:
  bool await_ready() noexcept;

  void await_suspend(std::coroutine_handle<> Outer) noexcept;

  inline semaphore_scope await_resume() noexcept {
    return semaphore_scope(&parent);
  }

  // Cannot be moved or copied due to holding intrusive list pointer
  aw_semaphore_acquire_scope(aw_semaphore_acquire_scope const&) = delete;
  aw_semaphore_acquire_scope&
  operator=(aw_semaphore_acquire_scope const&) = delete;
  aw_semaphore_acquire_scope(aw_semaphore_acquire_scope&&) = delete;
  aw_semaphore_acquire_scope& operator=(aw_semaphore_acquire_scope&&) = delete;
};

class [[nodiscard(
  "You must co_await aw_semaphore_co_release for it to have any effect."
)]] aw_semaphore_co_release : tmc::detail::AwaitTagNoGroupAsIs {
  semaphore& parent;

  friend class semaphore;

  inline aw_semaphore_co_release(semaphore& Parent) noexcept : parent(Parent) {}

public:
  inline bool await_ready() noexcept { return false; }

  std::coroutine_handle<> await_suspend(std::coroutine_handle<> Outer) noexcept;

  inline void await_resume() noexcept {}

  // Copy/move constructors *could* be implemented, but why?
  aw_semaphore_co_release(aw_semaphore_co_release const&) = delete;
  aw_semaphore_co_release& operator=(aw_semaphore_co_release const&) = delete;
  aw_semaphore_co_release(aw_semaphore_co_release&&) = delete;
  aw_semaphore_co_release& operator=(aw_semaphore_co_release&&) = delete;
};

/// An async version of std::counting_semaphore.
class semaphore : protected tmc::detail::waiter_data_base {
  friend class aw_acquire;
  friend class aw_semaphore_acquire_scope;
  friend class aw_semaphore_co_release;

public:
  /// The count is packed into half a machine word along with the awaiter count.
  /// Thus it is only allowed half a machine word of bits.
  using half_word = tmc::detail::half_word;

  /// Sets the initial number of resources available in the semaphore.
  /// This is only an initial value and not a maximum; by calling release(), the
  /// number of resources available may exceed this initial value.
  inline semaphore(half_word InitialCount) noexcept {
    value = static_cast<size_t>(InitialCount);
  }

  /// Returns an estimate of the current number of available resources.
  /// This value is not guaranteed to be consistent with any other operation.
  /// Even if this returns non-zero, awaiting afterward may suspend.
  /// Even if this returns zero, awaiting afterward may not suspend.
  inline size_t count() noexcept {
    return tmc::detail::HALF_MASK & value.load(std::memory_order_relaxed);
  }

  /// Increases the available resources by ReleaseCount. If there are awaiters,
  /// they will be awoken until all resources have been consumed.
  /// Does not symmetric transfer; awaiters will be posted to their executors.
  void release(size_t ReleaseCount = 1) noexcept;

  /// Increases the available resources by 1. If there are awaiters,
  /// 1 will be awoken and the resource will be transferred to it.
  /// The awaiter may be resumed by symmetric transfer if it is eligible
  /// (it resumes on the same executor and priority as the caller).
  inline aw_semaphore_co_release co_release() noexcept {
    return aw_semaphore_co_release(*this);
  }

  /// Tries to acquire the semaphore. If no resources are ready, will
  /// suspend until a resource becomes ready.
  inline aw_acquire operator co_await() noexcept { return aw_acquire(*this); }

  /// Tries to acquire the semaphore. If no resources are ready, will
  /// suspend until a resource becomes ready, then transfer the
  /// ownership to this task.
  /// Returns an object that will release the resource (and resume an awaiter)
  /// when it goes out of scope.
  inline aw_semaphore_acquire_scope acquire_scope() noexcept {
    return aw_semaphore_acquire_scope(*this);
  }

  /// On destruction, any awaiters will be resumed.
  ~semaphore();
};

namespace detail {
template <> struct awaitable_traits<tmc::semaphore> {
  static constexpr configure_mode mode = WRAPPER;

  using result_type = void;
  using self_type = tmc::semaphore;
  using awaiter_type = tmc::aw_acquire;

  static awaiter_type get_awaiter(self_type& Awaitable) noexcept {
    return Awaitable.operator co_await();
  }
};
} // namespace detail
} // namespace tmc

#ifdef TMC_IMPL
#include "tmc/detail/semaphore.ipp"
#endif
