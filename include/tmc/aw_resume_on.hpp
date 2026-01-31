// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/compat.hpp"
#include "tmc/detail/concepts_awaitable.hpp" // IWYU pragma: keep
#include "tmc/detail/mixins.hpp"
#include "tmc/detail/thread_locals.hpp"
#include "tmc/ex_any.hpp"

#include <coroutine>

namespace tmc {
/// The awaitable type returned by `tmc::resume_on()`.
class [[nodiscard(
  "You must co_await aw_resume_on for it to have any "
  "effect."
)]] aw_resume_on : tmc::detail::AwaitTagNoGroupAsIs,
                   public tmc::detail::with_priority_mixin<aw_resume_on> {
  friend class tmc::detail::with_priority_mixin<aw_resume_on>;
  tmc::ex_any* executor;
  size_t prio;

public:
  /// It is recommended to call `resume_on()` instead of using this constructor
  /// directly.
  aw_resume_on(tmc::ex_any* Executor)
      : executor(Executor), prio(tmc::detail::this_thread::this_task.prio) {}

  /// Resume immediately if outer is already running on the requested executor,
  /// at the requested priority.
  inline bool await_ready() const noexcept {
    return tmc::detail::this_thread::exec_prio_is(executor, prio);
  }

  /// Post the outer task to the requested executor.
  inline void await_suspend(std::coroutine_handle<> Outer) const noexcept {
    // For this to work correctly, we must change the priority of the executor
    // thread by posting the task to the executor with the new priority.
    // Directly changing tmc::detail::this_thread::this_task.prio is
    // insufficient, as it doesn't update the task_stopper_bitsets.
    executor->post(std::move(Outer), prio);
  }

  /// Does nothing.
  inline void await_resume() const noexcept {}
};

/// Returns an awaitable that moves this task onto the requested executor. If
/// this task is already running on the requested executor, the co_await will do
/// nothing.
template <typename Exec> inline aw_resume_on resume_on(Exec&& Executor) {
  return aw_resume_on(
    tmc::detail::get_executor_traits<Exec>::type_erased(Executor)
  );
}

/// Returns an awaitable that moves this task onto the requested executor. If
/// this task is already running on the requested executor, the co_await will do
/// nothing.
template <typename Exec> inline aw_resume_on resume_on(Exec* Executor) {
  return aw_resume_on(
    tmc::detail::get_executor_traits<Exec>::type_erased(*Executor)
  );
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
class aw_ex_scope_exit
    : tmc::detail::AwaitTagNoGroupAsIs,
      public tmc::detail::resume_on_mixin<aw_ex_scope_exit<E>>,
      public tmc::detail::with_priority_mixin<aw_ex_scope_exit<E>> {
  friend class aw_ex_scope_enter<E>;
  friend class tmc::detail::resume_on_mixin<aw_ex_scope_exit<E>>;
  friend class tmc::detail::with_priority_mixin<aw_ex_scope_exit<E>>;
  tmc::ex_any* continuation_executor;
  size_t prio;

  aw_ex_scope_exit(tmc::ex_any* Executor, size_t Priority)
      : continuation_executor(Executor), prio(Priority) {}

public:
  /// Returns an awaitable that can be co_await'ed to exit the
  /// executor scope. This is idempotent.
  /// (Not strictly necessary - you can just `co_await std::move(*this);`
  /// directly - but makes code a bit easier to understand.)
  [[nodiscard(
    "You must co_await exit() for it to have any "
    "effect."
  )]] inline aw_ex_scope_exit&&
  exit() {
    return std::move(*this);
  }

  /// Always suspends.
  inline bool await_ready() const noexcept {
    // Always suspends, due to the possibility to resume on another executor.
    return false;
  }

  /// Post this task to the continuation executor.
  inline void await_suspend(std::coroutine_handle<> Outer) noexcept {
    tmc::detail::post_checked(continuation_executor, std::move(Outer), prio);
  }

  inline void await_resume() const noexcept {}
};

/// The awaitable type returned by `tmc::enter()`.
template <typename E>
class [[nodiscard(
  "You must co_await aw_ex_scope_enter for it to have any "
  "effect."
)]] aw_ex_scope_enter
    : tmc::detail::AwaitTagNoGroupAsIs,
      public tmc::detail::with_priority_mixin<aw_ex_scope_enter<E>> {
  friend aw_ex_scope_enter<E> enter<E>(E&);
  friend aw_ex_scope_enter<E> enter<E>(E*);
  friend class tmc::detail::with_priority_mixin<aw_ex_scope_enter<E>>;
  E& scope_executor;
  tmc::ex_any* continuation_executor;
  size_t prio;
  size_t originalPrio;
  aw_ex_scope_enter(E& Executor)
      : scope_executor(Executor),
        continuation_executor(tmc::detail::this_thread::executor),
        prio(tmc::detail::this_thread::this_task.prio),
        originalPrio(tmc::detail::this_thread::this_task.prio) {}

public:
  /// Always suspends.
  inline bool await_ready() const noexcept {
    // Always suspends, due to the possibility to resume on another executor.
    return false;
  }

  /// Switch this task to the target executor.
  inline std::coroutine_handle<>
  await_suspend(std::coroutine_handle<> Outer) noexcept {
    return tmc::detail::get_executor_traits<E>::dispatch(
      scope_executor, Outer, prio
    );
  }

  /// Returns an `aw_ex_scope_exit` with an `exit()` method that can be called
  /// to exit the executor, and resume this task back on its original executor.
  TMC_AWAIT_RESUME inline aw_ex_scope_exit<E> await_resume() noexcept {
    return aw_ex_scope_exit<E>(continuation_executor, originalPrio);
  }
};

/// Returns an awaitable that suspends the current task and resumes it in the
/// target executor's context. It may be resumed on a different thread than the
/// one calling enter(). This is idempotent, and is similar in effect
/// to `co_await resume_on(exec);`, but additionally saves the current priority
/// so that when `exit()` is called, the original priority will be restored.
/// This holds true even if you call `enter().with_priority()`:
/// ```
/// auto originalExec = tmc::current_executor();
/// assert(tmc::current_priority() == 0);
///
/// auto scope = co_await tmc::enter(some_ex).with_priority(1);
/// assert(tmc::current_executor() == some_ex.type_erased());
/// assert(tmc::current_priority() == 1);
/// co_await scope.exit();
///
/// assert(tmc::current_executor() == originalExec);
/// assert(tmc::current_priority() == 0);
/// ```
///
/// `exit()` also offers the `with_priority()` and `resume_on()`
/// customizations that let you modify this behavior when exiting.
template <typename E> inline aw_ex_scope_enter<E> enter(E& Executor) {
  return aw_ex_scope_enter<E>(Executor);
}
/// A convenience function identical to tmc::enter(E& exec)
template <typename E> inline aw_ex_scope_enter<E> enter(E* Executor) {
  return aw_ex_scope_enter<E>(*Executor);
}
} // namespace tmc
