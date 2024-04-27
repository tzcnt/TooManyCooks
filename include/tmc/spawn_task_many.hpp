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
#include <type_traits>
#include <vector>

namespace tmc {

/// For use when the number of items to spawn is known at compile time.
/// `Count` must be non-zero.
/// `Iter` must be an iterator type that implements `operator*()` and
/// `Iter& operator++()`.
///
/// Submits `Count` items to the executor.
template <
  size_t Count, typename TaskIter,
  typename Result = typename std::iter_value_t<TaskIter>::result_type>
aw_task_many<Result, Count> spawn_many(TaskIter TaskIterator)
  requires(std::is_convertible_v<std::iter_value_t<TaskIter>, task<Result>>)
{
  static_assert(Count != 0);
  return aw_task_many<Result, Count>(TaskIterator);
}

/// For use when the number of items to spawn is known at compile time.
/// `Count` must be non-zero.
/// `Functor` must be a copyable type that implements `Result operator()`.
/// `FunctorIterator` must be a pointer to an array of `Functor`.
///
/// Submits `Count` items to the executor.
template <
  size_t Count, typename FuncIter,
  typename Functor = std::iter_value_t<FuncIter>,
  typename Result = std::invoke_result_t<Functor>>
aw_task_many<Result, Count> spawn_many(FuncIter FunctorIterator)
  requires(
    std::is_invocable_r_v<Result, Functor> && (!requires {
      typename Functor::result_type;
    } || !std::is_convertible_v<Functor, task<typename Functor::result_type>>)
  )
{
  static_assert(Count != 0);
  return aw_task_many<Result, Count>(FunctorIterator);
}

/// For use when the number of items to spawn is a runtime parameter.
/// `It` must be an iterator type that implements `operator*()` and
/// `It& operator++()`.
/// `TaskCount` must be non-zero.
///
/// Submits `TaskCount` items to the executor.
template <
  typename TaskIter,
  typename Result = typename std::iter_value_t<TaskIter>::result_type>
aw_task_many<Result, 0> spawn_many(TaskIter TaskIterator, size_t TaskCount)
  requires(std::is_convertible_v<std::iter_value_t<TaskIter>, task<Result>>)
{
  return aw_task_many<Result, 0>(TaskIterator, TaskCount);
}

/// For use when the number of items to spawn is a runtime parameter.
/// `Functor` must be a copyable type that implements `Result operator()`.
/// `FunctorIterator` must be a pointer to an array of `Functor`.
/// `FunctorCount` must be non-zero.
///
/// Submits `FunctorCount` items to the executor.
template <
  typename FuncIter, typename Functor = std::iter_value_t<FuncIter>,
  typename Result = std::invoke_result_t<Functor>>
aw_task_many<Result, 0>
spawn_many(FuncIter FunctorIterator, size_t FunctorCount)
  requires(
    std::is_invocable_r_v<Result, Functor> && (!requires {
      typename Functor::result_type;
    } || !std::is_convertible_v<Functor, task<typename Functor::result_type>>)
  )
{
  return aw_task_many<Result, 0>(FunctorIterator, FunctorCount);
}

template <typename Result, size_t Count, bool RValue> class aw_task_many_impl;

template <typename Result, size_t Count, bool RValue> class aw_task_many_impl {
  aw_task_many<typename Result::value_type, Count>& me;
  friend aw_task_many<typename Result::value_type, Count>;
  aw_task_many_impl(aw_task_many<typename Result::value_type, Count>& Me);

public:
  /// Always suspends.
  inline bool await_ready() const noexcept;

  /// Suspends the outer coroutine, submits the wrapped task to the
  /// executor, and waits for it to complete.
  inline std::coroutine_handle<> await_suspend(std::coroutine_handle<> Outer
  ) noexcept;

  /// Returns the value provided by the wrapped task.
  inline Result& await_resume() noexcept
    requires(!RValue);

  /// Returns the value provided by the wrapped task.
  inline Result&& await_resume() noexcept
    requires(RValue);
};

template <size_t Count> class aw_task_many_impl<void, Count, false> {
  aw_task_many<void, Count>& me;
  friend aw_task_many<void, Count>;

  inline aw_task_many_impl(aw_task_many<void, Count>& Me);

public:
  /// Always suspends.
  inline bool await_ready() const noexcept;

  /// Suspends the outer coroutine, submits the wrapped task to the
  /// executor, and waits for it to complete.
  inline std::coroutine_handle<> await_suspend(std::coroutine_handle<> Outer
  ) noexcept;

  /// Does nothing.
  inline void await_resume() noexcept;
};

// Primary template is forward-declared in "tmc/detail/aw_run_early.hpp".
template <typename Result, size_t Count>
class [[nodiscard(
  "You must use the aw_task_many<Result> by one of: 1. co_await 2. run_early()"
)]] aw_task_many {
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
  friend class aw_task_many_impl<ResultArray, Count, true>;
  friend class aw_task_many_impl<ResultArray, Count, false>;
  WrappedArray wrapped;
  std::coroutine_handle<> continuation;
  detail::type_erased_executor* executor;
  detail::type_erased_executor* continuation_executor;
  size_t prio;
  bool did_await;
  // result[i] and done_count are both written by each coroutine
  // for cache sharing efficiency, they should be adjacent
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
      task<Result> t = std::move(*TaskIterator);
      auto& p = t.promise();
      p.continuation = &continuation;
      p.continuation_executor = &continuation_executor;
      p.done_count = &done_count;
      p.result_ptr = &result[i];
      wrapped[i] = std::move(t);
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
      task<Result> t = std::move(*TaskIterator);
      auto& p = t.promise();
      p.continuation = &continuation;
      p.continuation_executor = &continuation_executor;
      p.done_count = &done_count;
      p.result_ptr = &result[i];
      wrapped[i] = std::move(t);
      ++TaskIterator;
    }
    done_count.store(static_cast<int64_t>(size) - 1, std::memory_order_release);
  }

  /// For use when `Count` is known at compile time.
  /// It is recommended to call `spawn_many()` instead of using this constructor
  /// directly.
  template <typename Iter, typename Functor = std::iter_value_t<Iter>>
  aw_task_many(Iter FunctorIterator)
    requires(std::is_invocable_r_v<Result, Functor> &&
             !std::is_convertible_v<
               Functor, task<typename Functor::result_type>>)
      : executor(detail::this_thread::executor),
        continuation_executor(detail::this_thread::executor),
        prio(detail::this_thread::this_task.prio), did_await(false) {
    const auto size = Count;
    for (size_t i = 0; i < size; ++i) {
      task<Result> t = [](Functor Func) -> task<Result> {
        co_return Func();
      }(*FunctorIterator);
      auto& p = t.promise();
      p.continuation = &continuation;
      p.continuation_executor = &continuation_executor;
      p.done_count = &done_count;
      p.result_ptr = &result[i];
      wrapped[i] = std::move(t);
      ++FunctorIterator;
    }
    done_count.store(static_cast<int64_t>(size) - 1, std::memory_order_release);
  }

  /// For use when `Count` is a runtime parameter.
  /// It is recommended to call `spawn_many()` instead of using this constructor
  /// directly.
  template <typename Iter, typename Functor = std::iter_value_t<Iter>>
  aw_task_many(Iter FunctorIterator, size_t FunctorCount)
    requires(Count == 0 && std::is_invocable_r_v<Result, Functor> &&
             !std::is_convertible_v<
               Functor, task<typename Functor::result_type>>)
      : executor(detail::this_thread::executor),
        continuation_executor(detail::this_thread::executor),
        prio(detail::this_thread::this_task.prio), did_await(false) {
    const auto size = FunctorCount;
    wrapped.resize(size);
    result.resize(size);
    for (size_t i = 0; i < size; ++i) {
      task<Result> t = [](Functor Func) -> task<Result> {
        co_return Func();
      }(*FunctorIterator);
      auto& p = t.promise();
      p.continuation = &continuation;
      p.continuation_executor = &continuation_executor;
      p.done_count = &done_count;
      p.result_ptr = &result[i];
      wrapped[i] = std::move(t);
      ++FunctorIterator;
    }
    done_count.store(static_cast<int64_t>(size) - 1, std::memory_order_release);
  }

  aw_task_many_impl<ResultArray, Count, false> operator co_await() & {
    return aw_task_many_impl<ResultArray, Count, false>(*this);
  }

  aw_task_many_impl<ResultArray, Count, true> operator co_await() && {
    return aw_task_many_impl<ResultArray, Count, true>(*this);
  }

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
  inline aw_run_early<Result, ResultArray> run_early() && {
    return aw_run_early<Result, ResultArray>(std::move(*this));
  }
};

template <size_t Count>
class [[nodiscard("You must use the aw_task_many<void> by one of: 1. co_await "
                  "2. detach() 3. run_early()")]] aw_task_many<void, Count> {
  static_assert(sizeof(task<void>) == sizeof(std::coroutine_handle<>));
  static_assert(alignof(task<void>) == alignof(std::coroutine_handle<>));
  using WrappedArray = std::conditional_t<
    Count == 0, std::vector<work_item>, std::array<work_item, Count>>;
  friend class aw_run_early<void, void>;
  friend class aw_task_many_impl<void, Count, false>;
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
      task<void> t = std::move(*TaskIterator);
      auto& p = t.promise();
      p.continuation = &continuation;
      p.continuation_executor = &continuation_executor;
      p.done_count = &done_count;
      wrapped[i] = std::move(t);
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
      task<void> t = std::move(*TaskIterator);
      auto& p = t.promise();
      p.continuation = &continuation;
      p.continuation_executor = &continuation_executor;
      p.done_count = &done_count;
      wrapped[i] = std::move(t);
      ++TaskIterator;
    }
    done_count.store(static_cast<int64_t>(size) - 1, std::memory_order_release);
  }

  /// For use when `Count` is known at compile time.
  /// It is recommended to call `spawn_many()` instead of using this constructor
  /// directly.
  template <typename Iter, typename Functor = std::iter_value_t<Iter>>
  aw_task_many(Iter FunctorIterator)
    requires(std::is_invocable_r_v<void, Functor> &&
             !std::is_convertible_v<Functor, task<void>>)
      : executor(detail::this_thread::executor),
        continuation_executor(detail::this_thread::executor),
        prio(detail::this_thread::this_task.prio), did_await(false) {
    const auto size = Count;
    for (size_t i = 0; i < size; ++i) {
      task<void> t = [](Functor Func) -> task<void> {
        Func();
        co_return;
      }(*FunctorIterator);
      auto& p = t.promise();
      p.continuation = &continuation;
      p.continuation_executor = &continuation_executor;
      p.done_count = &done_count;
      wrapped[i] = std::move(t);
      ++FunctorIterator;
    }
    done_count.store(static_cast<int64_t>(size) - 1, std::memory_order_release);
  }

  /// For use when `Count` is a runtime parameter.
  /// It is recommended to call `spawn_many()` instead of using this constructor
  /// directly.
  template <typename Iter, typename Functor = std::iter_value_t<Iter>>
  aw_task_many(Iter FunctorIterator, size_t FunctorCount)
    requires(Count == 0 && std::is_invocable_r_v<void, Functor> &&
             !std::is_convertible_v<Functor, task<void>>)
      : executor(detail::this_thread::executor),
        continuation_executor(detail::this_thread::executor),
        prio(detail::this_thread::this_task.prio), did_await(false) {
    const auto size = FunctorCount;
    wrapped.resize(size);
    for (size_t i = 0; i < size; ++i) {
      task<void> t = [](Functor Func) -> task<void> {
        Func();
        co_return;
      }(*FunctorIterator);
      auto& p = t.promise();
      p.continuation = &continuation;
      p.continuation_executor = &continuation_executor;
      p.done_count = &done_count;
      wrapped[i] = std::move(t);
      ++FunctorIterator;
    }
    done_count.store(static_cast<int64_t>(size) - 1, std::memory_order_release);
  }

  aw_task_many_impl<void, Count, false> operator co_await() {
    return aw_task_many_impl<void, Count, false>(*this);
  }

  /// Submit the tasks to the executor immediately. They cannot be awaited
  /// afterward.
  void detach() {
    assert(!did_await);
    executor->post_bulk(wrapped.data(), prio, wrapped.size());
    did_await = true;
  }

  ~aw_task_many() noexcept { assert(did_await); }

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
  inline aw_run_early<void, void> run_early() && {
    return aw_run_early<void, void>(std::move(*this));
  }
};

template <typename Result, size_t Count, bool RValue>
aw_task_many_impl<Result, Count, RValue>::aw_task_many_impl(
  aw_task_many<typename Result::value_type, Count>& Me
)
    : me(Me) {}

/// Always suspends.
template <typename Result, size_t Count, bool RValue>
inline bool
aw_task_many_impl<Result, Count, RValue>::await_ready() const noexcept {
  return false;
}

/// Suspends the outer coroutine, submits the wrapped task to the
/// executor, and waits for it to complete.
template <typename Result, size_t Count, bool RValue>
inline std::coroutine_handle<>
aw_task_many_impl<Result, Count, RValue>::await_suspend(
  std::coroutine_handle<> Outer
) noexcept {
  me.continuation = Outer;
  assert(!me.did_await);
  me.did_await = true;
  // if the newly posted tasks are at least as high priority as the currently
  // running or next-running (yield requested) task, we can directly transfer
  // to one
  bool doSymmetricTransfer =
    me.prio <= detail::this_thread::this_task.yield_priority->load(
                 std::memory_order_acquire
               ) &&
    me.executor == detail::this_thread::executor;
  auto postCount =
    doSymmetricTransfer ? me.wrapped.size() - 1 : me.wrapped.size();
  if (postCount != 0) {
    me.executor->post_bulk(me.wrapped.data(), me.prio, postCount);
  }
  if (doSymmetricTransfer) {
    // symmetric transfer to the last task IF it should run immediately
    return TMC_WORK_ITEM_AS_STD_CORO(me.wrapped.back());
  } else {
    return std::noop_coroutine();
  }
}

/// Returns the value provided by the wrapped function.
template <typename Result, size_t Count, bool RValue>
inline Result& aw_task_many_impl<Result, Count, RValue>::await_resume() noexcept
  requires(!RValue)
{
  return me.result;
}

/// Returns the value provided by the wrapped function.
template <typename Result, size_t Count, bool RValue>
inline Result&&
aw_task_many_impl<Result, Count, RValue>::await_resume() noexcept
  requires(RValue)
{
  return std::move(me.result);
}

template <size_t Count>
inline aw_task_many_impl<void, Count, false>::aw_task_many_impl(
  aw_task_many<void, Count>& Me
)
    : me(Me) {}

template <size_t Count>
inline bool
aw_task_many_impl<void, Count, false>::await_ready() const noexcept {
  return false;
}

/// Suspends the outer coroutine, submits the wrapped task to the
/// executor, and waits for it to complete.
template <size_t Count>
inline std::coroutine_handle<>
aw_task_many_impl<void, Count, false>::await_suspend(
  std::coroutine_handle<> Outer
) noexcept {
  me.continuation = Outer;
  assert(!me.did_await);
  me.did_await = true;
  // if the newly posted tasks are at least as high priority as the currently
  // running or next-running (yield requested) task, we can directly transfer
  // to one
  bool doSymmetricTransfer =
    me.prio <= detail::this_thread::this_task.yield_priority->load(
                 std::memory_order_acquire
               ) &&
    me.executor == detail::this_thread::executor;
  auto postCount =
    doSymmetricTransfer ? me.wrapped.size() - 1 : me.wrapped.size();
  if (postCount != 0) {
    me.executor->post_bulk(me.wrapped.data(), me.prio, postCount);
  }
  if (doSymmetricTransfer) {
    // symmetric transfer to the last task IF it should run immediately
    return TMC_WORK_ITEM_AS_STD_CORO(me.wrapped.back());
  } else {
    return std::noop_coroutine();
  }
}

/// Does nothing.
template <size_t Count>
inline void aw_task_many_impl<void, Count, false>::await_resume() noexcept {}

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
//       return t;
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
//   return t;
// });
