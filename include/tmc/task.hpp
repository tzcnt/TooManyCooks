#pragma once
#include "tmc/detail/concepts.hpp"
#include "tmc/detail/thread_locals.hpp"
#include <atomic>
#include <coroutine>
#include <exception>
#include <functional>
#include <type_traits>

namespace tmc {
template <typename result_t> struct task;

namespace detail {

template <typename result_t> struct task_promise;

// template <typename result_t> struct continuation_resumer {
//   constexpr bool await_ready() const noexcept { return false; }
//   constexpr void await_resume() const noexcept {}
//   constexpr std::coroutine_handle<>
//   await_suspend(std::coroutine_handle<task_promise<result_t>> handle) const
//   noexcept {
//     auto continuation = handle.promise().continuation;
//     if (continuation) {
//       return continuation;
//     } else {
//       handle.destroy();
//       return std::noop_coroutine();
//     }
//   }
// };

template <typename result_t> struct mt1_continuation_resumer {
  static_assert(sizeof(void*) == sizeof(std::coroutine_handle<>));
  static_assert(alignof(void*) == alignof(std::coroutine_handle<>));
  static_assert(std::is_trivially_copyable_v<std::coroutine_handle<>>);
  static_assert(std::is_trivially_destructible_v<std::coroutine_handle<>>);
  constexpr bool await_ready() const noexcept { return false; }
  constexpr void await_resume() const noexcept {}
  constexpr std::coroutine_handle<>
  await_suspend(std::coroutine_handle<task_promise<result_t>> handle
  ) const noexcept {
    auto& p = handle.promise();
    void* raw_continuation = p.continuation;
    if (p.done_count == nullptr) {
      // solo task, lazy execution
      std::coroutine_handle<> continuation =
        std::coroutine_handle<>::from_address(raw_continuation);
      std::coroutine_handle<> next;
      if (continuation) {
        if (this_thread::executor == p.continuation_executor) {
          next = continuation;
        } else {
          static_cast<detail::type_erased_executor*>(p.continuation_executor)
            ->post_variant(
              std::move(continuation), this_thread::this_task.prio
            );
          next = std::noop_coroutine();
        }
      } else {
        next = std::noop_coroutine();
      }
      handle.destroy();
      return next;
    } else { // p.done_count != nullptr
             // task is part of a spawn_many group, or eagerly executed
             // continuation is a std::coroutine_handle<>*
             // continuation_executor is a detail::type_erased_executor**

      std::coroutine_handle<> next;
      if (p.done_count->fetch_sub(1, std::memory_order_acq_rel) == 0) {
        std::coroutine_handle<> continuation =
          *(static_cast<std::coroutine_handle<>*>(raw_continuation));
        if (continuation) {
          detail::type_erased_executor* continuation_executor =
            *static_cast<detail::type_erased_executor**>(p.continuation_executor
            );
          if (this_thread::executor == continuation_executor) {
            next = continuation;
          } else {
            continuation_executor->post_variant(
              std::move(continuation), this_thread::this_task.prio
            );
            next = std::noop_coroutine();
          }
        } else {
          next = std::noop_coroutine();
        }
      } else {
        next = std::noop_coroutine();
      }
      // TODO where to call destroy, and where not?
      handle.destroy();
      return next;
    }
    ///////////////////// OLD below
    // std::coroutine_handle<> next;
    // if (p.continuation && (p.done_count == nullptr ||
    // p.done_count->fetch_sub(1, std::memory_order_acq_rel) == 0)) {
    //   if (this_thread::executor == p.continuation_executor) {
    //     next = p.continuation;
    //   } else {
    //     p.continuation_executor->post_variant(std::move(p.continuation),
    //     this_thread::this_task.prio); next = std::noop_coroutine();
    //   }
    // } else {
    //   next = std::noop_coroutine();
    // }
    // handle.destroy();
    // return next;
  }
};

template <IsNotVoid result_t> struct task_promise<result_t> {
  // coroutine impls
  task_promise()
      : continuation{nullptr}, continuation_executor{this_thread::executor},
        done_count{nullptr}, result_ptr{nullptr} {}
  constexpr std::suspend_always initial_suspend() const noexcept { return {}; }
  constexpr mt1_continuation_resumer<result_t> final_suspend() const noexcept {
    return {};
  }
  task<result_t> get_return_object() noexcept {
    return {task<result_t>::from_promise(*this)};
  }
  void unhandled_exception() {
    throw;
    // exc = std::current_exception();
  }

  void return_value(result_t&& value) { *result_ptr = std::move(value); }
  void return_value(const result_t& value) { *result_ptr = value; }

  // friend struct mt1_continuation_resumer<result_t>;
  // friend struct aw_task<result_t>;
  void* continuation;
  void* continuation_executor;
  std::atomic<int64_t>*
    done_count; // if this is non-nullptr, fetch_sub & check before resuming
  result_t*
    result_ptr; // if this is non-nullptr, return val to pointed-to location
  // std::exception_ptr exc;
};

template <IsVoid result_t> struct task_promise<result_t> {
  // coroutine impls
  task_promise()
      : continuation{nullptr}, continuation_executor{this_thread::executor},
        done_count{nullptr} {}
  constexpr std::suspend_always initial_suspend() const noexcept { return {}; }
  constexpr mt1_continuation_resumer<result_t> final_suspend() const noexcept {
    return {};
  }
  task<result_t> get_return_object() noexcept {
    return {task<result_t>::from_promise(*this)};
  }
  void unhandled_exception() {
    throw;
    // exc = std::current_exception();
  }

  void return_void() {}

  // friend struct mt1_continuation_resumer<result_t>;
  // friend struct aw_task<result_t>;
  void* continuation;
  void* continuation_executor;
  std::atomic<int64_t>*
    done_count; // if this is non-nullptr, fetch_sub & check before resuming
  // std::exception_ptr exc;
};

} // namespace detail

template <typename result_t> struct aw_task;
template <typename result_t>
struct task : std::coroutine_handle<detail::task_promise<result_t>> {
  using result_type = result_t;
  using promise_type = detail::task_promise<result_t>;
  aw_task<result_t> operator co_await() { return aw_task<result_t>(*this); }

  inline task& resume_on(detail::type_erased_executor* e) {
    std::coroutine_handle<detail::task_promise<result_t>>::promise()
      .continuation_executor = e;
    return *this;
  }
  template <detail::TypeErasableExecutor Exec> task& resume_on(Exec& executor) {
    return resume_on(executor.type_erased());
  }
  template <detail::TypeErasableExecutor Exec> task& resume_on(Exec* executor) {
    return resume_on(executor->type_erased());
  }
};

template <IsNotVoid result_t> struct aw_task<result_t> {
  task<result_t> handle;
  result_t result;

  constexpr aw_task(const task<result_t>& handle_in) : handle(handle_in) {}
  constexpr bool await_ready() const noexcept { return handle.done(); }
  constexpr std::coroutine_handle<> await_suspend(std::coroutine_handle<> outer
  ) noexcept {
    auto& p = handle.promise();
    p.continuation = outer.address();
    p.result_ptr = &result;
    return handle;
  }
  constexpr result_t await_resume() const noexcept { return result; }
};

template <IsVoid result_t> struct aw_task<result_t> {
  task<result_t> inner;

  constexpr aw_task(const task<result_t>& handle_in) : inner(handle_in) {}
  constexpr bool await_ready() const noexcept { return inner.done(); }
  constexpr std::coroutine_handle<> await_suspend(std::coroutine_handle<> outer
  ) const noexcept {
    auto& p = inner.promise();
    p.continuation = outer.address();
    return inner;
  }
  constexpr void await_resume() const noexcept {}
};

template <typename E, typename T>
void post(E& ex, T&& item, size_t priority)
  requires(std::is_convertible_v<T, std::coroutine_handle<>>)
{
  ex.post_variant(std::coroutine_handle<>(std::forward<T>(item)), priority);
}
template <typename E, typename T>
void post(E& ex, T&& item, size_t priority)
  requires(!std::is_convertible_v<T, std::coroutine_handle<>> && std::is_convertible_v<std::invoke_result_t<T>, std::coroutine_handle<>>)
{
  ex.post_variant(std::coroutine_handle<>(item()), priority);
}
#if WORK_ITEM_IS(CORO)
template <typename E, typename T>
void post(E& ex, T&& item, size_t priority)
  requires(!std::is_convertible_v<T, std::coroutine_handle<>> && std::is_void_v<std::invoke_result_t<T>>)
{
  ex.post_variant(
    std::coroutine_handle<>([](T t) -> task<void> {
      t();
      co_return;
    }(std::forward<T>(item))),
    priority
  );
}
#else
template <typename E, typename T>
void post(E& ex, T&& item, size_t priority)
  requires(!std::is_convertible_v<T, std::coroutine_handle<>> && std::is_void_v<std::invoke_result_t<T>>)
{
  ex.post_variant(std::forward<T>(item), priority);
}
#endif
} // namespace tmc
