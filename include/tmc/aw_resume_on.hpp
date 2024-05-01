#pragma once
#include "tmc/detail/concepts.hpp" // IWYU pragma: keep
#include "tmc/detail/thread_locals.hpp"
#include "tmc/task.hpp"
#include <coroutine>

namespace tmc {
/// The awaitable type returned by `tmc::resume_on()`.
class [[nodiscard("You must co_await aw_resume_on for it to have any "
                  "effect.")]] aw_resume_on {
  detail::type_erased_executor* executor;
  size_t prio;

public:
  /// It is recommended to call `resume_on()` instead of using this constructor
  /// directly.
  aw_resume_on(detail::type_erased_executor* Executor)
      : executor(Executor), prio(detail::this_thread::this_task.prio) {}

  /// Resume immediately if outer is already running on the requested executor,
  /// at the requested priority.
  inline bool await_ready() const noexcept {
    return detail::this_thread::executor == executor &&
           detail::this_thread::this_task.prio == prio;
  }

  /// Post the outer task to the requested executor.
  TMC_FORCE_INLINE inline void await_suspend(std::coroutine_handle<> Outer
  ) const noexcept {
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
    // Directly changing detail::.this_thread::this_task.prio is insufficient,
    // as it doesn't update the task_stopper_bitsets.
    prio = Priority;
    return *this;
  }
};

/// Returns an awaitable that moves this task onto the requested executor. If
/// this task is already running on the requested executor, the co_await will do
/// nothing.
inline aw_resume_on resume_on(detail::type_erased_executor* Executor) {
  return aw_resume_on(Executor);
}

/// Returns an awaitable that moves this task onto the requested executor. If
/// this task is already running on the requested executor, the co_await will do
/// nothing.
template <detail::TypeErasableExecutor Exec>
inline aw_resume_on resume_on(Exec& Executor) {
  return resume_on(Executor.type_erased());
}

/// Returns an awaitable that moves this task onto the requested executor. If
/// this task is already running on the requested executor, the co_await will do
/// nothing.
template <detail::TypeErasableExecutor Exec>
inline aw_resume_on resume_on(Exec* Executor) {
  return resume_on(Executor->type_erased());
}

/// Equivalent to `resume_on(tmc::current_executor()).with_priority(Priority);`
inline aw_resume_on change_priority(size_t Priority) {
  return resume_on(detail::this_thread::executor).with_priority(Priority);
}

template <typename E> class aw_ex_scope_exit;
template <typename E> class aw_ex_scope_enter;
template <typename E> inline aw_ex_scope_enter<E> enter(E& Executor);
template <typename E> inline aw_ex_scope_enter<E> enter(E* Executor);

/// The awaitable type returned by `co_await tmc::enter()`.
/// Call `co_await this.exit()` to exit the executor scope.
template <typename E> class aw_ex_scope_exit {
  friend class aw_ex_scope_enter<E>;
  detail::type_erased_executor* continuation_executor;
  size_t prio;

  aw_ex_scope_exit(detail::type_erased_executor* Executor, size_t Priority)
      : continuation_executor(Executor), prio(Priority) {}

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
  inline bool await_ready() { return false; }

  /// Post this task to the continuation executor.
  TMC_FORCE_INLINE inline void await_suspend(std::coroutine_handle<> Outer) {
    continuation_executor->post(std::move(Outer), prio);
  }

  /// Restores the original priority.
  /// Only necessary in case of resuming onto an executor where post()
  /// doesn't respect priority, such as ex_asio.
  inline void await_resume() { detail::this_thread::this_task.prio = prio; }

  /// When awaited, the outer coroutine will be resumed on the provided
  /// executor.
  inline aw_ex_scope_exit& resume_on(detail::type_erased_executor* Executor) {
    continuation_executor = Executor;
    return *this;
  }

  /// When awaited, the outer coroutine will be resumed on the provided
  /// executor.
  template <detail::TypeErasableExecutor Exec>
  aw_ex_scope_exit& resume_on(Exec& Executor) {
    return resume_on(Executor.type_erased());
  }

  /// When awaited, the outer coroutine will be resumed on the provided
  /// executor.
  template <detail::TypeErasableExecutor Exec>
  aw_ex_scope_exit& resume_on(Exec* Executor) {
    return resume_on(Executor->type_erased());
  }

  /// When awaited, the outer coroutine will be resumed with the provided
  /// priority.
  inline aw_ex_scope_exit& resume_with_priority(size_t Priority) {
    prio = Priority;
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
  aw_ex_scope_enter(E& Executor)
      : scope_executor(Executor),
        continuation_executor(detail::this_thread::executor),
        prio(detail::this_thread::this_task.prio) {}

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
    return scope_executor.task_enter_context(Outer, prio);
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
/// Returns a pointer to the current thread's executor. Will
/// return nullptr if this thread is not associated with an executor.
inline detail::type_erased_executor* current_executor() {
  return detail::this_thread::executor;
}

/// Saves the current TMC executor and priority level before awaiting the
/// provided awaitable. After the awaitable completes, returns the awaiting task
/// back to the saved executor / priority.
///
/// Use of this function isn't *strictly* necessary, if you are sure that an
/// external awaitable won't lasso your task onto a different executor.
template <typename Result, typename ExternalAwaitable>
[[nodiscard("You must await the return type of safe_await_external()"
)]] tmc::task<Result>
safe_await_external(ExternalAwaitable&& Awaitable) {
  return [](
           ExternalAwaitable ExAw, tmc::aw_resume_on TakeMeHome
         ) -> tmc::task<Result> {
    auto result = co_await ExAw;
    co_await TakeMeHome;
    co_return result;
  }(static_cast<ExternalAwaitable&&>(Awaitable),
           tmc::resume_on(tmc::detail::this_thread::executor));
}
} // namespace tmc
