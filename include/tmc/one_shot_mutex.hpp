// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once
#include "tmc/detail/impl.hpp" // IWYU pragma: keep

#include "tmc/aw_yield.hpp"
#include "tmc/detail/concepts_awaitable.hpp"
#include "tmc/ex_any.hpp"
#include "tmc/task.hpp"

#include <atomic>
#include <coroutine>

namespace tmc {
class one_shot_mutex;

namespace detail {
struct one_shot_mutex_waiter {
  one_shot_mutex_waiter* next;
  std::coroutine_handle<> continuation;
  tmc::ex_any* continuation_executor;
  size_t continuation_priority;
};
} // namespace detail

class [[nodiscard(
  "You must co_await aw_one_shot_mutex_lock for it to have any effect."
)]] aw_one_shot_mutex_lock : tmc::detail::AwaitTagNoGroupAsIs {
  tmc::detail::one_shot_mutex_waiter me;
  std::atomic<one_shot_mutex*> parent;

  friend class one_shot_mutex;

  inline aw_one_shot_mutex_lock(one_shot_mutex& Parent) noexcept : parent(&Parent) {}

public:
  inline bool await_ready() noexcept { return false; }

  TMC_DECL std::coroutine_handle<>
  await_suspend(std::coroutine_handle<> Outer) noexcept;

  inline void await_resume() noexcept {}

  aw_one_shot_mutex_lock(aw_one_shot_mutex_lock const&) = delete;
  aw_one_shot_mutex_lock& operator=(aw_one_shot_mutex_lock const&) = delete;
  aw_one_shot_mutex_lock(aw_one_shot_mutex_lock&&) = delete;
  aw_one_shot_mutex_lock& operator=(aw_one_shot_mutex_lock&&) = delete;
};

/// A serializing primitive similar to `tmc::mutex`, except ownership ends when
/// the acquiring coroutine next suspends. This makes it suitable for protecting
/// short, synchronous stretches of coroutine execution without carrying the
/// lock across later awaits.
class one_shot_mutex {
  friend class aw_one_shot_mutex_lock;

  std::atomic<tmc::detail::one_shot_mutex_waiter*> waiters;
  std::atomic<bool> running;

  TMC_DECL std::coroutine_handle<>
  enqueue(tmc::detail::one_shot_mutex_waiter& Waiter) noexcept;

  TMC_DECL tmc::task<void> run_loop();

public:
  inline one_shot_mutex() noexcept : waiters{nullptr}, running{false} {}

  /// Returns true if a runner is currently executing queued work for the mutex.
  /// This value is not guaranteed to be consistent with any other operation.
  inline bool is_locked() noexcept {
    return running.load(std::memory_order_relaxed);
  }

  /// Suspend this coroutine and resubmit it to its current executor. Since the
  /// lock is owned by the runner rather than the caller, this releases the
  /// mutex before the coroutine resumes later.
  inline aw_yield co_unlock() noexcept { return tmc::yield(); }

  /// Queue this coroutine to run under the mutex until it next suspends.
  inline aw_one_shot_mutex_lock operator co_await() noexcept {
    return aw_one_shot_mutex_lock(*this);
  }

  /// Destruction is only valid once all queued work has completed and no
  /// runner is active.
  TMC_DECL ~one_shot_mutex();
};

namespace detail {
template <> struct awaitable_traits<tmc::one_shot_mutex> {
  static constexpr configure_mode mode = WRAPPER;

  using result_type = void;
  using self_type = tmc::one_shot_mutex;
  using awaiter_type = tmc::aw_one_shot_mutex_lock;

  static awaiter_type get_awaiter(self_type& Awaitable) noexcept {
    return Awaitable.operator co_await();
  }
};
} // namespace detail
} // namespace tmc

#if !defined(TMC_STANDALONE_COMPILATION) || defined(TMC_IMPL)
#include "tmc/detail/one_shot_mutex.ipp"
#endif
