#pragma once
#include "tmc/detail/concepts.hpp"
#include "tmc/detail/thread_locals.hpp"
#include <coroutine>
#include <mutex>
namespace tmc {
struct aw_resume_on {
  detail::type_erased_executor *executor;
  // don't need to suspend if we are already running on the requested executor
  inline bool await_ready() const noexcept {
    return detail::this_thread::executor == executor;
  }

  inline void await_suspend(std::coroutine_handle<> outer) const noexcept {
    executor->post_variant(std::move(outer),
                           detail::this_thread::this_task.prio);
  }

  inline void await_resume() const noexcept {}
};
[[nodiscard("You must co_await the return of "
            "resume_on().")]] constexpr aw_resume_on
resume_on(detail::type_erased_executor *executor) {
  return {executor};
}

template <detail::TypeErasableExecutor Exec>
[[nodiscard("You must co_await the return of "
            "resume_on().")]] constexpr aw_resume_on
resume_on(Exec &executor) {
  return resume_on(executor.type_erased());
}

template <detail::TypeErasableExecutor Exec>
[[nodiscard("You must co_await the return of "
            "resume_on().")]] constexpr aw_resume_on
resume_on(Exec *executor) {
  return resume_on(executor->type_erased());
}

// TODO this isn't right - we need to call maybe_change_prio_slot() on SOME
// executor, but not on others Not sure if overhead of checking prio should be
// required for other calls Also, do we always check yield_if_requested() or
// should that be a separate user call?
// [[nodiscard("You must co_await the return of change_priority().")]] inline
// aw_resume_on change_priority(size_t priority) {
//   return {detail::this_thread::executor, priority};
// }
} // namespace tmc
