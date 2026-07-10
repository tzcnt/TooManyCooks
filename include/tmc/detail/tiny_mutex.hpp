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
#include <type_traits>

namespace tmc {
class tiny_mutex;

namespace detail {
struct tiny_mutex_waiter {
  tiny_mutex_waiter* next;
  std::coroutine_handle<> continuation;
  tmc::ex_any* continuation_executor;
  size_t continuation_priority;
};

struct tiny_mutex_state {
  static inline constexpr uintptr_t RUNNING = 0x1;

  std::atomic<uintptr_t> waiters;
  std::atomic<size_t> refs;

  inline tiny_mutex_state() noexcept : waiters{0}, refs{1} {}
};

// Ensure that it's safe to use pointer tagging to synchronize.
static_assert(alignof(tiny_mutex_waiter) >= 2);

[[nodiscard]] inline bool tiny_mutex_running(uintptr_t Value) noexcept {
  return 0 != (Value & tiny_mutex_state::RUNNING);
}

template <typename Result>
using tiny_mutex_return_storage_t = std::conditional_t<
  std::is_lvalue_reference_v<Result>, Result, std::remove_cvref_t<Result>>;

} // namespace detail

class [[nodiscard(
  "You must co_await aw_tiny_mutex_lock for it to have any effect."
)]] aw_tiny_mutex_lock : tmc::detail::AwaitTagNoGroupAsIs {
  tmc::detail::tiny_mutex_waiter me;
  std::atomic<tiny_mutex*> parent;

  friend class tiny_mutex;

  inline aw_tiny_mutex_lock(tiny_mutex& Parent) noexcept : parent(&Parent) {}

public:
  inline bool await_ready() noexcept { return false; }

  TMC_DECL std::coroutine_handle<>
  await_suspend(std::coroutine_handle<> Outer) noexcept;

  inline void await_resume() noexcept {}

  aw_tiny_mutex_lock(aw_tiny_mutex_lock const&) = delete;
  aw_tiny_mutex_lock& operator=(aw_tiny_mutex_lock const&) = delete;
  aw_tiny_mutex_lock(aw_tiny_mutex_lock&&) = delete;
  aw_tiny_mutex_lock& operator=(aw_tiny_mutex_lock&&) = delete;
};

template <typename Result>
class [[nodiscard(
  "You must co_await aw_tiny_mutex_co_unlock_return for it to have any "
  "effect."
)]] aw_tiny_mutex_co_unlock_return : tmc::detail::AwaitTagNoGroupAsIs {

  // Store lvalues by reference. Move rvalues into this.
  using ReturnValueStorage = std::conditional_t<
    std::is_lvalue_reference_v<Result>, Result, std::remove_cvref_t<Result>>;

  // Handle value return and void return.
  struct empty {};
  using ResultStorage =
    std::conditional_t<std::is_void_v<Result>, empty, ReturnValueStorage>;
  TMC_NO_UNIQUE_ADDRESS ResultStorage result;

  friend class tiny_mutex;

  template <typename ResultArg>
  inline explicit aw_tiny_mutex_co_unlock_return(ResultArg&& ResultIn) noexcept
      : result(static_cast<ResultArg&&>(ResultIn)) {}

  inline aw_tiny_mutex_co_unlock_return() noexcept {}

public:
  inline bool await_ready() noexcept { return false; }

  template <typename P>
  void await_suspend(std::coroutine_handle<P> Outer) noexcept {
    if constexpr (std::is_void_v<Result>) {
      Outer.promise().return_void();
    } else {
      Outer.promise().return_value(static_cast<Result&&>(result));
    }
    Outer.promise().customizer.post_continuation(Outer);
  }

  // Never runs; post_continuation destroys this coroutine.
  [[maybe_unused]] inline void await_resume() noexcept {}

  aw_tiny_mutex_co_unlock_return(aw_tiny_mutex_co_unlock_return const&) =
    delete;
  aw_tiny_mutex_co_unlock_return&
  operator=(aw_tiny_mutex_co_unlock_return const&) = delete;
  aw_tiny_mutex_co_unlock_return(aw_tiny_mutex_co_unlock_return&&) = delete;
  aw_tiny_mutex_co_unlock_return&
  operator=(aw_tiny_mutex_co_unlock_return&&) = delete;
};

/// A serializing primitive similar to `tmc::mutex`, except ownership ends when
/// the acquiring coroutine next suspends. This makes it suitable for protecting
/// short, synchronous stretches of coroutine execution without carrying the
/// lock across later awaits.
class tiny_mutex {
  friend class aw_tiny_mutex_lock;

  tmc::detail::tiny_mutex_state* state;

  TMC_DECL std::coroutine_handle<>
  enqueue(tmc::detail::tiny_mutex_waiter& Waiter) noexcept;

  static TMC_DECL tmc::task<void>
  run_loop(tmc::detail::tiny_mutex_state* State);

public:
  inline tiny_mutex() noexcept : state(new tmc::detail::tiny_mutex_state) {}

  /// Returns true if a runner is currently executing queued work for the mutex.
  /// This value is not guaranteed to be consistent with any other operation.
  inline bool is_locked() noexcept {
    return tmc::detail::tiny_mutex_running(
      state->waiters.load(std::memory_order_relaxed)
    );
  }

  /// Suspend this coroutine and resubmit it to its current executor. Since the
  /// lock is owned by the runner rather than the caller, this releases the
  /// mutex before the coroutine resumes later.
  inline aw_yield co_unlock() noexcept { return tmc::yield(); }

  /// Completes this coroutine immediately, returns value to its parent
  /// coroutine, and resubmits the parent to its current executor. Since the
  /// lock is owned by the runner rather than the caller, this releases the
  /// mutex before the coroutine resumes later.
  ///
  /// The purpose of this is to skip a round-trip through the executor when
  /// you want to unlock this mutex immediately before returning.
  ///
  /// ```
  /// // You can replace this:
  /// co_await mut.co_unlock();
  /// co_return result;
  ///
  /// // With this:
  /// co_await mut.co_unlock_return(result);
  /// TMC_UNREACHABLE;
  /// ```
  template <typename Result>
  inline aw_tiny_mutex_co_unlock_return<Result>
  co_unlock_return(Result&& result) noexcept {
    return aw_tiny_mutex_co_unlock_return<Result>(
      static_cast<Result&&>(result)
    );
  }

  /// Completes this coroutine immediately and resubmits the parent to its
  /// current executor. Since the lock is owned by the runner rather than the
  /// caller, this releases the mutex before the coroutine resumes later.
  ///
  /// The purpose of this is to skip a round-trip through the executor when
  /// you want to unlock this mutex immediately before returning.
  ///
  /// ```
  /// // You can replace this:
  /// co_await mut.co_unlock();
  /// co_return;
  ///
  /// // With this:
  /// co_await mut.co_unlock_return();
  /// TMC_UNREACHABLE;
  /// ```
  inline aw_tiny_mutex_co_unlock_return<void> co_unlock_return() noexcept {
    return aw_tiny_mutex_co_unlock_return<void>();
  }

  /// Queue this coroutine to run under the mutex until it next suspends.
  inline aw_tiny_mutex_lock operator co_await() noexcept {
    return aw_tiny_mutex_lock(*this);
  }

  /// Destruction is only valid once all queued work has completed and no
  /// runner is active.
  TMC_DECL ~tiny_mutex();

private:
  tiny_mutex(tiny_mutex const& Other) = delete;
  tiny_mutex& operator=(tiny_mutex const& Other) = delete;
  tiny_mutex(tiny_mutex&& Other) = delete;
  tiny_mutex& operator=(tiny_mutex&& Other) = delete;
};

namespace detail {
template <> struct awaitable_traits<tmc::tiny_mutex> {
  static constexpr configure_mode mode = WRAPPER;

  using result_type = void;
  using self_type = tmc::tiny_mutex;
  using awaiter_type = tmc::aw_tiny_mutex_lock;

  static awaiter_type get_awaiter(self_type& Awaitable) noexcept {
    return Awaitable.operator co_await();
  }
};
} // namespace detail
} // namespace tmc

#if !defined(TMC_STANDALONE_COMPILATION) || defined(TMC_IMPL)
#include "tmc/detail/tiny_mutex.ipp"
#endif
