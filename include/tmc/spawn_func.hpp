#pragma once
#include "tmc/detail/concepts.hpp"
#include "tmc/detail/thread_locals.hpp"
#include "tmc/task.hpp"
#include <cassert>
#include <coroutine>
#include <functional>
#include <mutex>

namespace tmc {

// An awaitable that wraps a function that will be posted as a separate task.
template <typename result_t> struct aw_spawned_func;
template <IsNotVoid result_t> struct aw_spawned_func<result_t> {
  using wrapped_t = std::function<result_t()>;
  wrapped_t wrapped;
  result_t result;
  size_t prio;
  bool did_await;
  aw_spawned_func(wrapped_t &&wrapped_in, size_t prio_in)
      : wrapped(wrapped_in), prio(prio_in), did_await(false) {}
  constexpr bool await_ready() const noexcept { return false; }

  constexpr void await_suspend(std::coroutine_handle<> outer) noexcept {
    did_await = true;
#if WORK_ITEM_IS(CORO)
    auto t = [](aw_spawned_func *me) -> task<void> {
      me->result = me->wrapped();
      co_return;
    }(this);
    auto &p = t.promise();
    p.continuation = outer.address();
    // TODO is release fence required here?
    detail::this_thread::executor->post_variant(t, prio);
#else
    detail::this_thread::executor->post_variant(
        [this, outer, continuation_executor = detail::this_thread::executor]() {
          result = wrapped();
          if (continuation_executor == detail::this_thread::executor) {
            outer.resume();
          } else {
            continuation_executor->post_variant(
                outer, detail::this_thread::this_task.prio);
          }
        },
        prio);
#endif
    // inner will be posted in destructor
  }

  constexpr result_t await_resume() const noexcept { return result; }

  ~aw_spawned_func() noexcept {
    // If you spawn a function that returns a non-void type,
    // then you must co_await the return of spawn!
    assert(did_await);
  }
  aw_spawned_func(const aw_spawned_func &) = delete;
  aw_spawned_func &operator=(const aw_spawned_func &) = delete;
  aw_spawned_func(aw_spawned_func &&other) {
    wrapped = std::move(other.wrapped);
    result = std::move(other.result);
    prio = other.prio;
    did_await = other.did_await;
    other.did_await = true; // prevent other from posting
  }
  aw_spawned_func &operator=(aw_spawned_func &&other) {
    wrapped = std::move(other.wrapped);
    result = std::move(other.result);
    prio = other.prio;
    did_await = other.did_await;
    other.did_await = true; // prevent other from posting
    return *this;
  }
};

// An awaitable that wraps a function that will be posted as a separate task.
template <IsVoid result_t> struct aw_spawned_func<result_t> {
  using wrapped_t = std::function<result_t()>;
  wrapped_t wrapped;
  size_t prio;
  bool did_await;
  aw_spawned_func(wrapped_t &&wrapped_in, size_t prio_in)
      : wrapped(wrapped_in), prio(prio_in), did_await(false) {}
  constexpr bool await_ready() const noexcept { return false; }

  constexpr void await_suspend(std::coroutine_handle<> outer) noexcept {
    did_await = true;
    //  mark outer to resume after inner completes
#if WORK_ITEM_IS(CORO)
    auto t = [](aw_spawned_func *me) -> task<void> {
      me->wrapped();
      co_return;
    }(this);
    auto &p = t.promise();
    p.continuation = outer.address();
    // TODO is release fence required here?
    detail::this_thread::executor->post_variant(t, prio);
#else
    detail::this_thread::executor->post_variant(
        [this, outer, continuation_executor = detail::this_thread::executor]() {
          wrapped();
          if (continuation_executor == detail::this_thread::executor) {
            outer.resume();
          } else {
            continuation_executor->post_variant(
                outer, detail::this_thread::this_task.prio);
          }
        },
        prio);
#endif
    // inner will be posted in destructor
  }

  constexpr void await_resume() const noexcept {}

  // automatic post without co_await IF the func doesn't return a value
  // for void result_t only
  ~aw_spawned_func() noexcept {
    if (!did_await) {
#if WORK_ITEM_IS(CORO)
      detail::this_thread::executor->post_variant(
          [](wrapped_t func) -> task<void> {
            func();
            co_return;
          }(wrapped),
          prio);
#else
      detail::this_thread::executor->post_variant(std::move(wrapped), prio);
#endif
    }
  }
  aw_spawned_func(const aw_spawned_func &) = delete;
  aw_spawned_func &operator=(const aw_spawned_func &) = delete;
  aw_spawned_func(aw_spawned_func &&other) {
    wrapped = std::move(other.wrapped);
    prio = other.prio;
    did_await = other.did_await;
    other.did_await = true; // prevent other from posting
  }
  aw_spawned_func &operator=(aw_spawned_func &&other) {
    wrapped = std::move(other.wrapped);
    prio = other.prio;
    did_await = other.did_await;
    other.did_await = true; // prevent other from posting
    return *this;
  }
};

// Wraps a function into a new task that will be posted to the thread pool.
// You may co_await the return value of spawn to suspend the current task
// until the spawned task completes.
template <typename result_t, typename... Args>
aw_spawned_func<result_t> spawn(std::function<result_t(Args...)> func,
                                Args... args) {
  return aw_spawned_func<result_t>(std::bind(func, args...),
                                   detail::this_thread::this_task.prio);
}

template <typename result_t, typename... Args>
aw_spawned_func<result_t> spawn(std::function<result_t(Args...)> func,
                                size_t prio, Args... args) {
  return aw_spawned_func<result_t>(std::bind(func, args...), prio);
}

} // namespace tmc
