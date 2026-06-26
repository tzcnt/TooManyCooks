// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once
#include "tmc/detail/impl.hpp" // IWYU pragma: keep

#include "tmc/detail/awaitable_customizer.hpp"
#include "tmc/detail/concepts_awaitable.hpp"
#include "tmc/detail/waiter_list.hpp"

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <type_traits>

namespace tmc::tests {
class waiter_count_accessor;
}

namespace tmc {
class semaphore;

/// The semaphore will be released when this goes out of scope.
class [[nodiscard("The semaphore will be released when this goes out of scope.")]]
semaphore_scope {
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
  TMC_DECL ~semaphore_scope();
};

/// Same as aw_acquire but returns a nodiscard semaphore_scope that releases
/// the semaphore on destruction.
class [[nodiscard(
  "You must co_await aw_semaphore_acquire_scope for it to have any effect."
)]] aw_semaphore_acquire_scope : tmc::detail::AwaitTagNoGroupAsIs {
  tmc::detail::waiter_list_node me;
  std::atomic<semaphore*> parent;

  friend class semaphore;

  inline aw_semaphore_acquire_scope(semaphore& Parent) noexcept : parent(&Parent) {}

public:
  TMC_DECL bool await_ready() noexcept;

  TMC_DECL void await_suspend(std::coroutine_handle<> Outer) noexcept;

  inline semaphore_scope await_resume() noexcept {
    return semaphore_scope(parent.load(std::memory_order_relaxed));
  }

  // Cannot be moved or copied due to holding intrusive list pointer
  aw_semaphore_acquire_scope(aw_semaphore_acquire_scope const&) = delete;
  aw_semaphore_acquire_scope& operator=(aw_semaphore_acquire_scope const&) = delete;
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

  TMC_DECL std::coroutine_handle<> await_suspend(std::coroutine_handle<> Outer) noexcept;

  inline void await_resume() noexcept {}

  // Copy/move constructors *could* be implemented, but why?
  aw_semaphore_co_release(aw_semaphore_co_release const&) = delete;
  aw_semaphore_co_release& operator=(aw_semaphore_co_release const&) = delete;
  aw_semaphore_co_release(aw_semaphore_co_release&&) = delete;
  aw_semaphore_co_release& operator=(aw_semaphore_co_release&&) = delete;
};

template <typename Result>
class [[nodiscard(
  "You must co_await aw_semaphore_co_release_return for it to have any effect."
)]] aw_semaphore_co_release_return : tmc::detail::AwaitTagNoGroupAsIs {
  semaphore& parent;

  // Store lvalues by reference. Move rvalues into this.
  using ReturnValueStorage = std::conditional_t<
    std::is_lvalue_reference_v<Result>, Result, std::remove_cvref_t<Result>>;

  // Handle value return and void return.
  struct empty {};
  using ResultStorage =
    std::conditional_t<std::is_void_v<Result>, empty, ReturnValueStorage>;
  TMC_NO_UNIQUE_ADDRESS ResultStorage result;

  friend class semaphore;

  // For result return
  template <typename ResultArg>
  inline aw_semaphore_co_release_return(semaphore& Parent, ResultArg&& ResultIn) noexcept
      : parent(Parent), result(static_cast<ResultArg&&>(ResultIn)) {}

  // For void return
  inline aw_semaphore_co_release_return(semaphore& Parent) noexcept : parent(Parent) {}

public:
  inline bool await_ready() noexcept { return false; }

  template <typename Promise>
  std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> Outer) noexcept;

  [[maybe_unused]] inline void await_resume() noexcept {}

  aw_semaphore_co_release_return(aw_semaphore_co_release_return const&) = delete;
  aw_semaphore_co_release_return&
  operator=(aw_semaphore_co_release_return const&) = delete;
  aw_semaphore_co_release_return(aw_semaphore_co_release_return&&) = delete;
  aw_semaphore_co_release_return& operator=(aw_semaphore_co_release_return&&) = delete;
};

/// An async version of std::counting_semaphore.
class semaphore : protected tmc::detail::waiter_data_base {
  friend class aw_acquire;
  friend class aw_semaphore_acquire_scope;
  friend class aw_semaphore_co_release;
  template <typename Result> friend class aw_semaphore_co_release_return;
  friend class ::tmc::tests::waiter_count_accessor;

  // Returns the number of awaiters currently registered (suspended and
  // waiting to acquire) on this semaphore. For testing purposes. Thread-safe.
  inline size_t waiter_count() noexcept {
    return (value.load(std::memory_order_acquire) >> tmc::detail::WAITERS_OFFSET) &
           tmc::detail::HALF_MASK;
  }

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
  TMC_DECL void release(size_t ReleaseCount = 1) noexcept;

  /// Increases the available resources by 1. If there are awaiters,
  /// 1 will be awoken and the resource will be transferred to it.
  /// The awaiter may be resumed by symmetric transfer if it is eligible
  /// (it resumes on the same executor and priority as the caller).
  inline aw_semaphore_co_release co_release() noexcept {
    return aw_semaphore_co_release(*this);
  }

  /// Increases the available resources by 1. If there are awaiters,
  /// 1 will be awoken and the resource will be transferred to it. Also
  /// completes this coroutine immediately, returns the result back to its
  /// parent coroutine, and resumes the parent coroutine. Both the resuming
  /// awaiter and the parent coroutine will be checked for symmetric transfer
  /// eligibility; otherwise they will be posted back to their respective
  /// executors.
  ///
  /// This effectively contains a `co_return` statement, ending the current
  /// coroutine; nothing will be executed after it in the current scope.
  ///
  /// The purpose of this is to skip a round-trip through the executor when
  /// you want to release this semaphore immediately before returning.
  ///
  /// ```
  /// // You can replace this:
  /// co_await sem.co_release();
  /// co_return result;
  ///
  /// // With this:
  /// co_await sem.co_release_return(result);
  /// std::unreachable();
  /// ```
  template <typename Result>
  inline aw_semaphore_co_release_return<Result>
  co_release_return(Result&& result) noexcept {
    return aw_semaphore_co_release_return<Result>(*this, static_cast<Result&&>(result));
  }

  /// Increases the available resources by 1. If there are awaiters,
  /// 1 will be awoken and the resource will be transferred to it. Also
  /// completes this coroutine immediately and resumes the parent coroutine.
  /// Both the resuming awaiter and the parent coroutine will be checked for
  /// symmetric transfer eligibility; otherwise they will be posted back to
  /// their respective executors.
  ///
  /// This effectively contains a `co_return` statement, ending the current
  /// coroutine; nothing will be executed after it in the current scope.
  ///
  /// The purpose of this is to skip a round-trip through the executor when
  /// you want to release this semaphore immediately before returning.
  ///
  /// ```
  /// // You can replace this:
  /// co_await sem.co_release();
  /// co_return;
  ///
  /// // With this:
  /// co_await sem.co_release_return();
  /// std::unreachable();
  /// ```
  inline aw_semaphore_co_release_return<void> co_release_return() noexcept {
    return aw_semaphore_co_release_return<void>(*this);
  }

  /// Tries to acquire 1 resource from the semaphore without suspending.
  /// Returns true on success, or false if no resources are ready. Not
  /// re-entrant.
  inline bool try_acquire() noexcept { return tmc::detail::try_acquire(value); }

  /// Tries to acquire 1 resource from the semaphore. If no resources are ready,
  /// will suspend until a resource becomes ready.
  inline aw_acquire operator co_await() noexcept { return aw_acquire(*this); }

  /// Tries to acquire 1 resource from the semaphore. If no resources are ready,
  /// will suspend until a resource becomes ready, then transfer the ownership
  /// to this task. Returns an object that will release the resource (and resume
  /// an awaiter) when it goes out of scope.
  inline aw_semaphore_acquire_scope acquire_scope() noexcept {
    return aw_semaphore_acquire_scope(*this);
  }

  /// On destruction, any awaiters will be resumed.
  TMC_DECL ~semaphore();
};

template <typename Result>
template <typename Promise>
std::coroutine_handle<> aw_semaphore_co_release_return<Result>::await_suspend(
  std::coroutine_handle<Promise> Outer
) noexcept {
  if constexpr (std::is_void_v<Result>) {
    Outer.promise().return_void();
  } else {
    Outer.promise().return_value(static_cast<Result&&>(result));
  }

  // Release the semaphore normally and capture the continuation
  size_t old = parent.value.fetch_add(1, std::memory_order_acq_rel);
  size_t v = 1 + old;
  auto toWake = parent.waiters.maybe_wake(parent.value, v, old, true);

  // Capture these values locally before destroying the coroutine frame
  auto& customizer = Outer.promise().customizer;
  void* continuationExecutor = customizer.continuation_executor;
  void* continuationPtr = customizer.continuation;
  void* doneCount = customizer.done_count;
  size_t flags = customizer.flags;

  // Destroy the coroutine *before* calling get_continuation, which could allow
  // the continuation to be stolen by another parent, which could then complete
  // and destroy the frame of this coroutine if it is HALO'd into that parent.
  // By destroying ourselves first, we avoid use-after-free. This is the same
  // protocol that tmc::task's final_suspend follows.
  Outer.destroy();

  std::coroutine_handle<> continuation =
    tmc::detail::awaitable_customizer_base::get_continuation(
      continuationExecutor, continuationPtr, doneCount, flags
    );

  size_t continuationPriority = flags & tmc::detail::task_flags::PRIORITY_MASK;
  return tmc::detail::try_symmetric_transfer2_waiter(
    toWake, continuation, static_cast<tmc::ex_any*>(continuationExecutor),
    continuationPriority
  );
}

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

#if !defined(TMC_STANDALONE_COMPILATION) || defined(TMC_IMPL)
#include "tmc/detail/semaphore.ipp"
#endif
