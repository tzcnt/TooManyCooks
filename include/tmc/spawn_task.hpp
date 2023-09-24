#pragma once
#include "tmc/detail/concepts.hpp"
#include "tmc/detail/thread_locals.hpp"
#include "tmc/task.hpp"
#include <cassert>
#include <coroutine>
#include <functional>
#include <mutex>
namespace tmc {
template <typename result_t> struct aw_spawned_task;
template <IsNotVoid result_t> struct aw_spawned_task<result_t> {
  using wrapped_t = task<result_t>;
  detail::type_erased_executor *executor;
  wrapped_t wrapped;
  result_t result;
  size_t prio;
  bool did_await;
  aw_spawned_task(wrapped_t wrapped, detail::type_erased_executor *executor,
                  size_t prio)
      : executor(executor), wrapped(wrapped), prio(prio), did_await(false) {}
  constexpr bool await_ready() const noexcept { return false; }

  constexpr void await_suspend(std::coroutine_handle<> outer) noexcept {
    did_await = true;
    auto &p = wrapped.promise();
    p.continuation = outer.address();
    p.result_ptr = &result;
    // TODO is release fence required here? maybe not if there's one inside
    // queue?
    executor->post_variant(std::coroutine_handle<>(wrapped), prio);
  }

  constexpr result_t await_resume() const noexcept { return result; }

  ~aw_spawned_task() noexcept {
    // If you spawn a task that returns a non-void type,
    // then you must co_await the return of spawn!
    assert(did_await);
  }
  aw_spawned_task(const aw_spawned_task &) = delete;
  aw_spawned_task &operator=(const aw_spawned_task &) = delete;
  aw_spawned_task(aw_spawned_task &&other) {
    executor = std::move(other.executor);
    wrapped = std::move(other.wrapped);
    result = std::move(other.result);
    prio = other.prio;
    did_await = other.did_await;
    assert(!did_await); // it's not valid to move this after posting; wrapped
                        // points to this
    other.did_await = true; // prevent other from posting
  }
  aw_spawned_task &operator=(aw_spawned_task &&other) {
    executor = std::move(other.executor);
    wrapped = std::move(other.wrapped);
    result = std::move(other.result);
    prio = other.prio;
    did_await = other.did_await;
    assert(!did_await); // it's not valid to move this after posting; wrapped
                        // points to this
    other.did_await = true; // prevent other from posting
    return *this;
  }

  inline aw_spawned_task &resume_on(detail::type_erased_executor *e) {
    wrapped.promise().continuation_executor = e;
    return *this;
  }
  template <detail::TypeErasableExecutor Exec>
  aw_spawned_task &resume_on(Exec &executor) {
    return resume_on(executor.type_erased());
  }
  template <detail::TypeErasableExecutor Exec>
  aw_spawned_task &resume_on(Exec *executor) {
    return resume_on(executor->type_erased());
  }

  inline aw_spawned_task &run_on(detail::type_erased_executor *e) {
    executor = e;
    return *this;
  }
  template <detail::TypeErasableExecutor Exec>
  aw_spawned_task &run_on(Exec &executor) {
    return run_on(executor.type_erased());
  }
  template <detail::TypeErasableExecutor Exec>
  aw_spawned_task &run_on(Exec *executor) {
    return run_on(executor->type_erased());
  }

  inline aw_spawned_task &with_priority(size_t priority) {
    prio = priority;
    return *this;
  }
};

template <IsVoid result_t> struct aw_spawned_task<result_t> {
  using wrapped_t = task<result_t>;
  detail::type_erased_executor *executor;
  wrapped_t wrapped;
  size_t prio;
  bool did_await;
  aw_spawned_task(wrapped_t wrapped, detail::type_erased_executor *executor,
                  size_t prio)
      : executor(executor), wrapped(wrapped), prio(prio), did_await(false) {}
  constexpr bool await_ready() const noexcept { return false; }

  constexpr void await_suspend(std::coroutine_handle<> outer) noexcept {
    did_await = true;
    auto &p = wrapped.promise();
    p.continuation = outer.address();
    // TODO is release fence required here? maybe not if there's one inside the
    // queue?
    executor->post_variant(std::coroutine_handle<>(wrapped), prio);
  }

  constexpr void await_resume() const noexcept {}

  // automatic post without co_await IF the func doesn't return a value
  // for void result_t only
  ~aw_spawned_task() noexcept {
    if (!did_await) {
      executor->post_variant(std::coroutine_handle<>(wrapped), prio);
    }
  }
  aw_spawned_task(const aw_spawned_task &) = delete;
  aw_spawned_task &operator=(const aw_spawned_task &) = delete;
  aw_spawned_task(aw_spawned_task &&other) {
    executor = std::move(other.executor);
    wrapped = std::move(other.wrapped);
    prio = other.prio;
    did_await = other.did_await;
    other.did_await = true; // prevent other from posting
  }
  aw_spawned_task &operator=(aw_spawned_task &&other) {
    executor = std::move(other.executor);
    wrapped = std::move(other.wrapped);
    prio = other.prio;
    did_await = other.did_await;
    other.did_await = true; // prevent other from posting
    return *this;
  }

  inline aw_spawned_task &resume_on(detail::type_erased_executor *e) {
    wrapped.promise().continuation_executor = e;
    return *this;
  }
  template <detail::TypeErasableExecutor Exec>
  aw_spawned_task &resume_on(Exec &executor) {
    return resume_on(executor.type_erased());
  }
  template <detail::TypeErasableExecutor Exec>
  aw_spawned_task &resume_on(Exec *executor) {
    return resume_on(executor->type_erased());
  }

  inline aw_spawned_task &run_on(detail::type_erased_executor *e) {
    executor = e;
    return *this;
  }
  template <detail::TypeErasableExecutor Exec>
  aw_spawned_task &run_on(Exec &executor) {
    return run_on(executor.type_erased());
  }
  template <detail::TypeErasableExecutor Exec>
  aw_spawned_task &run_on(Exec *executor) {
    return run_on(executor->type_erased());
  }

  inline aw_spawned_task &with_priority(size_t priority) {
    prio = priority;
    return *this;
  }
};

/// spawn() creates a task wrapper that allows you to customize a task, by
/// calling `run_on()`, `resume_on()`, and/or `with_priority()` before the task
/// is spawned.
///
/// If `result_t` is non-void, the task will be spawned when the the wrapper is
/// co_await'ed:
/// `auto result = co_await spawn(task_result()).with_priority(1);`
///
/// If `result_t` is void, you can do the same thing:
/// `co_await spawn(task_void()).with_priority(1);`
///
/// If `result_t` is void, you also have the option to spawn it detached -
/// the task will be spawned when the wrapper temporary is destroyed:
/// `spawn(task_void()).with_priority(1);`

template <typename result_t> aw_spawned_task<result_t> spawn(task<result_t> t) {
  return aw_spawned_task<result_t>(t, detail::this_thread::executor,
                                   detail::this_thread::this_task.prio);
}

} // namespace tmc
