#pragma once
#include "tmc/detail/aw_run_early.hpp"
#include "tmc/detail/concepts.hpp" // IWYU pragma: keep
#include "tmc/detail/thread_locals.hpp"
#include "tmc/task.hpp"
#include <array>
#include <atomic>
#include <cassert>
#include <coroutine>
#include <iterator>
#include <vector>

namespace tmc {

/// For use when the number of items to spawn is known at compile time.
/// `Count` must be non-zero.
/// `Iter` must be an iterator type that implements `operator*()` and
/// `Iter& operator++()`.
///
/// Submits `Count` items to the executor.
template <
  size_t Count, typename Iter,
  typename Result = typename std::iter_value_t<Iter>::result_type>
aw_task_many<Result, Count> spawn_many(Iter TaskIterator)
  requires(std::is_convertible_v<std::iter_value_t<Iter>, task<Result>>)
{
  static_assert(Count != 0);
  return aw_task_many<Result, Count>(TaskIterator);
}

/// For use when the number of items to spawn is known at compile time.
/// `Count` must be non-zero.
/// `Functor` must be a copyable type that implements `Result operator()`.
/// `FunctorIterator` must be a pointer to an array of `Functor`.
///
/// Submits `count` items to the executor.
template <size_t Count, typename Result, typename Functor>
aw_task_many<Result, Count> spawn_many(Functor* FunctorIterator)
  requires(!std::is_convertible_v<Functor, std::coroutine_handle<>> && std::is_invocable_r_v<Result, Functor>)
{
  static_assert(Count != 0);
  return aw_task_many<Result, Count>(FunctorIterator);
}

/// For use when the number of items to spawn is a runtime parameter.
/// `It` must be an iterator type that implements `operator*()` and
/// `It& operator++()`.
/// `TaskCount` must be non-zero.
///
/// Submits `count` items to the executor.
template <
  typename Iter,
  typename Result = typename std::iter_value_t<Iter>::result_type>
aw_task_many<Result, 0> spawn_many(Iter TaskIterator, size_t TaskCount)
  requires(std::is_convertible_v<std::iter_value_t<Iter>, task<Result>>)
{
  return aw_task_many<Result, 0>(TaskIterator, TaskCount);
}

/// For use when the number of items to spawn is a runtime parameter.
/// `Functor` must be a copyable type that implements `Result operator()`.
/// `FunctorIterator` must be a pointer to an array of `Functor`.
/// `FunctorCount` must be non-zero.
///
/// Submits `count` items to the executor.
template <typename Result, typename Functor>
aw_task_many<Result, 0>
spawn_many(Functor* FunctorIterator, size_t FunctorCount)
  requires(!std::is_convertible_v<Functor, std::coroutine_handle<>> && std::is_invocable_r_v<Result, Functor>)
{
  return aw_task_many<Result, 0>(FunctorIterator, FunctorCount);
}

// Primary template is forward-declared in "tmc/detail/aw_run_early.hpp".
template <IsNotVoid Result, size_t Count> class aw_task_many<Result, Count> {
  static_assert(sizeof(task<Result>) == sizeof(std::coroutine_handle<>));
  static_assert(alignof(task<Result>) == alignof(std::coroutine_handle<>));
  using WrappedArray = std::conditional_t<
    Count == 0, std::vector<work_item>, std::array<work_item, Count>>;

  /// If `Count` is a compile-time template argument, returns a
  /// `std::array<Result, Count>`. If `Count` is a runtime parameter, returns
  /// a `std::vector<Result>` of size `Count`;
  using ResultArray = std::conditional_t<
    Count == 0, std::vector<Result>, std::array<Result, Count>>;
  friend class aw_run_early<Result, ResultArray>;
  WrappedArray wrapped;
  std::coroutine_handle<> continuation;
  detail::type_erased_executor* executor;
  detail::type_erased_executor* continuation_executor;
  size_t prio;
  bool did_await;
  ResultArray result;
  std::atomic<int64_t> done_count;

public:
  /// For use when `Count` is known at compile time.
  /// It is recommended to call `spawn_many()` instead of using this constructor
  /// directly.
  template <typename Iter>
  aw_task_many(Iter TaskIterator)
    requires(std::is_convertible_v<
              typename std::iter_value_t<Iter>, task<Result>>)
      : executor(detail::this_thread::executor),
        continuation_executor(detail::this_thread::executor),
        prio(detail::this_thread::this_task.prio), did_await(false) {
    const auto size = Count;
    for (size_t i = 0; i < size; ++i) {
      // TODO capture an iter_adapter that applies these transformations instead
      // of the whole wrapped array -- see note at end of file
      // If this is lazily evaluated only when the tasks are posted, this class
      // can be made movable
      task<Result> t = *TaskIterator;
      auto& p = t.promise();
      p.continuation = &continuation;
      p.continuation_executor = &continuation_executor;
      p.done_count = &done_count;
      p.result_ptr = &result[i];
      wrapped[i] = std::coroutine_handle<>(t);
      ++TaskIterator;
    }
    done_count.store(static_cast<int64_t>(size) - 1, std::memory_order_release);
  }

  /// For use when `Count` is a runtime parameter.
  /// It is recommended to call `spawn_many()` instead of using this constructor
  /// directly.
  template <typename Iter>
  aw_task_many(Iter TaskIterator, size_t TaskCount)
    requires(Count == 0 && std::is_convertible_v<
                             typename std::iter_value_t<Iter>, task<Result>>)
      : executor(detail::this_thread::executor),
        continuation_executor(detail::this_thread::executor),
        prio(detail::this_thread::this_task.prio), did_await(false) {
    const auto size = TaskCount;
    wrapped.resize(size);
    result.resize(size);
    for (size_t i = 0; i < size; ++i) {
      task<Result> t = *TaskIterator;
      auto& p = t.promise();
      p.continuation = &continuation;
      p.continuation_executor = &continuation_executor;
      p.done_count = &done_count;
      p.result_ptr = &result[i];
      wrapped[i] = std::coroutine_handle<>(t);
      ++TaskIterator;
    }
    done_count.store(static_cast<int64_t>(size) - 1, std::memory_order_release);
  }

  /// For use when `Count` is known at compile time.
  /// It is recommended to call `spawn_many()` instead of using this constructor
  /// directly.
  template <typename Functor>
  aw_task_many(Functor* FunctorIterator)
    requires(!std::is_convertible_v<Functor, std::coroutine_handle<>> &&
             std::is_invocable_r_v<Result, Functor>)
      : executor(detail::this_thread::executor),
        continuation_executor(detail::this_thread::executor),
        prio(detail::this_thread::this_task.prio), did_await(false) {
    const auto size = Count;
    for (size_t i = 0; i < size; ++i) {
      task<Result> t = [](Functor Func) -> task<Result> {
        co_return Func();
      }(FunctorIterator[i]);
      auto& p = t.promise();
      p.continuation = &continuation;
      p.continuation_executor = &continuation_executor;
      p.done_count = &done_count;
      p.result_ptr = &result[i];
      wrapped[i] = std::coroutine_handle<>(t);
    }
    done_count.store(static_cast<int64_t>(size) - 1, std::memory_order_release);
  }

  /// For use when `Count` is a runtime parameter.
  /// It is recommended to call `spawn_many()` instead of using this constructor
  /// directly.
  template <typename Functor>
  aw_task_many(Functor* FunctorIterator, size_t FunctorCount)
    requires(!std::is_convertible_v<Functor, std::coroutine_handle<>> &&
             std::is_invocable_r_v<Result, Functor> && Count == 0)
      : executor(detail::this_thread::executor),
        continuation_executor(detail::this_thread::executor),
        prio(detail::this_thread::this_task.prio), did_await(false) {
    const auto size = FunctorCount;
    wrapped.resize(size);
    result.resize(size);
    for (size_t i = 0; i < size; ++i) {
      task<Result> t = [](Functor Func) -> task<Result> {
        co_return Func();
      }(FunctorIterator[i]);
      auto& p = t.promise();
      p.continuation = &continuation;
      p.continuation_executor = &continuation_executor;
      p.done_count = &done_count;
      p.result_ptr = &result[i];
      wrapped[i] = std::coroutine_handle<>(t);
    }
    done_count.store(static_cast<int64_t>(size) - 1, std::memory_order_release);
  }

  /// Always suspends.
  constexpr bool await_ready() const noexcept { return false; }

  /// Submits the provided tasks to the chosen executor and waits for them to
  /// complete.
  std::coroutine_handle<> await_suspend(std::coroutine_handle<> Outer
  ) noexcept {
    continuation = Outer;
    assert(!did_await);
    did_await = true;
    // if the newly posted tasks are at least as high priority as the currently
    // running or next-running (yield requested) task, we can directly transfer
    // to one
    bool doSymmetricTransfer =
      prio <= detail::this_thread::this_task.yield_priority->load(
                std::memory_order_acquire
              ) &&
      executor == detail::this_thread::executor;
    auto postCount = doSymmetricTransfer ? wrapped.size() - 1 : wrapped.size();
    if (postCount != 0) {
      executor->post_bulk(wrapped.data(), prio, postCount);
    }
    if (doSymmetricTransfer) {
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

  ResultArray& await_resume() & noexcept { return result; }
  ResultArray&& await_resume() && noexcept { return std::move(result); }

  ~aw_task_many() noexcept {
    // If you spawn a function that returns a non-void type,
    // then you must co_await the return of spawn!
    assert(did_await);
  }
  aw_task_many(const aw_task_many&) = delete;
  aw_task_many& operator=(const aw_task_many&) = delete;
  aw_task_many(aw_task_many&& Other) = delete;
  // aw_task_many(aw_task_many&& other) {
  //   executor = std::move(other.executor);
  //   continuation = std::move(other.continuation);
  //   wrapped = std::move(other.wrapped);
  //   result = std::move(other.result);
  //   prio = other.prio;
  //   did_await = other.did_await;
  //   other.did_await = true; // prevent other from posting
  // }
  aw_task_many& operator=(aw_task_many&& Other) = delete;
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

  /// After the spawned tasks complete, the outer coroutine will be resumed
  /// on the provided executor.
  inline aw_task_many& resume_on(detail::type_erased_executor* Executor) {
    continuation_executor = Executor;
    return *this;
  }
  /// After the spawned tasks complete, the outer coroutine will be resumed
  /// on the provided executor.
  template <detail::TypeErasableExecutor Exec>
  aw_task_many& resume_on(Exec& Executor) {
    return resume_on(Executor.type_erased());
  }
  /// After the spawned tasks complete, the outer coroutine will be resumed
  /// on the provided executor.
  template <detail::TypeErasableExecutor Exec>
  aw_task_many& resume_on(Exec* Executor) {
    return resume_on(Executor->type_erased());
  }

  /// The wrapped tasks will run on the provided executor.
  inline aw_task_many& run_on(detail::type_erased_executor* Executor) {
    executor = Executor;
    return *this;
  }
  /// The wrapped tasks will run on the provided executor.
  template <detail::TypeErasableExecutor Exec>
  aw_task_many& run_on(Exec& Executor) {
    return run_on(Executor.type_erased());
  }
  /// The wrapped tasks will run on the provided executor.
  template <detail::TypeErasableExecutor Exec>
  aw_task_many& run_on(Exec* Executor) {
    return run_on(Executor->type_erased());
  }

  /// Sets the priority of the wrapped tasks. If co_awaited, the outer
  /// coroutine will also be resumed with this priority.
  inline aw_task_many& with_priority(size_t Priority) {
    prio = Priority;
    return *this;
  }

  /// Submits the wrapped tasks immediately, without suspending the current
  /// coroutine. You must await the return type before destroying it.
  ///
  /// This is not how you spawn a task in a detached state! For that, just call
  /// spawn_many() and discard the return value.
  ///
  /// (You cannot spawn tasks that return non-void `Result` in a detached
  /// state; if the task returns a value, you must consume it).
  inline aw_run_early<Result, ResultArray> run_early() {
    return aw_run_early<Result, ResultArray>(std::move(*this));
  }
};

template <IsVoid Result, size_t Count> class aw_task_many<Result, Count> {
  static_assert(sizeof(task<void>) == sizeof(std::coroutine_handle<>));
  static_assert(alignof(task<void>) == alignof(std::coroutine_handle<>));
  using WrappedArray = std::conditional_t<
    Count == 0, std::vector<work_item>, std::array<work_item, Count>>;
  friend class aw_run_early<Result, void>;
  WrappedArray wrapped;
  std::coroutine_handle<> continuation;
  detail::type_erased_executor* executor;
  detail::type_erased_executor* continuation_executor;
  size_t prio;
  bool did_await;
  std::atomic<int64_t> done_count;

public:
  /// For use when `Count` is known at compile time.
  /// It is recommended to call `spawn_many()` instead of using this constructor
  /// directly.
  template <typename Iter>
  aw_task_many(Iter TaskIterator)
    requires(std::is_convertible_v<
              typename std::iter_value_t<Iter>, task<void>>)
      : executor(detail::this_thread::executor),
        continuation_executor(detail::this_thread::executor),
        prio(detail::this_thread::this_task.prio), did_await(false) {
    const auto size = Count;
    for (size_t i = 0; i < size; ++i) {
      task<void> t = *TaskIterator;
      auto& p = t.promise();
      p.continuation = &continuation;
      p.continuation_executor = &continuation_executor;
      p.done_count = &done_count;
      wrapped[i] = std::coroutine_handle<>(t);
      ++TaskIterator;
    }
    done_count.store(static_cast<int64_t>(size) - 1, std::memory_order_release);
  }

  /// For use when `Count` is a runtime parameter.
  /// It is recommended to call `spawn_many()` instead of using this constructor
  /// directly.
  template <typename Iter>
  aw_task_many(Iter TaskIterator, size_t TaskCount)
    requires(std::is_convertible_v<
              typename std::iter_value_t<Iter>, task<void>>)
      : executor(detail::this_thread::executor),
        continuation_executor(detail::this_thread::executor),
        prio(detail::this_thread::this_task.prio), did_await(false) {
    const auto size = TaskCount;
    wrapped.resize(size);
    for (size_t i = 0; i < size; ++i) {
      task<void> t = *TaskIterator;
      auto& p = t.promise();
      p.continuation = &continuation;
      p.continuation_executor = &continuation_executor;
      p.done_count = &done_count;
      wrapped[i] = std::coroutine_handle<>(t);
      ++TaskIterator;
    }
    done_count.store(static_cast<int64_t>(size) - 1, std::memory_order_release);
  }

  /// For use when `Count` is known at compile time.
  /// It is recommended to call `spawn_many()` instead of using this constructor
  /// directly.
  template <typename Functor>
  aw_task_many(Functor* FunctorIterator)
    requires(!std::is_convertible_v<Functor, std::coroutine_handle<>> &&
             std::is_invocable_r_v<Result, Functor>)
      : executor(detail::this_thread::executor),
        continuation_executor(detail::this_thread::executor),
        prio(detail::this_thread::this_task.prio), did_await(false) {
    const auto size = Count;
    for (size_t i = 0; i < size; ++i) {
      task<void> t = [](Functor Func) -> task<void> {
        Func();
        co_return;
        // TODO change this to use ++ instead of indexing
        // (so it's actually compatible with iterators)
      }(FunctorIterator[i]);
      auto& p = t.promise();
      p.continuation = &continuation;
      p.continuation_executor = &continuation_executor;
      p.done_count = &done_count;
      wrapped[i] = std::coroutine_handle<>(t);
    }
    done_count.store(static_cast<int64_t>(size) - 1, std::memory_order_release);
  }

  /// For use when `Count` is a runtime parameter.
  /// It is recommended to call `spawn_many()` instead of using this constructor
  /// directly.
  template <typename Functor>
  aw_task_many(Functor* FunctorIterator, size_t FunctorCount)
    requires(!std::is_convertible_v<Functor, std::coroutine_handle<>> &&
             std::is_invocable_r_v<Result, Functor> && Count == 0)
      : executor(detail::this_thread::executor),
        continuation_executor(detail::this_thread::executor),
        prio(detail::this_thread::this_task.prio), did_await(false) {
    const auto size = FunctorCount;
    wrapped.resize(size);
    for (size_t i = 0; i < size; ++i) {
      task<void> t = [](Functor Func) -> task<void> {
        Func();
        co_return;
      }(FunctorIterator[i]);
      auto& p = t.promise();
      p.continuation = &continuation;
      p.continuation_executor = &continuation_executor;
      p.done_count = &done_count;
      wrapped[i] = std::coroutine_handle<>(t);
    }
    done_count.store(static_cast<int64_t>(size) - 1, std::memory_order_release);
  }

  /// Always suspends.
  constexpr bool await_ready() const noexcept { return false; }

  /// Submits the provided tasks to the chosen executor and waits for them to
  /// complete.
  std::coroutine_handle<> await_suspend(std::coroutine_handle<> Outer
  ) noexcept {
    continuation = Outer;
    assert(!did_await);
    did_await = true;
    // if the newly posted tasks are at least as high priority as the currently
    // running or next-running (yield requested) task, we can directly transfer
    // to one
    bool doSymmetricTransfer =
      prio <= detail::this_thread::this_task.yield_priority->load(
                std::memory_order_acquire
              ) &&
      executor == detail::this_thread::executor;
    auto postCount = doSymmetricTransfer ? wrapped.size() - 1 : wrapped.size();
    if (postCount != 0) {
      executor->post_bulk(wrapped.data(), prio, postCount);
    }
    if (doSymmetricTransfer) {
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

  /// For void Result, if this was not co_await'ed, post the tasks to the
  /// executor in the destructor. This allows spawn() to be invoked as a
  /// standalone function to create detached tasks.
  ~aw_task_many() noexcept {
    if (!did_await) {
      executor->post_bulk(wrapped.data(), prio, wrapped.size());
    }
  }

  aw_task_many(const aw_task_many&) = delete;
  aw_task_many& operator=(const aw_task_many&) = delete;
  aw_task_many(aw_task_many&& Other) = delete;
  // aw_task_many(aw_task_many&& other) {
  //   executor = std::move(other.executor);
  //   continuation = std::move(other.continuation);
  //   wrapped = std::move(other.wrapped);
  //   prio = other.prio;
  //   did_await = other.did_await;
  //   other.did_await = true; // prevent other from posting
  // }
  aw_task_many& operator=(aw_task_many&& Other) = delete;
  // aw_task_many& operator=(aw_task_many&& other) {
  //   executor = std::move(other.executor);
  //   continuation = std::move(other.continuation);
  //   wrapped = std::move(other.wrapped);
  //   prio = other.prio;
  //   did_await = other.did_await;
  //   other.did_await = true; // prevent other from posting
  //   return *this;
  // }

  /// After the spawned tasks complete, the outer coroutine will be resumed
  /// on the provided executor.
  inline aw_task_many& resume_on(detail::type_erased_executor* Executor) {
    continuation_executor = Executor;
    return *this;
  }
  /// After the spawned tasks complete, the outer coroutine will be resumed
  /// on the provided executor.
  template <detail::TypeErasableExecutor Exec>
  aw_task_many& resume_on(Exec& Executor) {
    return resume_on(Executor.type_erased());
  }
  /// After the spawned tasks complete, the outer coroutine will be resumed
  /// on the provided executor.
  template <detail::TypeErasableExecutor Exec>
  aw_task_many& resume_on(Exec* Executor) {
    return resume_on(Executor->type_erased());
  }

  /// The wrapped tasks will run on the provided executor.
  inline aw_task_many& run_on(detail::type_erased_executor* Executor) {
    executor = Executor;
    return *this;
  }
  /// The wrapped tasks will run on the provided executor.
  template <detail::TypeErasableExecutor Exec>
  aw_task_many& run_on(Exec& Executor) {
    return run_on(Executor.type_erased());
  }
  /// The wrapped tasks will run on the provided executor.
  template <detail::TypeErasableExecutor Exec>
  aw_task_many& run_on(Exec* Executor) {
    return run_on(Executor->type_erased());
  }

  /// Sets the priority of the wrapped tasks. If co_awaited, the outer
  /// coroutine will also be resumed with this priority.
  inline aw_task_many& with_priority(size_t Priority) {
    prio = Priority;
    return *this;
  }

  /// Submits the wrapped task immediately, without suspending the current
  /// coroutine. You must await the return type before destroying it.
  ///
  /// This is not how you spawn a task in a detached state! For that, just call
  /// spawn() and discard the return value.
  inline aw_run_early<Result, void> run_early() {
    return aw_run_early<Result, void>(std::move(*this));
  }
};

} // namespace tmc

// some ways to capture transformer iterator instead of entire wrapper
// (and reduce size of this struct)
// causes problems with CTAD; need to split Functor and non-Functor versions
// template <typename InputIter> struct TaskIterTransformer {
//     InputIter task_iter;
//     aw_task_many *me;
//     std::coroutine_handle<> operator()(size_t i) {
//       task<void> t = *task_iter;
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
//   task<void> t = *task_iter;
//   ++task_iter;
//   auto &p = t.promise();
//   p.continuation = &continuation;
//   p.done_count = &done_count;
//   p.result_ptr = &result[i];
//   return std::coroutine_handle<>(t);
// });
