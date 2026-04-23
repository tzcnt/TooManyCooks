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
#include <cstdint>

namespace tmc {
class one_shot_mutex;

namespace detail {
struct one_shot_mutex_waiter {
  one_shot_mutex_waiter* next;
  std::coroutine_handle<> continuation;
  tmc::ex_any* continuation_executor;
  size_t continuation_priority;
};

struct one_shot_mutex_state {
  static inline constexpr uintptr_t RUNNING = 0x1;

  std::atomic<uintptr_t> waiters;
  std::atomic<size_t> refs;

  inline one_shot_mutex_state() noexcept : waiters{0}, refs{1} {}
};

// Ensure that it's safe to use pointer tagging to synchronize.
static_assert(alignof(one_shot_mutex_waiter) >= 2);

[[nodiscard]] inline bool one_shot_mutex_running(uintptr_t Value) noexcept {
  return 0 != (Value & one_shot_mutex_state::RUNNING);
}

} // namespace detail

class [[nodiscard(
  "You must co_await aw_one_shot_mutex_lock for it to have any effect."
)]] aw_one_shot_mutex_lock : tmc::detail::AwaitTagNoGroupAsIs {
  tmc::detail::one_shot_mutex_waiter me;
  std::atomic<one_shot_mutex*> parent;

  friend class one_shot_mutex;

  inline aw_one_shot_mutex_lock(one_shot_mutex& Parent) noexcept
      : parent(&Parent) {}

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

template <typename Result>
class [[nodiscard(
  "You must co_await aw_one_shot_mutex_return_value_unlock for it to have any "
  "effect."
)]] aw_one_shot_mutex_return_value_unlock : tmc::detail::AwaitTagNoGroupAsIs {
  tmc::detail::one_shot_mutex_waiter me;
  std::atomic<one_shot_mutex*> parent;
  Result&& result;

  friend class one_shot_mutex;

  template <typename ResultArg>
  inline aw_one_shot_mutex_return_value_unlock(
    one_shot_mutex& Parent, ResultArg&& ResultIn
  ) noexcept
      : parent(&Parent), result(static_cast<ResultArg&&>(ResultIn)) {}

public:
  inline bool await_ready() noexcept { return false; }

  template <typename P>
  void await_suspend(std::coroutine_handle<P> Outer) noexcept {
    Outer.promise().return_value(static_cast<Result&&>(result));
    Outer.promise().customizer.post_continuation(Outer);
  }

  // Never runs; customizer.post_continuation destroys this.
  [[maybe_unused]] inline void await_resume() noexcept {}

  aw_one_shot_mutex_return_value_unlock(
    aw_one_shot_mutex_return_value_unlock const&
  ) = delete;
  aw_one_shot_mutex_return_value_unlock&
  operator=(aw_one_shot_mutex_return_value_unlock const&) = delete;
  aw_one_shot_mutex_return_value_unlock(
    aw_one_shot_mutex_return_value_unlock&&
  ) = delete;
  aw_one_shot_mutex_return_value_unlock&
  operator=(aw_one_shot_mutex_return_value_unlock&&) = delete;
};

class [[nodiscard(
  "You must co_await aw_one_shot_mutex_return_void_unlock for it to have any "
  "effect."
)]] aw_one_shot_mutex_return_void_unlock : tmc::detail::AwaitTagNoGroupAsIs {
  tmc::detail::one_shot_mutex_waiter me;
  std::atomic<one_shot_mutex*> parent;

  friend class one_shot_mutex;

  inline aw_one_shot_mutex_return_void_unlock(one_shot_mutex& Parent) noexcept
      : parent(&Parent) {}

public:
  inline bool await_ready() noexcept { return false; }

  template <typename P>
  void await_suspend(std::coroutine_handle<P> Outer) noexcept {
    Outer.promise().customizer.post_continuation(Outer);
  }

  // Never runs; customizer.post_continuation destroys this.
  [[maybe_unused]] inline void await_resume() noexcept {}

  aw_one_shot_mutex_return_void_unlock(
    aw_one_shot_mutex_return_void_unlock const&
  ) = delete;
  aw_one_shot_mutex_return_void_unlock&
  operator=(aw_one_shot_mutex_return_void_unlock const&) = delete;
  aw_one_shot_mutex_return_void_unlock(aw_one_shot_mutex_return_void_unlock&&) =
    delete;
  aw_one_shot_mutex_return_void_unlock&
  operator=(aw_one_shot_mutex_return_void_unlock&&) = delete;
};

/// A serializing primitive similar to `tmc::mutex`, except ownership ends when
/// the acquiring coroutine next suspends. This makes it suitable for protecting
/// short, synchronous stretches of coroutine execution without carrying the
/// lock across later awaits.
class one_shot_mutex {
  friend class aw_one_shot_mutex_lock;

  tmc::detail::one_shot_mutex_state* state;

  TMC_DECL std::coroutine_handle<>
  enqueue(tmc::detail::one_shot_mutex_waiter& Waiter) noexcept;

  TMC_DECL static tmc::task<void>
  run_loop(tmc::detail::one_shot_mutex_state* State);

public:
  inline one_shot_mutex() noexcept
      : state(new tmc::detail::one_shot_mutex_state) {}

  /// Returns true if a runner is currently executing queued work for the mutex.
  /// This value is not guaranteed to be consistent with any other operation.
  inline bool is_locked() noexcept {
    return tmc::detail::one_shot_mutex_running(
      state->waiters.load(std::memory_order_relaxed)
    );
  }

  /// Suspend this coroutine and resubmit it to its current executor. Since the
  /// lock is owned by the runner rather than the caller, this releases the
  /// mutex before the coroutine resumes later.
  inline aw_yield co_unlock() noexcept { return tmc::yield(); }

  /// Completes this coroutine immediately at the unlock suspension point,
  /// setting the result on the parent coroutine and posting its continuation
  /// directly. Unlike spelling this as `co_await mut.co_unlock(); co_return
  /// result;`, locals in this coroutine are destroyed immediately rather than
  /// after a later resume.
  template <typename Result>
  inline aw_one_shot_mutex_return_value_unlock<Result>
  co_unlock_return_value(Result&& result) noexcept {
    return aw_one_shot_mutex_return_value_unlock<Result>(
      *this, static_cast<Result&&>(result)
    );
  }

  /// Completes this coroutine immediately at the unlock suspension point and
  /// posts the parent continuation directly. Unlike spelling this as
  /// `co_await mut.co_unlock(); co_return;`, locals in this coroutine are
  /// destroyed immediately rather than after a later resume.
  inline aw_one_shot_mutex_return_void_unlock co_unlock_return_void() noexcept {
    return aw_one_shot_mutex_return_void_unlock(*this);
  }

  /// Queue this coroutine to run under the mutex until it next suspends.
  inline aw_one_shot_mutex_lock operator co_await() noexcept {
    return aw_one_shot_mutex_lock(*this);
  }

  /// Destruction is only valid once all queued work has completed and no
  /// runner is active.
  TMC_DECL ~one_shot_mutex();

private:
  one_shot_mutex(one_shot_mutex const& Other) = delete;
  one_shot_mutex& operator=(one_shot_mutex const& Other) = delete;
  one_shot_mutex(one_shot_mutex&& Other) = delete;
  one_shot_mutex& operator=(one_shot_mutex&& Other) = delete;
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
