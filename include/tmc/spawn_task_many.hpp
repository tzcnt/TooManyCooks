#pragma once
#include "tmc/detail/aw_run_early.hpp"
#include "tmc/detail/concepts.hpp"
#include "tmc/detail/thread_locals.hpp"
#include "tmc/task.hpp"
#include <atomic>
#include <cassert>
#include <coroutine>
#include <functional>
#include <iterator>
#include <mutex>

namespace tmc {

// For use when count is known at compile time
template <
  size_t count, typename Iter,
  typename result_t = std::iter_value_t<Iter>::result_type>
aw_task_many<result_t, count> spawn_many(Iter t)
  requires(std::is_convertible_v<std::iter_value_t<Iter>, task<result_t>>)
{
  static_assert(count != 0);
  return aw_task_many<result_t, count>(t);
}

template <size_t count, typename result_t, typename Callable>
aw_task_many<result_t, count> spawn_many(Callable* c)
  requires(!std::is_convertible_v<Callable, std::coroutine_handle<>> && std::is_invocable_r_v<result_t, Callable>)
{
  static_assert(count != 0);
  return aw_task_many<result_t, count>(c);
}

// For use when count is a runtime parameter
template <
  typename Iter, typename result_t = std::iter_value_t<Iter>::result_type>
aw_task_many<result_t, 0> spawn_many(Iter t, size_t count)
  requires(std::is_convertible_v<std::iter_value_t<Iter>, task<result_t>>)
{
  return aw_task_many<result_t, 0>(t, count);
}

template <typename result_t, typename Callable>
aw_task_many<result_t, 0> spawn_many(Callable* c, size_t count)
  requires(!std::is_convertible_v<Callable, std::coroutine_handle<>> && std::is_invocable_r_v<result_t, Callable>)
{
  return aw_task_many<result_t, 0>(c, count);
}

// Primary template is forward-declared in "tmc/detail/aw_run_early.hpp".
template <IsNotVoid result_t, size_t count>
class aw_task_many<result_t, count> {
  using wrapped_t = task<result_t>;
  static_assert(sizeof(wrapped_t) == sizeof(std::coroutine_handle<>));
  static_assert(alignof(wrapped_t) == alignof(std::coroutine_handle<>));
  using wrapped_arr_t = std::conditional_t<
    count == 0, std::vector<work_item>, std::array<work_item, count>>;
  using result_arr_t = std::conditional_t<
    count == 0, std::vector<result_t>, std::array<result_t, count>>;
  friend class aw_run_early<result_t, result_arr_t>;
  wrapped_arr_t wrapped;
  std::coroutine_handle<> continuation;
  detail::type_erased_executor* executor;
  detail::type_erased_executor* continuation_executor;
  size_t prio;
  bool did_await;
  result_arr_t result;
  std::atomic<int64_t> done_count;

public:
  // For use when count is known at compile time

  /// It is recommended to call `spawn_many()` instead of using this constructor
  /// directly.
  template <typename Iter>
  aw_task_many(Iter task_iter)
    requires(std::is_convertible_v<typename std::iter_value_t<Iter>, wrapped_t>)
      : executor(detail::this_thread::executor),
        continuation_executor(detail::this_thread::executor),
        prio(detail::this_thread::this_task.prio), did_await(false) {
    const auto size = count;
    for (size_t i = 0; i < size; ++i) {
      // TODO capture an iter_adapter that applies these transformations instead
      // of the whole wrapped arr -- see note at end of file
      // If this is lazily evaluated only when the tasks are posted, this class
      // can be made movable
      wrapped_t t = *task_iter;
      auto& p = t.promise();
      p.continuation = &continuation;
      p.continuation_executor = &continuation_executor;
      p.done_count = &done_count;
      p.result_ptr = &result[i];
      wrapped[i] = std::coroutine_handle<>(t);
      ++task_iter;
    }
    done_count.store(size - 1, std::memory_order_release);
  }

  // For use when count is runtime dynamic

  /// It is recommended to call `spawn_many()` instead of using this constructor
  /// directly.
  template <typename Iter>
  aw_task_many(Iter task_iter, size_t count_in)
    requires(count == 0 &&
             std::is_convertible_v<typename std::iter_value_t<Iter>, wrapped_t>)
      : executor(detail::this_thread::executor),
        continuation_executor(detail::this_thread::executor),
        prio(detail::this_thread::this_task.prio), did_await(false) {
    const auto size = count_in;
    wrapped.resize(size);
    result.resize(size);
    for (size_t i = 0; i < size; ++i) {
      wrapped_t t = *task_iter;
      auto& p = t.promise();
      p.continuation = &continuation;
      p.continuation_executor = &continuation_executor;
      p.done_count = &done_count;
      p.result_ptr = &result[i];
      wrapped[i] = std::coroutine_handle<>(t);
      ++task_iter;
    }
    done_count.store(size - 1, std::memory_order_release);
  }

  // For use when count is known at compile time

  /// It is recommended to call `spawn_many()` instead of using this constructor
  /// directly.
  template <typename Callable>
  aw_task_many(Callable* callable_in)
    requires(!std::is_convertible_v<Callable, std::coroutine_handle<>> &&
             std::is_invocable_r_v<result_t, Callable>)
      : executor(detail::this_thread::executor),
        continuation_executor(detail::this_thread::executor),
        prio(detail::this_thread::this_task.prio), did_await(false) {
    const auto size = count;
    for (size_t i = 0; i < size; ++i) {
      wrapped_t t = [](Callable callable) -> wrapped_t {
        co_return callable();
      }(callable_in[i]);
      auto& p = t.promise();
      p.continuation = &continuation;
      p.continuation_executor = &continuation_executor;
      p.done_count = &done_count;
      p.result_ptr = &result[i];
      wrapped[i] = std::coroutine_handle<>(t);
    }
    done_count.store(size - 1, std::memory_order_release);
  }

  // For use when count is runtime dynamic

  /// It is recommended to call `spawn_many()` instead of using this constructor
  /// directly.
  template <typename Callable>
  aw_task_many(Callable* callable_in, size_t count_in)
    requires(!std::is_convertible_v<Callable, std::coroutine_handle<>> &&
             std::is_invocable_r_v<result_t, Callable> && count == 0)
      : executor(detail::this_thread::executor),
        continuation_executor(detail::this_thread::executor),
        prio(detail::this_thread::this_task.prio), did_await(false) {
    const auto size = count_in;
    wrapped.resize(size);
    result.resize(size);
    for (size_t i = 0; i < size; ++i) {
      wrapped_t t = [](Callable callable) -> wrapped_t {
        co_return callable();
      }(callable_in[i]);
      auto& p = t.promise();
      p.continuation = &continuation;
      p.continuation_executor = &continuation_executor;
      p.done_count = &done_count;
      p.result_ptr = &result[i];
      wrapped[i] = std::coroutine_handle<>(t);
    }
    done_count.store(size - 1, std::memory_order_release);
  }

  constexpr bool await_ready() const noexcept { return false; }

  std::coroutine_handle<> await_suspend(std::coroutine_handle<> outer
  ) noexcept {
    continuation = outer;
    assert(!did_await);
    did_await = true;
    // if the newly posted tasks are at least as high priority as the currently
    // running or next-running (yield requested) task, we can directly transfer
    // to one
    bool do_symmetric_transfer =
      prio <= detail::this_thread::this_task.yield_priority->load(
                std::memory_order_acquire
              ) &&
      executor == detail::this_thread::executor;
    auto postCount =
      do_symmetric_transfer ? wrapped.size() - 1 : wrapped.size();
    if (postCount != 0) {
      // TODO is release fence required here?
      executor->post_bulk(wrapped.data(), prio, postCount);
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

  result_arr_t& await_resume() & noexcept { return result; }
  result_arr_t&& await_resume() && noexcept { return std::move(result); }

  ~aw_task_many() noexcept {
    // If you spawn a function that returns a non-void type,
    // then you must co_await the return of spawn!
    assert(did_await);
  }
  aw_task_many(const aw_task_many&) = delete;
  aw_task_many& operator=(const aw_task_many&) = delete;
  aw_task_many(aw_task_many&& other) = delete;
  // aw_task_many(aw_task_many&& other) {
  //   executor = std::move(other.executor);
  //   continuation = std::move(other.continuation);
  //   wrapped = std::move(other.wrapped);
  //   result = std::move(other.result);
  //   prio = other.prio;
  //   did_await = other.did_await;
  //   other.did_await = true; // prevent other from posting
  // }
  aw_task_many& operator=(aw_task_many&& other) = delete;
  // aw_task_many& operator=(aw_task_many&& other) {
  //   executor = std::move(other.executor);
  //   continuation = std::move(other.continuation);
  //   wrapped = std::move(other.wrapped);
  //   result = std::move(other.result);
  //   prio = other.prio;
  //   did_await = other.did_await;
  //   other.did_await = true; // prevent other from posting
  //   return *this;
  // }

  inline aw_task_many& resume_on(detail::type_erased_executor* e) {
    continuation_executor = e;
    return *this;
  }
  template <detail::TypeErasableExecutor Exec>
  aw_task_many& resume_on(Exec& executor) {
    return resume_on(executor.type_erased());
  }
  template <detail::TypeErasableExecutor Exec>
  aw_task_many& resume_on(Exec* executor) {
    return resume_on(executor->type_erased());
  }

  inline aw_task_many& run_on(detail::type_erased_executor* e) {
    executor = e;
    return *this;
  }
  template <detail::TypeErasableExecutor Exec>
  aw_task_many& run_on(Exec& executor) {
    return run_on(executor.type_erased());
  }
  template <detail::TypeErasableExecutor Exec>
  aw_task_many& run_on(Exec* executor) {
    return run_on(executor->type_erased());
  }

  inline aw_task_many& with_priority(size_t priority) {
    prio = priority;
    return *this;
  }

  inline aw_run_early<result_t, result_arr_t> run_early() {
    return aw_run_early<result_t, result_arr_t>(std::move(*this));
  }
};

template <IsVoid result_t, size_t count> class aw_task_many<result_t, count> {
  using wrapped_t = task<void>;
  static_assert(sizeof(wrapped_t) == sizeof(std::coroutine_handle<>));
  static_assert(alignof(wrapped_t) == alignof(std::coroutine_handle<>));
  using wrapped_arr_t = std::conditional_t<
    count == 0, std::vector<work_item>, std::array<work_item, count>>;
  friend class aw_run_early<result_t, void>;
  wrapped_arr_t wrapped;
  std::coroutine_handle<> continuation;
  detail::type_erased_executor* executor;
  detail::type_erased_executor* continuation_executor;
  size_t prio;
  bool did_await;
  std::atomic<int64_t> done_count;

public:
  // For use when count is known at compile time

  /// It is recommended to call `spawn_many()` instead of using this constructor
  /// directly.
  template <typename Iter>
  aw_task_many(Iter task_iter)
    requires(std::is_convertible_v<typename std::iter_value_t<Iter>, wrapped_t>)
      : executor(detail::this_thread::executor),
        continuation_executor(detail::this_thread::executor),
        prio(detail::this_thread::this_task.prio), did_await(false) {
    const auto size = count;
    for (size_t i = 0; i < size; ++i) {
      wrapped_t t = *task_iter;
      auto& p = t.promise();
      p.continuation = &continuation;
      p.continuation_executor = &continuation_executor;
      p.done_count = &done_count;
      wrapped[i] = std::coroutine_handle<>(t);
      ++task_iter;
    }
    done_count.store(size - 1, std::memory_order_release);
  }

  // For use when count is runtime dynamic

  /// It is recommended to call `spawn_many()` instead of using this constructor
  /// directly.
  template <typename Iter>
  aw_task_many(Iter task_iter, size_t count_in)
    requires(std::is_convertible_v<typename std::iter_value_t<Iter>, wrapped_t>)
      : executor(detail::this_thread::executor),
        continuation_executor(detail::this_thread::executor),
        prio(detail::this_thread::this_task.prio), did_await(false) {
    const auto size = count_in;
    wrapped.resize(size);
    for (size_t i = 0; i < size; ++i) {
      wrapped_t t = *task_iter;
      auto& p = t.promise();
      p.continuation = &continuation;
      p.continuation_executor = &continuation_executor;
      p.done_count = &done_count;
      wrapped[i] = std::coroutine_handle<>(t);
      ++task_iter;
    }
    done_count.store(size - 1, std::memory_order_release);
  }

  // For use when count is known at compile time

  /// It is recommended to call `spawn_many()` instead of using this constructor
  /// directly.
  template <typename Callable>
  aw_task_many(Callable* callable_in)
    requires(!std::is_convertible_v<Callable, std::coroutine_handle<>> &&
             std::is_invocable_r_v<result_t, Callable>)
      : executor(detail::this_thread::executor),
        continuation_executor(detail::this_thread::executor),
        prio(detail::this_thread::this_task.prio), did_await(false) {
    const auto size = count;
    for (size_t i = 0; i < size; ++i) {
      wrapped_t t = [](Callable callable) -> wrapped_t {
        callable();
        co_return;
      }(callable_in[i]);
      auto& p = t.promise();
      p.continuation = &continuation;
      p.continuation_executor = &continuation_executor;
      p.done_count = &done_count;
      wrapped[i] = std::coroutine_handle<>(t);
    }
    done_count.store(size - 1, std::memory_order_release);
  }

  // For use when count is runtime dynamic

  /// It is recommended to call `spawn_many()` instead of using this constructor
  /// directly.
  template <typename Callable>
  aw_task_many(Callable* callable_in, size_t count_in)
    requires(!std::is_convertible_v<Callable, std::coroutine_handle<>> &&
             std::is_invocable_r_v<result_t, Callable> && count == 0)
      : executor(detail::this_thread::executor),
        continuation_executor(detail::this_thread::executor),
        prio(detail::this_thread::this_task.prio), did_await(false) {
    const auto size = count_in;
    wrapped.resize(size);
    for (size_t i = 0; i < size; ++i) {
      wrapped_t t = [](Callable callable) -> wrapped_t {
        callable();
        co_return;
      }(callable_in[i]);
      auto& p = t.promise();
      p.continuation = &continuation;
      p.continuation_executor = &continuation_executor;
      p.done_count = &done_count;
      wrapped[i] = std::coroutine_handle<>(t);
    }
    done_count.store(size - 1, std::memory_order_release);
  }

  constexpr bool await_ready() const noexcept { return false; }

  std::coroutine_handle<> await_suspend(std::coroutine_handle<> outer
  ) noexcept {
    continuation = outer;
    assert(!did_await);
    did_await = true;
    // if the newly posted tasks are at least as high priority as the currently
    // running or next-running (yield requested) task, we can directly transfer
    // to one
    bool do_symmetric_transfer =
      prio <= detail::this_thread::this_task.yield_priority->load(
                std::memory_order_acquire
              ) &&
      executor == detail::this_thread::executor;
    auto postCount =
      do_symmetric_transfer ? wrapped.size() - 1 : wrapped.size();
    if (postCount != 0) {
      // TODO is release fence required here?
      executor->post_bulk(wrapped.data(), prio, postCount);
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
    if (!did_await) {
      executor->post_bulk(wrapped.data(), prio, wrapped.size());
    }
  }

  aw_task_many(const aw_task_many&) = delete;
  aw_task_many& operator=(const aw_task_many&) = delete;
  aw_task_many(aw_task_many&& other) = delete;
  // aw_task_many(aw_task_many&& other) {
  //   executor = std::move(other.executor);
  //   continuation = std::move(other.continuation);
  //   wrapped = std::move(other.wrapped);
  //   prio = other.prio;
  //   did_await = other.did_await;
  //   other.did_await = true; // prevent other from posting
  // }
  aw_task_many& operator=(aw_task_many&& other) = delete;
  // aw_task_many& operator=(aw_task_many&& other) {
  //   executor = std::move(other.executor);
  //   continuation = std::move(other.continuation);
  //   wrapped = std::move(other.wrapped);
  //   prio = other.prio;
  //   did_await = other.did_await;
  //   other.did_await = true; // prevent other from posting
  //   return *this;
  // }

  inline aw_task_many& resume_on(detail::type_erased_executor* e) {
    continuation_executor = e;
    return *this;
  }
  template <detail::TypeErasableExecutor Exec>
  aw_task_many& resume_on(Exec& executor) {
    return resume_on(executor.type_erased());
  }
  template <detail::TypeErasableExecutor Exec>
  aw_task_many& resume_on(Exec* executor) {
    return resume_on(executor->type_erased());
  }

  inline aw_task_many& run_on(detail::type_erased_executor* e) {
    executor = e;
    return *this;
  }
  template <detail::TypeErasableExecutor Exec>
  aw_task_many& run_on(Exec& executor) {
    return run_on(executor.type_erased());
  }
  template <detail::TypeErasableExecutor Exec>
  aw_task_many& run_on(Exec* executor) {
    return run_on(executor->type_erased());
  }

  inline aw_task_many& with_priority(size_t priority) {
    prio = priority;
    return *this;
  }

  inline aw_run_early<result_t, void> run_early() {
    return aw_run_early<result_t, void>(std::move(*this));
  }
};

} // namespace tmc

// some ways to capture transformer iterator instead of entire wrapper
// (and reduce size of this struct)
// causes problems with CTAD; need to split callable and non-callable versions
// template <typename InputIter> struct TaskIterTransformer {
//     InputIter task_iter;
//     aw_task_many *me;
//     std::coroutine_handle<> operator()(size_t i) {
//       wrapped_t t = *task_iter;
//       ++task_iter;
//       auto &p = t.promise();
//       p.continuation = &me->continuation;
//       p.done_count = &me->done_count;
//       p.result_ptr = &me->result[i];
//       return std::coroutine_handle<>(t);
//     }
//   };
// iter = iter_adapter(0, TaskIterTransformer{std::move(task_iter), this});
// iter = [this, task_iter = std::move(task_iter)](
//     size_t i) -> std::coroutine_handle<> {
//   wrapped_t t = *task_iter;
//   ++task_iter;
//   auto &p = t.promise();
//   p.continuation = &continuation;
//   p.done_count = &done_count;
//   p.result_ptr = &result[i];
//   return std::coroutine_handle<>(t);
// });
