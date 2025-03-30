// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/compat.hpp"
#include "tmc/detail/concepts.hpp" // IWYU pragma: keep
#include "tmc/detail/thread_locals.hpp"

#include <coroutine>

namespace tmc {
/// The awaitable type returned by `tmc::resume_on()`.
class [[nodiscard("You must co_await aw_resume_on for it to have any "
                  "effect.")]] aw_resume_on : tmc::detail::AwaitTagNoGroupAsIs {
  tmc::detail::ex_any* executor;
  size_t prio;

public:
  /// It is recommended to call `resume_on()` instead of using this constructor
  /// directly.
  aw_resume_on(tmc::detail::ex_any* Executor)
      : executor(Executor), prio(tmc::detail::this_thread::this_task.prio) {}

  /// Resume immediately if outer is already running on the requested executor,
  /// at the requested priority.
  inline bool await_ready() const noexcept {
    return tmc::detail::this_thread::exec_is(executor) &&
           tmc::detail::this_thread::prio_is(prio);
  }

  /// Post the outer task to the requested executor.
  TMC_FORCE_INLINE inline void await_suspend(std::coroutine_handle<> Outer
  ) const noexcept {
    // executor check not needed, this function is explicit
    executor->post(std::move(Outer), prio);
  }

  /// Does nothing.
  inline void await_resume() const noexcept {}

  /// When awaited, the outer coroutine will be resumed with the provided
  /// priority.
  [[nodiscard("You must co_await aw_resume_on for it to have any "
              "effect.")]] inline aw_resume_on&
  with_priority(size_t Priority) {
    // For this to work correctly, we must change the priority of the executor
    // thread by posting the task to the executor with the new priority.
    // Directly changing tmc::detail::this_thread::this_task.prio is
    // insufficient, as it doesn't update the task_stopper_bitsets.
    prio = Priority;
    return *this;
  }
};

/// Returns an awaitable that moves this task onto the requested executor. If
/// this task is already running on the requested executor, the co_await will do
/// nothing.
inline aw_resume_on resume_on(tmc::detail::ex_any* Executor) {
  return aw_resume_on(Executor);
}

/// Returns an awaitable that moves this task onto the requested executor. If
/// this task is already running on the requested executor, the co_await will do
/// nothing.
template <typename Exec> inline aw_resume_on resume_on(Exec& Executor) {
  return resume_on(tmc::detail::executor_traits<Exec>::type_erased(Executor));
}

/// Returns an awaitable that moves this task onto the requested executor. If
/// this task is already running on the requested executor, the co_await will do
/// nothing.
template <typename Exec> inline aw_resume_on resume_on(Exec* Executor) {
  return resume_on(tmc::detail::executor_traits<Exec>::type_erased(*Executor));
}

/// Equivalent to `resume_on(tmc::current_executor()).with_priority(Priority);`
inline aw_resume_on change_priority(size_t Priority) {
  return resume_on(tmc::detail::this_thread::executor).with_priority(Priority);
}

template <typename E> class aw_ex_scope_exit;
template <typename E> class aw_ex_scope_enter;
template <typename E> inline aw_ex_scope_enter<E> enter(E& Executor);
template <typename E> inline aw_ex_scope_enter<E> enter(E* Executor);

/// The awaitable type returned by `co_await tmc::enter()`.
/// Call `co_await this.exit()` to exit the executor scope.
template <typename E>
class aw_ex_scope_exit : tmc::detail::AwaitTagNoGroupAsIs {
  friend class aw_ex_scope_enter<E>;
  tmc::detail::ex_any* continuation_executor;
  size_t prio;

  aw_ex_scope_exit(tmc::detail::ex_any* Executor, size_t Priority)
      : continuation_executor(Executor), prio(Priority) {}

public:
  /// Returns an awaitable that can be co_await'ed to exit the
  /// executor scope. This is idempotent.
  /// (Not strictly necessary - you can just `co_await std::move(*this);`
  /// directly - but makes code a bit easier to understand.)
  [[nodiscard("You must co_await exit() for it to have any "
              "effect.")]] inline aw_ex_scope_exit&&
  exit() {
    return std::move(*this);
  }

  /// Always suspends.
  inline bool await_ready() { return false; }

  /// Post this task to the continuation executor.
  TMC_FORCE_INLINE inline void await_suspend(std::coroutine_handle<> Outer) {
    tmc::detail::post_checked(continuation_executor, std::move(Outer), prio);
  }

  /// Restores the original priority.
  /// Only necessary in case of resuming onto an executor where post()
  /// doesn't respect priority, such as ex_asio.
  inline void await_resume() {
    tmc::detail::this_thread::this_task.prio = prio;
  }

  /// When awaited, the outer coroutine will be resumed on the provided
  /// executor.
  inline aw_ex_scope_exit& resume_on(tmc::detail::ex_any* Executor) {
    continuation_executor = Executor;
    return *this;
  }

  /// When awaited, the outer coroutine will be resumed on the provided
  /// executor.
  template <typename Exec> aw_ex_scope_exit& resume_on(Exec& Executor) {
    return resume_on(tmc::detail::executor_traits<Exec>::type_erased(Executor));
  }

  /// When awaited, the outer coroutine will be resumed on the provided
  /// executor.
  template <typename Exec> aw_ex_scope_exit& resume_on(Exec* Executor) {
    return resume_on(tmc::detail::executor_traits<Exec>::type_erased(*Executor)
    );
  }

  /// When awaited, the outer coroutine will be resumed with the provided
  /// priority.
  inline aw_ex_scope_exit& with_priority(size_t Priority) {
    prio = Priority;
    return *this;
  }
};

/// The awaitable type returned by `tmc::enter()`.
template <typename E>
class [[nodiscard("You must co_await aw_ex_scope_enter for it to have any "
                  "effect.")]] aw_ex_scope_enter
    : tmc::detail::AwaitTagNoGroupAsIs {
  friend aw_ex_scope_enter<E> enter<E>(E&);
  friend aw_ex_scope_enter<E> enter<E>(E*);
  E& scope_executor;
  tmc::detail::ex_any* continuation_executor;
  size_t prio;
  aw_ex_scope_enter(E& Executor)
      : scope_executor(Executor),
        continuation_executor(tmc::detail::this_thread::executor),
        prio(tmc::detail::this_thread::this_task.prio) {}

public:
  /// Always suspends.
  inline bool await_ready() {
    // always have to suspend here, even if we can get the lock right away
    // we need to resume() inside of run_loop so that if this coro gets
    // suspended, it won't suspend while holding the lock forever
    return false;
  }

  /// Switch this task to the target executor.
  TMC_FORCE_INLINE inline std::coroutine_handle<>
  await_suspend(std::coroutine_handle<> Outer) {
    return tmc::detail::executor_traits<E>::task_enter_context(
      scope_executor, Outer, prio
    );
  }

  /// Returns an `aw_ex_scope_exit` with an `exit()` method that can be called
  /// to exit the executor, and resume this task back on its original executor.
  inline aw_ex_scope_exit<E> await_resume() {
    tmc::detail::this_thread::this_task.prio = prio;
    // TODO setting the priority on the scope_exit object may not be necessary
    // as we already set it on the thread local
    // When is it valid for these to be different?
    return aw_ex_scope_exit<E>(continuation_executor, prio);
  }

  /// When awaited, the outer coroutine will be resumed with the provided
  /// priority.
  inline aw_ex_scope_enter& with_priority(size_t Priority) {
    prio = Priority;
    return *this;
  }
};

/// Returns an awaitable that suspends the current task and resumes it in the
/// target executor's context. It may be resumed on a different thread than the
/// one calling enter(). This is
/// idempotent, and is similar in effect to `co_await resume_on(exec);`,
/// but additionally saves the current priority in the case of an `exec` that
/// does not use priority internally, such as `ex_braid` or `ex_cpu`.
template <typename E> inline aw_ex_scope_enter<E> enter(E& Executor) {
  return aw_ex_scope_enter<E>(Executor);
}
/// A convenience function identical to tmc::enter(E& exec)
template <typename E> inline aw_ex_scope_enter<E> enter(E* Executor) {
  return aw_ex_scope_enter<E>(*Executor);
}
} // namespace tmc
