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
template <typename result_t, size_t count> struct aw_task_many;
template <IsNotVoid result_t, size_t count>
struct aw_task_many<result_t, count> {
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
  bool did_post;
  // written at completion
  result_arr_t result;
  std::atomic<int64_t> done_count;
  // For use when count is known at compile time
  template <typename Iter>
  aw_task_many(Iter task_iter, size_t prio_in, bool eager)
    requires(std::is_convertible_v<typename std::iter_value_t<Iter>, wrapped_t>)
      : prio(prio_in), did_post(eager) {
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
    if (eager) {
      done_count.store(size, std::memory_order_release);
      detail::this_thread::executor->post_bulk(wrapped.data(), prio, size);
    } else {
      done_count.store(size - 1, std::memory_order_release);
    }
  }
  // For use when count is runtime dynamic
  template <typename Iter>
  aw_task_many(Iter task_iter, size_t prio_in, size_t count_in, bool eager)
    requires(count == 0 &&
             std::is_convertible_v<typename std::iter_value_t<Iter>, wrapped_t>)
      : prio(prio_in), did_post(eager) {
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
    if (eager) {
      done_count.store(size, std::memory_order_release);
      detail::this_thread::executor->post_bulk(wrapped.data(), prio, size);
    } else {
      done_count.store(size - 1, std::memory_order_release);
    }
  }
  // For use when count is known at compile time
  template <typename Callable>
  aw_task_many(Callable *callable_in, size_t prio_in, bool eager)
    requires(!std::is_convertible_v<Callable, std::coroutine_handle<>> &&
             std::is_invocable_r_v<result_t, Callable>)
      : prio(prio_in), did_post(eager) {
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
    if (eager) {
      done_count.store(size, std::memory_order_release);
      detail::this_thread::executor->post_bulk(wrapped.data(), prio, size);
    } else {
      done_count.store(size - 1, std::memory_order_release);
    }
  }
  // For use when count is runtime dynamic
  template <typename Callable>
  aw_task_many(Callable *callable_in, size_t prio_in, size_t count_in,
               bool eager)
    requires(!std::is_convertible_v<Callable, std::coroutine_handle<>> &&
             std::is_invocable_r_v<result_t, Callable> && count == 0)
      : prio(prio_in), did_post(eager) {
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
    if (eager) {
      done_count.store(size, std::memory_order_release);
      detail::this_thread::executor->post_bulk(wrapped.data(), prio, size);
    } else {
      done_count.store(size - 1, std::memory_order_release);
    }
  }
  constexpr bool await_ready() const noexcept { return false; }

  std::coroutine_handle<>
  await_suspend(std::coroutine_handle<> outer) noexcept {
    continuation = outer;
    if (did_post) {
      auto remaining = done_count.fetch_sub(1, std::memory_order_acq_rel);
      if (remaining > 0) {
        return std::noop_coroutine();
      } else {
        return outer;
      }
    }
    did_post = true;
    // if the newly posted tasks are at least as high priority as the currently
    // running or next-running (yield requested) task, we can directly transfer
    // to one
    bool do_symmetric_transfer =
        prio <= detail::this_thread::this_task.yield_priority->load(
                    std::memory_order_acquire);
    auto postCount =
        do_symmetric_transfer ? wrapped.size() - 1 : wrapped.size();
    if (postCount != 0) {
      // TODO is release fence required here?
      detail::this_thread::executor->post_bulk(wrapped.data(), prio, postCount);
    }
    if (do_symmetric_transfer) {
      // symmetric transfer to the last task IF it should run immediately
#if WORK_ITEM_IS(CORO)
      return wrapped.back();
#elif WORK_ITEM_IS(FUNCORO) || WORK_ITEM_IS(FUNCORO32)
      return wrapped.back().as_coroutine();
#elif WORK_ITEM_IS(FUNC)
      return *wrapped.back().template target<std::coroutine_handle<>>();
#endif
    } else {
      return std::noop_coroutine();
    }
  }

  result_arr_t &&await_resume() noexcept { return std::move(result); }

  ~aw_task_many() noexcept {
    // If you spawn a function that returns a non-void type,
    // then you must co_await the return of spawn!
    assert(did_post);
  }
  aw_task_many(const aw_task_many &) = delete;
  aw_task_many &operator=(const aw_task_many &) = delete;
  aw_task_many(aw_task_many &&other) = delete;
  // aw_task_many(aw_task_many&& other) {
  //   continuation = std::move(other.continuation);
  //   wrapped = std::move(other.wrapped);
  //   result = std::move(other.result);
  //   prio = other.prio;
  //   did_await = other.did_await;
  //   other.did_await = true; // prevent other from posting
  // }
  aw_task_many &operator=(aw_task_many &&other) = delete;
  // aw_task_many& operator=(aw_task_many&& other) {
  //   continuation = std::move(other.continuation);
  //   wrapped = std::move(other.wrapped);
  //   result = std::move(other.result);
  //   prio = other.prio;
  //   did_await = other.did_await;
  //   other.did_await = true; // prevent other from posting
  //   return *this;
  // }

  inline aw_task_many &resume_on(detail::type_erased_executor *e) {
    for (size_t i = 0; i < wrapped.size(); ++i) {
      task<result_t> t = task<result_t>::from_address(wrapped[i].address());
      t.promise().continuation_executor = e;
    }
    return *this;
  }
  template <detail::TypeErasableExecutor Exec>
  aw_task_many &resume_on(Exec &executor) {
    return resume_on(executor.type_erased());
  }
  template <detail::TypeErasableExecutor Exec>
  aw_task_many &resume_on(Exec *executor) {
    return resume_on(executor->type_erased());
  }
};

template <IsVoid result_t, size_t count> struct aw_task_many<result_t, count> {
  using wrapped_t = task<void>;
  static_assert(sizeof(wrapped_t) == sizeof(std::coroutine_handle<>));
  static_assert(alignof(wrapped_t) == alignof(std::coroutine_handle<>));
  using wrapped_arr_t = std::conditional_t<count == 0, std::vector<work_item>,
                                           std::array<work_item, count>>;
  wrapped_arr_t wrapped;
  std::coroutine_handle<> continuation;
  size_t prio;
  bool did_post;
  // written at completion
  std::atomic<int64_t> done_count;
  // For use when count is known at compile time
  template <typename Iter>
  aw_task_many(Iter task_iter, size_t prio_in, bool eager)
    requires(std::is_convertible_v<typename std::iter_value_t<Iter>, wrapped_t>)
      : prio(prio_in), did_post(eager) {
    const auto size = count;
    for (size_t i = 0; i < size; ++i) {
      wrapped_t t = *task_iter;
      auto &p = t.promise();
      p.continuation = &continuation;
      p.done_count = &done_count;
      wrapped[i] = std::coroutine_handle<>(t);
      ++task_iter;
    }
    if (eager) {
      done_count.store(size, std::memory_order_release);
      detail::this_thread::executor->post_bulk(wrapped.data(), prio, size);
    } else {
      done_count.store(size - 1, std::memory_order_release);
    }
  }
  // For use when count is runtime dynamic
  template <typename Iter>
  aw_task_many(Iter task_iter, size_t prio_in, size_t count_in, bool eager)
    requires(std::is_convertible_v<typename std::iter_value_t<Iter>, wrapped_t>)
      : prio(prio_in), did_post(eager) {
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
    if (eager) {
      done_count.store(size, std::memory_order_release);
      detail::this_thread::executor->post_bulk(wrapped.data(), prio, size);
    } else {
      done_count.store(size - 1, std::memory_order_release);
    }
  }
  // For use when count is known at compile time
  template <typename Callable>
  aw_task_many(Callable *callable_in, size_t prio_in, bool eager)
    requires(!std::is_convertible_v<Callable, std::coroutine_handle<>> &&
             std::is_invocable_r_v<result_t, Callable>)
      : prio(prio_in), did_post(eager) {
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
    if (eager) {
      done_count.store(size, std::memory_order_release);
      detail::this_thread::executor->post_bulk(wrapped.data(), prio, size);
    } else {
      done_count.store(size - 1, std::memory_order_release);
    }
  }
  // For use when count is runtime dynamic
  template <typename Callable>
  aw_task_many(Callable *callable_in, size_t prio_in, size_t count_in,
               bool eager)
    requires(!std::is_convertible_v<Callable, std::coroutine_handle<>> &&
             std::is_invocable_r_v<result_t, Callable> && count == 0)
      : prio(prio_in), did_post(eager) {
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
    if (eager) {
      done_count.store(size, std::memory_order_release);
      detail::this_thread::executor->post_bulk(wrapped.data(), prio, size);
    } else {
      done_count.store(size - 1, std::memory_order_release);
    }
  }
  constexpr bool await_ready() const noexcept { return false; }

  std::coroutine_handle<>
  await_suspend(std::coroutine_handle<> outer) noexcept {
    continuation = outer;
    if (did_post) {
      auto remaining = done_count.fetch_sub(1, std::memory_order_acq_rel);
      if (remaining > 0) {
        return std::noop_coroutine();
      } else {
        return outer;
      }
    }
    did_post = true;
    // if the newly posted tasks are at least as high priority as the currently
    // running or next-running (yield requested) task, we can directly transfer
    // to one
    bool do_symmetric_transfer =
        prio <= detail::this_thread::this_task.yield_priority->load(
                    std::memory_order_acquire);
    auto postCount =
        do_symmetric_transfer ? wrapped.size() - 1 : wrapped.size();
    if (postCount != 0) {
      // TODO is release fence required here?
      detail::this_thread::executor->post_bulk(wrapped.data(), prio, postCount);
    }
    if (do_symmetric_transfer) {
// symmetric transfer to the last task IF it should run immediately
#if WORK_ITEM_IS(CORO)
      return wrapped.back();
#elif WORK_ITEM_IS(FUNCORO) || WORK_ITEM_IS(FUNCORO32)
      return wrapped.back().as_coroutine();
#elif WORK_ITEM_IS(FUNC)
      return *wrapped.back().template target<std::coroutine_handle<>>();
#endif
    } else {
      return std::noop_coroutine();
    }
  }

  constexpr void await_resume() const noexcept {}

  // automatic post without co_await IF the func doesn't return a value
  // for void result_t only
  ~aw_task_many() noexcept {
    if (!did_post) {
      detail::this_thread::executor->post_bulk(wrapped.data(), prio,
                                               wrapped.size());
    }
  }

  aw_task_many(const aw_task_many &) = delete;
  aw_task_many &operator=(const aw_task_many &) = delete;
  aw_task_many(aw_task_many &&other) = delete;
  // aw_task_many(aw_task_many&& other) {
  //   wrapped = std::move(other.wrapped);
  //   prio = other.prio;
  //   did_await = other.did_await;
  //   other.did_await = true; // prevent other from posting
  // }
  aw_task_many &operator=(aw_task_many &&other) = delete;
  // aw_task_many& operator=(aw_task_many&& other) {
  //   wrapped = std::move(other.wrapped);
  //   prio = other.prio;
  //   did_await = other.did_await;
  //   other.did_await = true; // prevent other from posting
  //   return *this;
  // }

  inline aw_task_many &resume_on(detail::type_erased_executor *e) {
    for (size_t i = 0; i < wrapped.size(); ++i) {
      task<result_t> t = task<result_t>::from_address(wrapped[i].address());
      t.promise().continuation_executor = e;
    }
    return *this;
  }
  template <detail::TypeErasableExecutor Exec>
  aw_task_many &resume_on(Exec &executor) {
    return resume_on(executor.type_erased());
  }
  template <detail::TypeErasableExecutor Exec>
  aw_task_many &resume_on(Exec *executor) {
    return resume_on(executor->type_erased());
  }
};

// Lazy spawn
// For use when count is known at compile time
template <size_t count, typename Iter,
          typename result_t = std::iter_value_t<Iter>::result_type>
aw_task_many<result_t, count> spawn_many(Iter t)
  requires(std::is_convertible_v<std::iter_value_t<Iter>, task<result_t>>)
{
  static_assert(count != 0);
  return aw_task_many<result_t, count>(t, detail::this_thread::this_task.prio,
                                       false);
}

template <size_t count, typename Iter,
          typename result_t = std::iter_value_t<Iter>::result_type>
aw_task_many<result_t, count> spawn_many(Iter t, size_t prio)
  requires(std::is_convertible_v<std::iter_value_t<Iter>, task<result_t>>)
{
  static_assert(count != 0);
  return aw_task_many<result_t, count>(t, prio, false);
}

template <size_t count, typename result_t, typename Callable>
aw_task_many<result_t, count> spawn_many(Callable *c)
  requires(!std::is_convertible_v<Callable, std::coroutine_handle<>> &&
           std::is_invocable_r_v<result_t, Callable>)
{
  static_assert(count != 0);
  return aw_task_many<result_t, count>(c, detail::this_thread::this_task.prio,
                                       false);
}

template <size_t count, typename result_t, typename Callable>
aw_task_many<result_t, count> spawn_many(Callable *c, size_t prio)
  requires(!std::is_convertible_v<Callable, std::coroutine_handle<>> &&
           std::is_invocable_r_v<result_t, Callable>)
{
  static_assert(count != 0);
  return aw_task_many<result_t, count>(c, prio, false);
}

// For use when count is a runtime parameter
template <typename Iter,
          typename result_t = std::iter_value_t<Iter>::result_type>
aw_task_many<result_t, 0> spawn_many(Iter t, size_t count)
  requires(std::is_convertible_v<std::iter_value_t<Iter>, task<result_t>>)
{
  return aw_task_many<result_t, 0>(t, detail::this_thread::this_task.prio,
                                   count, false);
}

template <typename Iter,
          typename result_t = std::iter_value_t<Iter>::result_type>
aw_task_many<result_t, 0> spawn_many(Iter t, size_t prio, size_t count)
  requires(std::is_convertible_v<std::iter_value_t<Iter>, task<result_t>>)
{
  return aw_task_many<result_t, 0>(t, prio, count, false);
}

template <typename result_t, typename Callable>
aw_task_many<result_t, 0> spawn_many(Callable *c, size_t count)
  requires(!std::is_convertible_v<Callable, std::coroutine_handle<>> &&
           std::is_invocable_r_v<result_t, Callable>)
{
  return aw_task_many<result_t, 0>(c, detail::this_thread::this_task.prio,
                                   count, false);
}

template <typename result_t, typename Callable>
aw_task_many<result_t, 0> spawn_many(Callable *c, size_t prio, size_t count)
  requires(!std::is_convertible_v<Callable, std::coroutine_handle<>> &&
           std::is_invocable_r_v<result_t, Callable>)
{
  return aw_task_many<result_t, 0>(c, prio, count, false);
}

// Eager spawn
// For use when count is known at compile time
template <size_t count, typename Iter,
          typename result_t = std::iter_value_t<Iter>::result_type>
[[nodiscard(
    "You must co_await the return of "
    "spawn_many_early(). It is not safe to destroy aw_early_many_task prior to "
    "awaiting it.")]] aw_task_many<result_t, count>
spawn_many_early(Iter t)
  requires(std::is_convertible_v<std::iter_value_t<Iter>, task<result_t>>)
{
  static_assert(count != 0);
  return aw_task_many<result_t, count>(t, detail::this_thread::this_task.prio,
                                       true);
}

template <size_t count, typename Iter,
          typename result_t = std::iter_value_t<Iter>::result_type>
[[nodiscard(
    "You must co_await the return of "
    "spawn_many_early(). It is not safe to destroy aw_early_many_task prior to "
    "awaiting it.")]] aw_task_many<result_t, count>
spawn_many_early(Iter t, size_t prio)
  requires(std::is_convertible_v<std::iter_value_t<Iter>, task<result_t>>)
{
  static_assert(count != 0);
  return aw_task_many<result_t, count>(t, prio, true);
}

template <size_t count, typename result_t, typename Callable>
[[nodiscard(
    "You must co_await the return of "
    "spawn_many_early(). It is not safe to destroy aw_early_many_task prior to "
    "awaiting it.")]] aw_task_many<result_t, count>
spawn_many_early(Callable *c)
  requires(!std::is_convertible_v<Callable, std::coroutine_handle<>> &&
           std::is_invocable_r_v<result_t, Callable>)
{
  static_assert(count != 0);
  return aw_task_many<result_t, count>(c, detail::this_thread::this_task.prio,
                                       true);
}

template <size_t count, typename result_t, typename Callable>
[[nodiscard(
    "You must co_await the return of "
    "spawn_many_early(). It is not safe to destroy aw_early_many_task prior to "
    "awaiting it.")]] aw_task_many<result_t, count>
spawn_many_early(Callable *c, size_t prio)
  requires(!std::is_convertible_v<Callable, std::coroutine_handle<>> &&
           std::is_invocable_r_v<result_t, Callable>)
{
  static_assert(count != 0);
  return aw_task_many<result_t, count>(c, prio, true);
}

// For use when count is a runtime parameter
template <typename Iter,
          typename result_t = std::iter_value_t<Iter>::result_type>
[[nodiscard(
    "You must co_await the return of "
    "spawn_many_early(). It is not safe to destroy aw_early_many_task prior to "
    "awaiting it.")]] aw_task_many<result_t, 0>
spawn_many_early(Iter t, size_t count)
  requires(std::is_convertible_v<std::iter_value_t<Iter>, task<result_t>>)
{
  return aw_task_many<result_t, 0>(t, detail::this_thread::this_task.prio,
                                   count, true);
}

template <typename Iter,
          typename result_t = std::iter_value_t<Iter>::result_type>
[[nodiscard(
    "You must co_await the return of "
    "spawn_many_early(). It is not safe to destroy aw_early_many_task prior to "
    "awaiting it.")]] aw_task_many<result_t, 0>
spawn_many_early(Iter t, size_t prio, size_t count)
  requires(std::is_convertible_v<std::iter_value_t<Iter>, task<result_t>>)
{
  return aw_task_many<result_t, 0>(t, prio, count, true);
}

template <typename result_t, typename Callable>
[[nodiscard(
    "You must co_await the return of "
    "spawn_many_early(). It is not safe to destroy aw_early_many_task prior to "
    "awaiting it.")]] aw_task_many<result_t, 0>
spawn_many_early(Callable *c, size_t count)
  requires(!std::is_convertible_v<Callable, std::coroutine_handle<>> &&
           std::is_invocable_r_v<result_t, Callable>)
{
  return aw_task_many<result_t, 0>(c, detail::this_thread::this_task.prio,
                                   count, true);
}

template <typename result_t, typename Callable>
[[nodiscard(
    "You must co_await the return of "
    "spawn_many_early(). It is not safe to destroy aw_early_many_task prior to "
    "awaiting it.")]] aw_task_many<result_t, 0>
spawn_many_early(Callable *c, size_t prio, size_t count)
  requires(!std::is_convertible_v<Callable, std::coroutine_handle<>> &&
           std::is_invocable_r_v<result_t, Callable>)
{
  return aw_task_many<result_t, 0>(c, prio, count, true);
}

} // namespace tmc
