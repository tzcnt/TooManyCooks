#pragma once
#include "tmc/detail/concepts.hpp"
#include "tmc/detail/thread_locals.hpp"
#include "tmc/task.hpp"
#include <atomic>
#include <cassert>
#include <coroutine>
#include <functional>
#include <iterator>
#include <mutex>

// TODO split eager and lazy versions of this into separate structs, or make
// eager a template parameter

// eager doesn't need to store wrapped, but debug check it was awaited
// lazy can have move constructor

namespace tmc {
template <typename result_t, size_t count> struct aw_task_many_early;
template <IsNotVoid result_t, size_t count>
struct aw_task_many_early<result_t, count> {
  using wrapped_t = task<result_t>;
  static_assert(sizeof(wrapped_t) == sizeof(std::coroutine_handle<>));
  static_assert(alignof(wrapped_t) == alignof(std::coroutine_handle<>));
  using wrapped_arr_t = std::conditional_t<count == 0, std::vector<work_item>,
                                           std::array<work_item, count>>;
  using result_arr_t = std::conditional_t<count == 0, std::vector<result_t>,
                                          std::array<result_t, count>>;
  // written at initiation
  wrapped_arr_t wrapped;
  std::coroutine_handle<> continuation;
  size_t prio;
  // written at completion
  result_arr_t result;
  std::atomic<int64_t> done_count;
  // For use when count is known at compile time
  template <typename Iter>
  aw_task_many_early(Iter task_iter, size_t prio_in)
    requires(std::is_convertible_v<typename std::iter_value_t<Iter>, wrapped_t>)
      : prio(prio_in) {
    const auto size = count;
    for (size_t i = 0; i < size; ++i) {
      // TODO capture an iter_adapter that applies these transformations instead
      // of the whole wrapped arr
      wrapped_t t = *task_iter;
      auto &p = t.promise();
      p.continuation = &continuation;
      p.done_count = &done_count;
      p.result_ptr = &result[i];
      wrapped[i] = std::coroutine_handle<>(t);
      ++task_iter;
    }
    done_count.store(size, std::memory_order_release);
    detail::this_thread::executor->post_bulk(wrapped.data(), prio, size);
  }
  // For use when count is runtime dynamic
  template <typename Iter>
  aw_task_many_early(Iter task_iter, size_t prio_in, size_t count_in)
    requires(count == 0 &&
             std::is_convertible_v<typename std::iter_value_t<Iter>, wrapped_t>)
      : prio(prio_in) {
    const auto size = count_in;
    wrapped.resize(size);
    result.resize(size);
    for (size_t i = 0; i < size; ++i) {
      wrapped_t t = *task_iter;
      auto &p = t.promise();
      p.continuation = &continuation;
      p.done_count = &done_count;
      p.result_ptr = &result[i];
      wrapped[i] = std::coroutine_handle<>(t);
      ++task_iter;
    }
    done_count.store(size, std::memory_order_release);
    detail::this_thread::executor->post_bulk(wrapped.data(), prio, size);
  }
  // For use when count is known at compile time
  template <typename Callable>
  aw_task_many_early(Callable *callable_in, size_t prio_in)
    requires(!std::is_convertible_v<Callable, std::coroutine_handle<>> &&
             std::is_invocable_r_v<result_t, Callable>)
      : prio(prio_in) {
    const auto size = count;
    for (size_t i = 0; i < size; ++i) {
      wrapped_t t = [](Callable callable) -> wrapped_t {
        co_return callable();
      }(callable_in[i]);
      auto &p = t.promise();
      p.continuation = &continuation;
      p.done_count = &done_count;
      p.result_ptr = &result[i];
      wrapped[i] = std::coroutine_handle<>(t);
    }
    done_count.store(size, std::memory_order_release);
    detail::this_thread::executor->post_bulk(wrapped.data(), prio, size);
  }
  // For use when count is runtime dynamic
  template <typename Callable>
  aw_task_many_early(Callable *callable_in, size_t prio_in, size_t count_in)
    requires(!std::is_convertible_v<Callable, std::coroutine_handle<>> &&
             std::is_invocable_r_v<result_t, Callable> && count == 0)
      : prio(prio_in) {
    const auto size = count_in;
    wrapped.resize(size);
    result.resize(size);
    for (size_t i = 0; i < size; ++i) {
      wrapped_t t = [](Callable callable) -> wrapped_t {
        co_return callable();
      }(callable_in[i]);
      auto &p = t.promise();
      p.continuation = &continuation;
      p.done_count = &done_count;
      p.result_ptr = &result[i];
      wrapped[i] = std::coroutine_handle<>(t);
    }
    done_count.store(size, std::memory_order_release);
    detail::this_thread::executor->post_bulk(wrapped.data(), prio, size);
  }
  constexpr bool await_ready() const noexcept { return false; }

  std::coroutine_handle<>
  await_suspend(std::coroutine_handle<> outer) noexcept {
    continuation = outer;
    auto remaining = done_count.fetch_sub(1, std::memory_order_acq_rel);
    if (remaining > 0) {
      return std::noop_coroutine();
    } else {
      return outer;
    }
  }

  result_arr_t &&await_resume() noexcept { return std::move(result); }

  ~aw_task_many_early() noexcept {}
  aw_task_many_early(const aw_task_many_early &) = delete;
  aw_task_many_early &operator=(const aw_task_many_early &) = delete;
  aw_task_many_early(aw_task_many_early &&other) = delete;
  // aw_task_many_early(aw_task_many_early&& other) {
  //   continuation = std::move(other.continuation);
  //   wrapped = std::move(other.wrapped);
  //   result = std::move(other.result);
  //   prio = other.prio;
  //   did_await = other.did_await;
  //   other.did_await = true; // prevent other from posting
  // }
  aw_task_many_early &operator=(aw_task_many_early &&other) = delete;
  // aw_task_many_early& operator=(aw_task_many_early&& other) {
  //   continuation = std::move(other.continuation);
  //   wrapped = std::move(other.wrapped);
  //   result = std::move(other.result);
  //   prio = other.prio;
  //   did_await = other.did_await;
  //   other.did_await = true; // prevent other from posting
  //   return *this;
  // }
};

template <IsVoid result_t, size_t count>
struct aw_task_many_early<result_t, count> {
  using wrapped_t = task<void>;
  static_assert(sizeof(wrapped_t) == sizeof(std::coroutine_handle<>));
  static_assert(alignof(wrapped_t) == alignof(std::coroutine_handle<>));
  using wrapped_arr_t = std::conditional_t<count == 0, std::vector<work_item>,
                                           std::array<work_item, count>>;
  wrapped_arr_t wrapped;
  std::coroutine_handle<> continuation;
  size_t prio;
  // written at completion
  std::atomic<int64_t> done_count;
  // For use when count is known at compile time
  template <typename Iter>
  aw_task_many_early(Iter task_iter, size_t prio_in)
    requires(std::is_convertible_v<typename std::iter_value_t<Iter>, wrapped_t>)
      : prio(prio_in) {
    const auto size = count;
    for (size_t i = 0; i < size; ++i) {
      wrapped_t t = *task_iter;
      auto &p = t.promise();
      p.continuation = &continuation;
      p.done_count = &done_count;
      wrapped[i] = std::coroutine_handle<>(t);
      ++task_iter;
    }
    done_count.store(size, std::memory_order_release);
    detail::this_thread::executor->post_bulk(wrapped.data(), prio, size);
  }
  // For use when count is runtime dynamic
  template <typename Iter>
  aw_task_many_early(Iter task_iter, size_t prio_in, size_t count_in)
    requires(std::is_convertible_v<typename std::iter_value_t<Iter>, wrapped_t>)
      : prio(prio_in) {
    const auto size = count_in;
    wrapped.resize(size);
    for (size_t i = 0; i < size; ++i) {
      wrapped_t t = *task_iter;
      auto &p = t.promise();
      p.continuation = &continuation;
      p.done_count = &done_count;
      wrapped[i] = std::coroutine_handle<>(t);
      ++task_iter;
    }
    done_count.store(size, std::memory_order_release);
    detail::this_thread::executor->post_bulk(wrapped.data(), prio, size);
  }
  // For use when count is known at compile time
  template <typename Callable>
  aw_task_many_early(Callable *callable_in, size_t prio_in)
    requires(!std::is_convertible_v<Callable, std::coroutine_handle<>> &&
             std::is_invocable_r_v<result_t, Callable>)
      : prio(prio_in) {
    const auto size = count;
    for (size_t i = 0; i < size; ++i) {
      wrapped_t t = [](Callable callable) -> wrapped_t {
        callable();
        co_return;
      }(callable_in[i]);
      auto &p = t.promise();
      p.continuation = &continuation;
      p.done_count = &done_count;
      wrapped[i] = std::coroutine_handle<>(t);
    }
    done_count.store(size, std::memory_order_release);
    detail::this_thread::executor->post_bulk(wrapped.data(), prio, size);
  }
  // For use when count is runtime dynamic
  template <typename Callable>
  aw_task_many_early(Callable *callable_in, size_t prio_in, size_t count_in)
    requires(!std::is_convertible_v<Callable, std::coroutine_handle<>> &&
             std::is_invocable_r_v<result_t, Callable> && count == 0)
      : prio(prio_in) {
    const auto size = count_in;
    wrapped.resize(size);
    for (size_t i = 0; i < size; ++i) {
      wrapped_t t = [](Callable callable) -> wrapped_t {
        callable();
        co_return;
      }(callable_in[i]);
      auto &p = t.promise();
      p.continuation = &continuation;
      p.done_count = &done_count;
      wrapped[i] = std::coroutine_handle<>(t);
    }
    done_count.store(size, std::memory_order_release);
    detail::this_thread::executor->post_bulk(wrapped.data(), prio, size);
  }
  constexpr bool await_ready() const noexcept { return false; }

  std::coroutine_handle<>
  await_suspend(std::coroutine_handle<> outer) noexcept {
    continuation = outer;
    auto remaining = done_count.fetch_sub(1, std::memory_order_acq_rel);
    if (remaining > 0) {
      return std::noop_coroutine();
    } else {
      return outer;
    }
  }

  constexpr void await_resume() const noexcept {}

  // automatic post without co_await IF the func doesn't return a value
  // for void result_t only
  ~aw_task_many_early() noexcept {}

  aw_task_many_early(const aw_task_many_early &) = delete;
  aw_task_many_early &operator=(const aw_task_many_early &) = delete;
  aw_task_many_early(aw_task_many_early &&other) = delete;
  // aw_task_many_early(aw_task_many_early&& other) {
  //   wrapped = std::move(other.wrapped);
  //   prio = other.prio;
  //   did_await = other.did_await;
  //   other.did_await = true; // prevent other from posting
  // }
  aw_task_many_early &operator=(aw_task_many_early &&other) = delete;
  // aw_task_many_early& operator=(aw_task_many_early&& other) {
  //   wrapped = std::move(other.wrapped);
  //   prio = other.prio;
  //   did_await = other.did_await;
  //   other.did_await = true; // prevent other from posting
  //   return *this;
  // }
};

// Eager spawn
// For use when count is known at compile time
template <size_t count, typename Iter,
          typename result_t = std::iter_value_t<Iter>::result_type>
[[nodiscard(
    "You must co_await the return of "
    "spawn_many_early(). It is not safe to destroy aw_early_many_task prior to "
    "awaiting it.")]] aw_task_many_early<result_t, count>
spawn_many_early(Iter t)
  requires(std::is_convertible_v<std::iter_value_t<Iter>, task<result_t>>)
{
  static_assert(count != 0);
  return aw_task_many_early<result_t, count>(
      t, detail::this_thread::this_task.prio);
}

template <size_t count, typename Iter,
          typename result_t = std::iter_value_t<Iter>::result_type>
[[nodiscard(
    "You must co_await the return of "
    "spawn_many_early(). It is not safe to destroy aw_early_many_task prior to "
    "awaiting it.")]] aw_task_many_early<result_t, count>
spawn_many_early(Iter t, size_t prio)
  requires(std::is_convertible_v<std::iter_value_t<Iter>, task<result_t>>)
{
  static_assert(count != 0);
  return aw_task_many_early<result_t, count>(t, prio);
}

template <size_t count, typename result_t, typename Callable>
[[nodiscard(
    "You must co_await the return of "
    "spawn_many_early(). It is not safe to destroy aw_early_many_task prior to "
    "awaiting it.")]] aw_task_many_early<result_t, count>
spawn_many_early(Callable *c)
  requires(!std::is_convertible_v<Callable, std::coroutine_handle<>> &&
           std::is_invocable_r_v<result_t, Callable>)
{
  static_assert(count != 0);
  return aw_task_many_early<result_t, count>(
      c, detail::this_thread::this_task.prio);
}

template <size_t count, typename result_t, typename Callable>
[[nodiscard(
    "You must co_await the return of "
    "spawn_many_early(). It is not safe to destroy aw_early_many_task prior to "
    "awaiting it.")]] aw_task_many_early<result_t, count>
spawn_many_early(Callable *c, size_t prio)
  requires(!std::is_convertible_v<Callable, std::coroutine_handle<>> &&
           std::is_invocable_r_v<result_t, Callable>)
{
  static_assert(count != 0);
  return aw_task_many_early<result_t, count>(c, prio);
}

// For use when count is a runtime parameter
template <typename Iter,
          typename result_t = std::iter_value_t<Iter>::result_type>
[[nodiscard(
    "You must co_await the return of "
    "spawn_many_early(). It is not safe to destroy aw_early_many_task prior to "
    "awaiting it.")]] aw_task_many_early<result_t, 0>
spawn_many_early(Iter t, size_t count)
  requires(std::is_convertible_v<std::iter_value_t<Iter>, task<result_t>>)
{
  return aw_task_many_early<result_t, 0>(t, detail::this_thread::this_task.prio,
                                         count);
}

template <typename Iter,
          typename result_t = std::iter_value_t<Iter>::result_type>
[[nodiscard(
    "You must co_await the return of "
    "spawn_many_early(). It is not safe to destroy aw_early_many_task prior to "
    "awaiting it.")]] aw_task_many_early<result_t, 0>
spawn_many_early(Iter t, size_t prio, size_t count)
  requires(std::is_convertible_v<std::iter_value_t<Iter>, task<result_t>>)
{
  return aw_task_many_early<result_t, 0>(t, prio, count);
}

template <typename result_t, typename Callable>
[[nodiscard(
    "You must co_await the return of "
    "spawn_many_early(). It is not safe to destroy aw_early_many_task prior to "
    "awaiting it.")]] aw_task_many_early<result_t, 0>
spawn_many_early(Callable *c, size_t count)
  requires(!std::is_convertible_v<Callable, std::coroutine_handle<>> &&
           std::is_invocable_r_v<result_t, Callable>)
{
  return aw_task_many_early<result_t, 0>(c, detail::this_thread::this_task.prio,
                                         count);
}

template <typename result_t, typename Callable>
[[nodiscard(
    "You must co_await the return of "
    "spawn_many_early(). It is not safe to destroy aw_early_many_task prior to "
    "awaiting it.")]] aw_task_many_early<result_t, 0>
spawn_many_early(Callable *c, size_t prio, size_t count)
  requires(!std::is_convertible_v<Callable, std::coroutine_handle<>> &&
           std::is_invocable_r_v<result_t, Callable>)
{
  return aw_task_many_early<result_t, 0>(c, prio, count);
}

} // namespace tmc
