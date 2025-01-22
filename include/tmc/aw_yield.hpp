// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/concepts.hpp"
#include "tmc/detail/thread_locals.hpp"

#include <coroutine>

namespace tmc {
/// Returns true if a higher priority task is requesting to run on this thread.
inline bool yield_requested() {
  // yield if the yield_priority value is smaller (higher priority)
  // than our currently running task
  return tmc::detail::this_thread::this_task.yield_priority->load(
           std::memory_order_relaxed
         ) < tmc::detail::this_thread::this_task.prio;
}

/// The awaitable type returned by `tmc::yield()`.
class [[nodiscard("You must co_await aw_yield for it to have any effect."
)]] aw_yield : tmc::detail::AwaitTagNoGroupAsIs {
public:
  /// This awaitable always suspends outer.
  inline bool await_ready() const noexcept { return false; }

  /// Post the outer task to its current executor, so that a higher priority
  /// task can run.
  TMC_FORCE_INLINE inline void await_suspend(std::coroutine_handle<> Outer
  ) const noexcept {
    tmc::detail::post_checked(
      tmc::detail::this_thread::executor, std::move(Outer),
      tmc::detail::this_thread::this_task.prio
    );
  }

  /// Does nothing.
  inline void await_resume() const noexcept {}
};

/// Returns an awaitable that suspends this task and resubmits it back to its
/// current executor, so that a higher priority task can run.
constexpr aw_yield yield() { return {}; }

/// The awaitable type returned by `tmc::yield_if_requested()`.
class [[nodiscard("You must co_await aw_yield_if_requested for it to have any "
                  "effect.")]] aw_yield_if_requested
    : tmc::detail::AwaitTagNoGroupAsIs {
public:
  /// Suspend only if a higher priority task is requesting to run on this thread
  /// (if `yield_requested()` returns true).
  inline bool await_ready() const noexcept { return !yield_requested(); }

  /// Post the outer task to its current executor, so that a higher priority
  /// task can run.
  TMC_FORCE_INLINE inline void await_suspend(std::coroutine_handle<> Outer
  ) const noexcept {
    tmc::detail::post_checked(
      tmc::detail::this_thread::executor, std::move(Outer),
      tmc::detail::this_thread::this_task.prio
    );
  }

  /// Does nothing.
  inline void await_resume() const noexcept {}
};

/// Returns an awaitable that suspends this task only if a higher priority task
/// is requesting to run on this thread.
///
/// `co_await yield_if_requested();`
///
/// is equivalent to
///
/// `if (yield_requested()) { co_await yield();}`
constexpr aw_yield_if_requested yield_if_requested() { return {}; }

/// The awaitable type returned by `tmc::check_yield_counter_dynamic()`.
class [[nodiscard(
  "You must co_await aw_yield_counter_dynamic for it to have any "
  "effect."
)]] aw_yield_counter_dynamic : tmc::detail::AwaitTagNoGroupAsIs {
  int64_t count;
  int64_t n;

public:
  /// It is recommended to call `check_yield_counter_dynamic()` instead of using
  /// this constructor directly.
  aw_yield_counter_dynamic(int64_t N) : count(0), n(N) {}

  /// Every `N` calls to `co_await`, this will check yield_requested() and
  /// suspend if that returns true.
  inline bool await_ready() noexcept {
    ++count;
    if (count < n) {
      return true;
    } else {
      count = 0;
      return !yield_requested();
    }
  }

  /// Post the outer task to its current executor, so that a higher priority
  /// task can run.
  TMC_FORCE_INLINE inline void await_suspend(std::coroutine_handle<> Outer
  ) const noexcept {
    tmc::detail::post_checked(
      tmc::detail::this_thread::executor, std::move(Outer),
      tmc::detail::this_thread::this_task.prio
    );
  }

  /// Does nothing.
  inline void await_resume() const noexcept {}

  /// Resets the internal counter value to 0. This might be useful if you
  /// yielded for another reason.
  inline void reset() { count = 0; }
};

/// Returns an awaitable that, every `N` calls to `co_await`, checks
/// `yield_requested()` and yields if that returns true. The counterpart
/// function `check_yield_counter()` allows setting `N` as a template parameter.
inline aw_yield_counter_dynamic check_yield_counter_dynamic(size_t N) {
  return aw_yield_counter_dynamic(static_cast<int64_t>(N));
}

/// The awaitable type returned by `tmc::check_yield_counter()`.
template <int64_t N>
class [[nodiscard("You must co_await aw_yield_counter for it to have any "
                  "effect.")]] aw_yield_counter
    : tmc::detail::AwaitTagNoGroupAsIs {
  int64_t count;

public:
  /// It is recommended to call `check_yield_counter()` instead of using
  /// this constructor directly.
  aw_yield_counter() : count(0) {}

  /// Every `N` calls to `co_await`, this will check yield_requested() and
  /// suspend if that returns true.
  inline bool await_ready() noexcept {
    ++count;
    if (count < N) {
      return true;
    } else {
      count = 0;
      return !yield_requested();
    }
  }

  /// Post the outer task to its current executor, so that a higher priority
  /// task can run.
  TMC_FORCE_INLINE inline void await_suspend(std::coroutine_handle<> Outer
  ) const noexcept {
    tmc::detail::post_checked(
      tmc::detail::this_thread::executor, std::move(Outer),
      tmc::detail::this_thread::this_task.prio
    );
  }

  /// Does nothing.
  inline void await_resume() const noexcept {}

  /// Resets the internal counter value to 0. This might be useful if you
  /// yielded for another reason.
  inline void reset() { count = 0; }
};

/// Returns an awaitable that, every `N` calls to `co_await`, checks
/// `yield_requested()` and yields if that returns true.
/// The counterpart function `check_yield_counter_dynamic()` allows passing `N`
/// as a runtime parameter.
template <int64_t N> inline aw_yield_counter<N> check_yield_counter() {
  return aw_yield_counter<N>();
}

/// Returns the current task's priority.
inline size_t current_priority() {
  return tmc::detail::this_thread::this_task.prio;
}
} // namespace tmc
