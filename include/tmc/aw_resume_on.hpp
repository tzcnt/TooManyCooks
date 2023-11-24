#pragma once
#include "tmc/detail/concepts.hpp"
#include "tmc/detail/thread_locals.hpp"
#include <coroutine>
#include <mutex>
namespace tmc {
/// The awaitable type returned by `tmc::resume_on()`.
class [[nodiscard("You must co_await aw_resume_on for it to have any "
                  "effect.")]] aw_resume_on {
  detail::type_erased_executor* executor;

public:
  /// It is recommended to call `resume_on()` instead of using this constructor
  /// directly.
  aw_resume_on(detail::type_erased_executor* e) : executor(e) {}

  /// Resume immediately if outer is already running on the requested executor.
  inline bool await_ready() const noexcept {
    return detail::this_thread::executor == executor;
  }

  /// Post the outer task to the requested executor.
  inline void await_suspend(std::coroutine_handle<> outer) const noexcept {
    executor->post(std::move(outer), detail::this_thread::this_task.prio);
  }

  /// Does nothing.
  inline void await_resume() const noexcept {}
};

/// Returns an awaitable that moves this task onto the requested executor. If
/// this task is already running on the requested executor, the co_await will do
/// nothing.
inline aw_resume_on resume_on(detail::type_erased_executor* executor) {
  return aw_resume_on(executor);
}

/// Returns an awaitable that moves this task onto the requested executor. If
/// this task is already running on the requested executor, the co_await will do
/// nothing.
template <detail::TypeErasableExecutor Exec>
inline aw_resume_on resume_on(Exec& executor) {
  return resume_on(executor.type_erased());
}

/// Returns an awaitable that moves this task onto the requested executor. If
/// this task is already running on the requested executor, the co_await will do
/// nothing.
template <detail::TypeErasableExecutor Exec>
inline aw_resume_on resume_on(Exec* executor) {
  return resume_on(executor->type_erased());
}

// TODO this isn't right - we need to call maybe_change_prio_slot() on SOME
// executor, but not on others Not sure if overhead of checking prio should be
// required for other calls Also, do we always check yield_if_requested() or
// should that be a separate user call?
// inline aw_resume_on change_priority(size_t priority) {
//   return {detail::this_thread::executor, priority};
// }

template <typename E> class aw_ex_scope_exit;
template <typename E> class aw_ex_scope_enter;
template <typename E> inline aw_ex_scope_enter<E> enter(E& exec);
template <typename E> inline aw_ex_scope_enter<E> enter(E* exec);

/// The awaitable type returned by `co_await tmc::enter()`.
/// Call `co_await this.exit()` to exit the executor scope.
template <typename E> class aw_ex_scope_exit {
  friend class aw_ex_scope_enter<E>;
  detail::type_erased_executor* continuation_executor;
  size_t prio;

  aw_ex_scope_exit(detail::type_erased_executor* exec, size_t priority)
      : continuation_executor(exec), prio(priority) {}

public:
  /// Returns an awaitable that can be co_await'ed to exit the
  /// executor scope. This is idempotent.
  /// (Not strictly necessary - you can just await aw_ex_scope_exit directly -
  /// but makes code a bit easier to understand.)
  [[nodiscard("You must co_await exit() for it to have any "
              "effect.")]] inline aw_ex_scope_exit&
  exit() {
    return *this;
  }

  /// Always suspends.
  constexpr bool await_ready() { return false; }

  /// Post this task to the continuation executor.
  inline void await_suspend(std::coroutine_handle<> outer) {
    continuation_executor->post(std::move(outer), prio);
  }

  /// Restores the original priority.
  /// Only necessary in case of resuming onto an executor where post()
  /// doesn't respect priority, such as ex_asio.
  constexpr void await_resume() { detail::this_thread::this_task.prio = prio; }

  /// When awaited, the outer coroutine will be resumed on the provided
  /// executor.
  inline aw_ex_scope_exit& resume_on(detail::type_erased_executor* executor) {
    continuation_executor = executor;
    return *this;
  }

  /// When awaited, the outer coroutine will be resumed on the provided
  /// executor.
  template <detail::TypeErasableExecutor Exec>
  aw_ex_scope_exit& resume_on(Exec& executor) {
    return resume_on(executor.type_erased());
  }

  /// When awaited, the outer coroutine will be resumed on the provided
  /// executor.
  template <detail::TypeErasableExecutor Exec>
  aw_ex_scope_exit& resume_on(Exec* executor) {
    return resume_on(executor->type_erased());
  }

  /// When awaited, the outer coroutine will be resumed with the provided
  /// priority.
  inline aw_ex_scope_exit& resume_with_priority(size_t priority) {
    prio = priority;
    return *this;
  }
};

/// The awaitable type returned by `tmc::enter()`.
template <typename E>
class [[nodiscard("You must co_await aw_ex_scope_enter for it to have any "
                  "effect.")]] aw_ex_scope_enter {
  friend aw_ex_scope_enter<E> enter<E>(E&);
  friend aw_ex_scope_enter<E> enter<E>(E*);
  E& scope_executor;
  detail::type_erased_executor* continuation_executor;
  size_t prio;
  aw_ex_scope_enter(E& executor)
      : scope_executor(executor),
        continuation_executor(detail::this_thread::executor),
        prio(detail::this_thread::this_task.prio) {}

public:
  /// Always suspends.
  constexpr bool await_ready() {
    // always have to suspend here, even if we can get the lock right away
    // we need to resume() inside of run_loop so that if this coro gets
    // suspended, it won't suspend while holding the lock forever
    return false;
  }

  /// Switch this task to the target executor.
  inline std::coroutine_handle<> await_suspend(std::coroutine_handle<> outer) {
    return scope_executor.task_enter_context(outer, prio);
  }

  /// Returns an `aw_ex_scope_exit` with an `exit()` method that can be called
  /// to exit the executor, and resume this task back on its original executor.
  inline aw_ex_scope_exit<E> await_resume() {
    detail::this_thread::this_task.prio = prio;
    // TODO setting the priority on the scope_exit object may not be necessary
    // as we already set it on the thread local
    // When is it valid for these to be different?
    return aw_ex_scope_exit<E>(continuation_executor, prio);
  }

  /// When awaited, the outer coroutine will be resumed with the provided
  /// priority.
  inline aw_ex_scope_enter& with_priority(size_t priority) {
    prio = priority;
    return *this;
  }
};

/// Returns an awaitable that suspends the current task and resumes it in the
/// target executor's context. It may be resumed on a different thread than the
/// one calling enter(). This is
/// idempotent, and is similar in effect to `co_await resume_on(exec);`,
/// but additionally saves the current priority in the case of an `exec` that
/// does not use priority internally, such as `ex_braid` or `ex_cpu`.
template <typename E> inline aw_ex_scope_enter<E> enter(E& exec) {
  return aw_ex_scope_enter<E>(exec);
}
/// A convenience function identical to tmc::enter(E& exec)
template <typename E> inline aw_ex_scope_enter<E> enter(E* exec) {
  return aw_ex_scope_enter<E>(*exec);
}
} // namespace tmc
