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
  wrapped_t wrapped;
  result_t result;
  size_t prio;
  bool did_await;
  aw_spawned_task(wrapped_t wrapped_in, size_t prio_in)
      : wrapped(wrapped_in), prio(prio_in), did_await(false) {}
  constexpr bool await_ready() const noexcept { return false; }

  constexpr void await_suspend(std::coroutine_handle<> outer) noexcept {
    did_await = true;
    auto &p = wrapped.promise();
    p.continuation = outer.address();
    p.result_ptr = &result;
    // TODO is release fence required here? maybe not if there's one inside
    // queue?
    detail::this_thread::executor->post_variant(
        std::coroutine_handle<>(wrapped), prio);
  }

  constexpr result_t await_resume() const noexcept { return result; }

  ~aw_spawned_task() noexcept {
    // If you spawn a function that returns a non-void type,
    // then you must co_await the return of spawn!
    assert(did_await);
  }
  aw_spawned_task(const aw_spawned_task &) = delete;
  aw_spawned_task &operator=(const aw_spawned_task &) = delete;
  aw_spawned_task(aw_spawned_task &&other) {
    wrapped = std::move(other.wrapped);
    result = std::move(other.result);
    prio = other.prio;
    did_await = other.did_await;
    assert(!did_await); // it's not valid to move this after posting; wrapped
                        // points to this
    other.did_await = true; // prevent other from posting
  }
  aw_spawned_task &operator=(aw_spawned_task &&other) {
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
};

template <IsVoid result_t> struct aw_spawned_task<result_t> {
  using wrapped_t = task<result_t>;
  wrapped_t wrapped;
  size_t prio;
  bool did_await;
  aw_spawned_task(wrapped_t wrapped_in, size_t prio_in)
      : wrapped(wrapped_in), prio(prio_in), did_await(false) {}
  constexpr bool await_ready() const noexcept { return false; }

  constexpr void await_suspend(std::coroutine_handle<> outer) noexcept {
    did_await = true;
    auto &p = wrapped.promise();
    p.continuation = outer.address();
    // TODO is release fence required here? maybe not if there's one inside the
    // queue?
    detail::this_thread::executor->post_variant(
        std::coroutine_handle<>(wrapped), prio);
  }

  constexpr void await_resume() const noexcept {}

  // automatic post without co_await IF the func doesn't return a value
  // for void result_t only
  ~aw_spawned_task() noexcept {
    if (!did_await) {
      detail::this_thread::executor->post_variant(
          std::coroutine_handle<>(wrapped), prio);
    }
  }
  aw_spawned_task(const aw_spawned_task &) = delete;
  aw_spawned_task &operator=(const aw_spawned_task &) = delete;
  aw_spawned_task(aw_spawned_task &&other) {
    wrapped = std::move(other.wrapped);
    prio = other.prio;
    did_await = other.did_await;
    assert(!did_await); // it's not valid to move this after posting; wrapped
                        // points to this
    other.did_await = true; // prevent other from posting
  }
  aw_spawned_task &operator=(aw_spawned_task &&other) {
    wrapped = std::move(other.wrapped);
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
};

template <typename result_t> aw_spawned_task<result_t> spawn(task<result_t> t) {
  // TODO get rid of aw_spawned_task as it serves no purpose, and just spawn
  // directly here

  // Add with_priority() func to task to allow changing priorities,
  // that's the only way awaiting this is useful (changing prio / exec)
  // (or change_priority()) - these can't be implemented easily
  return aw_spawned_task<result_t>(t, detail::this_thread::this_task.prio);
}

template <typename result_t>
aw_spawned_task<result_t> spawn(task<result_t> t, size_t prio) {
  return aw_spawned_task<result_t>(t, prio);
}
} // namespace tmc
