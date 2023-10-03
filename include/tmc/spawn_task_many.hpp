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
template <typename Iter, typename result_t, size_t count> struct aw_task_many;
template <typename Iter, IsNotVoid result_t, size_t count>
struct aw_task_many<Iter, result_t, count> {
  using wrapped_t = task<result_t>;
  static_assert(sizeof(wrapped_t) == sizeof(std::coroutine_handle<>));
  static_assert(alignof(wrapped_t) == alignof(std::coroutine_handle<>));
  using result_arr_t = std::conditional_t<count == 0, std::vector<result_t>,
                                          std::array<result_t, count>>;
  // written at initiation
  Iter input_iter;
  std::coroutine_handle<> continuation;
  detail::type_erased_executor *executor;
  detail::type_erased_executor *continuation_executor;
  size_t prio;
  bool did_await;
  // written at completion
  result_arr_t result;
  std::atomic<int64_t> done_count;
  // For use when count is known at compile time
  aw_task_many(Iter task_iter)
    requires(std::is_convertible_v<typename std::iter_value_t<Iter>, wrapped_t>)
      : input_iter(task_iter), executor(detail::this_thread::executor),
        continuation_executor(detail::this_thread::executor),
        prio(detail::this_thread::this_task.prio), did_await(false) {
    const auto size = count;
    done_count.store(size - 1, std::memory_order_release);
  }
  // For use when count is runtime dynamic
  aw_task_many(Iter task_iter, size_t count_in)
    requires(count == 0 &&
             std::is_convertible_v<typename std::iter_value_t<Iter>, wrapped_t>)
      : input_iter(task_iter), executor(detail::this_thread::executor),
        continuation_executor(detail::this_thread::executor),
        prio(detail::this_thread::this_task.prio), did_await(false) {
    const auto size = count_in;
    result.resize(size);
    done_count.store(size - 1, std::memory_order_release);
  }

  struct TaskIterTransformer {
    using value_type = std::coroutine_handle<>;
    Iter task_iter;
    aw_task_many *me;
    size_t i;
    std::coroutine_handle<> operator*() {
      wrapped_t t = *task_iter;
      auto &p = t.promise();
      p.continuation = &me->continuation;
      p.done_count = &me->done_count;
      p.continuation_executor = me->continuation_executor;
      p.result_ptr = &me->result[i];
      return std::coroutine_handle<>(t);
    }
    auto &operator++() {
      ++task_iter;
      ++i;
      return *this;
    }
  };

  constexpr bool await_ready() const noexcept { return false; }

  std::coroutine_handle<>
  await_suspend(std::coroutine_handle<> outer) noexcept {
    continuation = outer;
    did_await = true;
    // if the newly posted tasks are at least as high priority as the currently
    // running or next-running (yield requested) task, we can directly transfer
    // to one
    TaskIterTransformer output_iter{input_iter, this, 0};
    bool do_symmetric_transfer =
        executor == detail::this_thread::executor &&
        prio <= detail::this_thread::this_task.yield_priority->load(
                    std::memory_order_acquire);
    auto size = done_count.load(std::memory_order_acquire) + 1;
    auto postCount = do_symmetric_transfer ? size - 1 : size;
    // Submitting templated iterator type to type-erased executor is hard to do.
    // For now, just buffer and submit work items in batches.
    {
      std::array<std::coroutine_handle<>, 64> work_items;
      while (postCount > 0) {
        size_t i = 0;
        while (i < 64 && i < postCount) {
          work_items[i] = *output_iter;
          ++output_iter;
          ++i;
        }
        executor->post_bulk(work_items.data(), prio, i);
        postCount -= i;
      }
    }
    if (do_symmetric_transfer) {
      // symmetric transfer to the last task IF it should run immediately
#if WORK_ITEM_IS(CORO)
      return *output_iter;
#elif WORK_ITEM_IS(FUNCORO) || WORK_ITEM_IS(FUNCORO32)
      return output_iter->as_coroutine();
#elif WORK_ITEM_IS(FUNC)
      return *output_iter->template target<std::coroutine_handle<>>();
#endif
    } else {
      return std::noop_coroutine();
    }
  }

  result_arr_t &await_resume() & noexcept { return result; }
  result_arr_t &&await_resume() && noexcept { return std::move(result); }

  ~aw_task_many() noexcept {
    // If you spawn a function that returns a non-void type,
    // then you must co_await the return of spawn!
    assert(did_await);
  }
  aw_task_many(const aw_task_many &) = delete;
  aw_task_many &operator=(const aw_task_many &) = delete;
  aw_task_many(aw_task_many &&other) = delete;
  // aw_task_many(aw_task_many&& other) {
  //   executor = std::move(other.executor);
  //   continuation = std::move(other.continuation);
  //   wrapped = std::move(other.wrapped);
  //   result = std::move(other.result);
  //   prio = other.prio;
  //   did_await = other.did_await;
  //   other.did_await = true; // prevent other from posting
  // }
  aw_task_many &operator=(aw_task_many &&other) = delete;
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

  inline aw_task_many &resume_on(detail::type_erased_executor *e) {
    continuation_executor = e;
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

  inline aw_task_many &run_on(detail::type_erased_executor *e) {
    executor = e;
    return *this;
  }
  template <detail::TypeErasableExecutor Exec>
  aw_task_many &run_on(Exec &executor) {
    return run_on(executor.type_erased());
  }
  template <detail::TypeErasableExecutor Exec>
  aw_task_many &run_on(Exec *executor) {
    return run_on(executor->type_erased());
  }

  inline aw_task_many &with_priority(size_t priority) {
    prio = priority;
    return *this;
  }
};

template <typename Iter, IsVoid result_t, size_t count>
struct aw_task_many<Iter, result_t, count> {
  using wrapped_t = task<void>;
  static_assert(sizeof(wrapped_t) == sizeof(std::coroutine_handle<>));
  static_assert(alignof(wrapped_t) == alignof(std::coroutine_handle<>));
  Iter input_iter;
  std::coroutine_handle<> continuation;
  detail::type_erased_executor *executor;
  detail::type_erased_executor *continuation_executor;
  size_t prio;
  bool did_await;
  // written at completion
  std::atomic<int64_t> done_count;
  // For use when count is known at compile time
  aw_task_many(Iter task_iter)
    requires(std::is_convertible_v<typename std::iter_value_t<Iter>, wrapped_t>)
      : input_iter(task_iter), executor(detail::this_thread::executor),
        continuation_executor(detail::this_thread::executor),
        prio(detail::this_thread::this_task.prio), did_await(false) {
    const auto size = count;
    done_count.store(size - 1, std::memory_order_release);
  }
  // For use when count is runtime dynamic
  aw_task_many(Iter task_iter, size_t count_in)
    requires(std::is_convertible_v<typename std::iter_value_t<Iter>, wrapped_t>)
      : input_iter(task_iter), executor(detail::this_thread::executor),
        continuation_executor(detail::this_thread::executor),
        prio(detail::this_thread::this_task.prio), did_await(false) {
    const auto size = count_in;
    done_count.store(size - 1, std::memory_order_release);
  }

  struct TaskIterTransformer {
    using value_type = std::coroutine_handle<>;
    Iter task_iter;
    aw_task_many *me;
    value_type operator*() {
      wrapped_t t = *task_iter;
      auto &p = t.promise();
      p.continuation = &me->continuation;
      p.continuation_executor = me->continuation_executor;
      p.done_count = &me->done_count;
      return std::coroutine_handle<>(t);
    }
    auto &operator++() {
      ++task_iter;
      return *this;
    }
  };

  constexpr bool await_ready() const noexcept { return false; }

  std::coroutine_handle<>
  await_suspend(std::coroutine_handle<> outer) noexcept {
    continuation = outer;
    did_await = true;
    // if the newly posted tasks are at least as high priority as the currently
    // running or next-running (yield requested) task, we can directly transfer
    // to one
    TaskIterTransformer output_iter{input_iter, this};
    bool do_symmetric_transfer =
        executor == detail::this_thread::executor &&
        prio <= detail::this_thread::this_task.yield_priority->load(
                    std::memory_order_acquire);
    auto size = done_count.load(std::memory_order_acquire) + 1;
    auto postCount = do_symmetric_transfer ? size - 1 : size;
    // Submitting templated iterator type to type-erased executor is hard to do.
    // For now, just buffer and submit work items in batches.
    {
      std::array<std::coroutine_handle<>, 64> work_items;
      while (postCount > 0) {
        size_t i = 0;
        while (i < 64 && i < postCount) {
          work_items[i] = *output_iter;
          ++output_iter;
          ++i;
        }
        executor->post_bulk(work_items.data(), prio, i);
        postCount -= i;
      }
    }
    if (do_symmetric_transfer) {
// symmetric transfer to the last task IF it should run immediately
#if WORK_ITEM_IS(CORO)
      return *output_iter;
#elif WORK_ITEM_IS(FUNCORO) || WORK_ITEM_IS(FUNCORO32)
      return output_iter->as_coroutine();
#elif WORK_ITEM_IS(FUNC)
      return *output_iter->template target<std::coroutine_handle<>>();
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
      auto size = done_count.load(std::memory_order_acquire) + 1;
      TaskIterTransformer output_iter{input_iter, this};
      std::array<std::coroutine_handle<>, 64> work_items;
      while (size > 0) {
        size_t i = 0;
        while (i < 64 && i < size) {
          work_items[i] = *output_iter;
          ++output_iter;
          ++i;
        }
        executor->post_bulk(work_items.data(), prio, i);
        size -= i;
      }
    }
  }

  aw_task_many(const aw_task_many &) = delete;
  aw_task_many &operator=(const aw_task_many &) = delete;
  aw_task_many(aw_task_many &&other) = delete;
  // aw_task_many(aw_task_many&& other) {
  //   executor = std::move(other.executor);
  //   continuation = std::move(other.continuation);
  //   wrapped = std::move(other.wrapped);
  //   prio = other.prio;
  //   did_await = other.did_await;
  //   other.did_await = true; // prevent other from posting
  // }
  aw_task_many &operator=(aw_task_many &&other) = delete;
  // aw_task_many& operator=(aw_task_many&& other) {
  //   executor = std::move(other.executor);
  //   continuation = std::move(other.continuation);
  //   wrapped = std::move(other.wrapped);
  //   prio = other.prio;
  //   did_await = other.did_await;
  //   other.did_await = true; // prevent other from posting
  //   return *this;
  // }

  inline aw_task_many &resume_on(detail::type_erased_executor *e) {
    continuation_executor = e;
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

  inline aw_task_many &run_on(detail::type_erased_executor *e) {
    executor = e;
    return *this;
  }
  template <detail::TypeErasableExecutor Exec>
  aw_task_many &run_on(Exec &executor) {
    return run_on(executor.type_erased());
  }
  template <detail::TypeErasableExecutor Exec>
  aw_task_many &run_on(Exec *executor) {
    return run_on(executor->type_erased());
  }

  inline aw_task_many &with_priority(size_t priority) {
    prio = priority;
    return *this;
  }
};

// Lazy spawn
// For use when count is known at compile time
template <size_t count, typename Iter,
          typename result_t = std::iter_value_t<Iter>::result_type>
aw_task_many<Iter, result_t, count> spawn_many(Iter t)
  requires(std::is_convertible_v<std::iter_value_t<Iter>, task<result_t>>)
{
  static_assert(count != 0);
  return aw_task_many<Iter, result_t, count>(t);
}

// For use when count is a runtime parameter
template <typename Iter,
          typename result_t = std::iter_value_t<Iter>::result_type>
aw_task_many<Iter, result_t, 0> spawn_many(Iter t, size_t count)
  requires(std::is_convertible_v<std::iter_value_t<Iter>, task<result_t>>)
{
  return aw_task_many<Iter, result_t, 0>(t, count);
}

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
