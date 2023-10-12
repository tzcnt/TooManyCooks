#pragma once
#include "tmc/detail/concepts.hpp"
#include "tmc/detail/thread_locals.hpp"
#include <coroutine>
#include <mutex>
namespace tmc {
/// The awaitable type returned by `tmc::resume_on()`.
class [[nodiscard("You must co_await aw_resume_on for it to have any "
                  "effect.")]] aw_resume_on {
  detail::type_erased_executor *executor;

public:
  /// It is recommended to call `resume_on()` instead of using this constructor
  /// directly.
  aw_resume_on(detail::type_erased_executor *e) : executor(e) {}

  /// Resume immediately if outer is already running on the requested executor.
  inline bool await_ready() const noexcept {
    return detail::this_thread::executor == executor;
  }

  /// Post the outer task to the requested executor.
  inline void await_suspend(std::coroutine_handle<> outer) const noexcept {
    executor->post_variant(std::move(outer),
                           detail::this_thread::this_task.prio);
  }

  /// Does nothing.
  inline void await_resume() const noexcept {}
};

/// Returns an awaitable that moves this task onto the requested executor. If
/// this task is already running on the requested executor, the co_await will do
/// nothing.
inline aw_resume_on resume_on(detail::type_erased_executor *executor) {
  return aw_resume_on(executor);
}

/// Returns an awaitable that moves this task onto the requested executor. If
/// this task is already running on the requested executor, the co_await will do
/// nothing.
template <detail::TypeErasableExecutor Exec>
inline aw_resume_on resume_on(Exec &executor) {
  return resume_on(executor.type_erased());
}

/// Returns an awaitable that moves this task onto the requested executor. If
/// this task is already running on the requested executor, the co_await will do
/// nothing.
template <detail::TypeErasableExecutor Exec>
inline aw_resume_on resume_on(Exec *executor) {
  return resume_on(executor->type_erased());
}

// TODO this isn't right - we need to call maybe_change_prio_slot() on SOME
// executor, but not on others Not sure if overhead of checking prio should be
// required for other calls Also, do we always check yield_if_requested() or
// should that be a separate user call?
// inline aw_resume_on change_priority(size_t priority) {
//   return {detail::this_thread::executor, priority};
// }
} // namespace tmc
