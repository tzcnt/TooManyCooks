// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/awaitable_customizer.hpp"
#include "tmc/detail/compat.hpp"
#include "tmc/detail/concepts_awaitable.hpp" // IWYU pragma: keep
#include "tmc/detail/each_result.hpp"
#include "tmc/detail/mixins.hpp"
#include "tmc/detail/task_wrapper.hpp"
#include "tmc/detail/thread_locals.hpp"
#include "tmc/ex_any.hpp"
#include "tmc/work_item.hpp"

#include <cassert>
#include <coroutine>
#include <tuple>
#include <type_traits>
#include <variant>

namespace tmc {
namespace detail {
// Replace void with std::monostate (void is not a valid tuple element type)
template <typename T>
using void_to_monostate =
  std::conditional_t<std::is_void_v<T>, std::monostate, T>;

// Get the last type of a parameter pack
// In C++26 you can use pack indexing instead: T...[sizeof...(T) - 1]
template <typename... T> struct last_type {
  using type = typename decltype((std::type_identity<T>{}, ...))::type;
};

template <> struct last_type<> {
  // workaround for empty tuples - the task object will be void / empty
  using type = void;
};

template <typename... T> using last_type_t = typename last_type<T...>::type;

// Create 2 instantiations of the Variadic, one where all of the types
// satisfy the predicate, and one where none of the types satisfy the predicate.
template <
  template <class> class Predicate, template <class...> class Variadic,
  class...>
struct predicate_partition;

// Definition for empty parameter pack
template <template <class> class Predicate, template <class...> class Variadic>
struct predicate_partition<Predicate, Variadic> {
  using true_types = Variadic<>;
  using false_types = Variadic<>;
};

template <
  template <class> class Predicate, template <class...> class Variadic, class T,
  class... Ts>
struct predicate_partition<Predicate, Variadic, T, Ts...> {
  template <class, class> struct Cons;
  template <class Head, class... Tail> struct Cons<Head, Variadic<Tail...>> {
    using type = Variadic<Head, Tail...>;
  };

  // Every type in the parameter pack satisfies the predicate
  using true_types = typename std::conditional<
    Predicate<T>::value,
    typename Cons<
      T, typename predicate_partition<Predicate, Variadic, Ts...>::true_types>::
      type,
    typename predicate_partition<Predicate, Variadic, Ts...>::true_types>::type;

  // No type in the parameter pack satisfies the predicate
  using false_types = typename std::conditional<
    !Predicate<T>::value,
    typename Cons<
      T, typename predicate_partition<
           Predicate, Variadic, Ts...>::false_types>::type,
    typename predicate_partition<Predicate, Variadic, Ts...>::false_types>::
    type;
};

// Partition the awaitables into two groups:
// 1. The awaitables that are coroutines, which will be submitted in bulk /
// symmetric transfer.
// 2. The awaitables that are not coroutines, which will be initiated by
// async_initiate.
template <typename T> struct treat_as_coroutine {
  static constexpr bool value =
    tmc::detail::get_awaitable_traits<T>::mode == tmc::detail::TMC_TASK ||
    tmc::detail::get_awaitable_traits<T>::mode == tmc::detail::COROUTINE ||
    tmc::detail::get_awaitable_traits<T>::mode == tmc::detail::WRAPPER;
};
} // namespace detail

/// The customizable task wrapper / awaitable type returned by
/// `tmc::spawn_tuple()`.
template <typename... Result> class aw_spawn_tuple;

template <bool IsEach, typename... Awaitable> class aw_spawn_tuple_impl {
  static constexpr auto Count = sizeof...(Awaitable);

  // When result_each() is called, tasks are synchronized via an atomic bitmask
  // with only 63 (or 31, on 32-bit) slots for tasks. result_each() doesn't seem
  // like a good fit for larger task groups anyway. If you really need more
  // room, please open a GitHub issue explaining why...
  static_assert(!IsEach || Count < TMC_PLATFORM_BITS);

  static constexpr size_t WorkItemCount =
    std::tuple_size_v<typename tmc::detail::predicate_partition<
      tmc::detail::treat_as_coroutine, std::tuple, Awaitable...>::true_types>;

  union {
    std::coroutine_handle<> symmetric_task;
    ptrdiff_t remaining_count;
  };
  std::coroutine_handle<> continuation;
  tmc::ex_any* continuation_executor;
  union {
    std::atomic<ptrdiff_t> done_count;
    std::atomic<size_t> sync_flags;
  };

  template <typename T>
  using ResultStorage =
    tmc::detail::result_storage_t<tmc::detail::void_to_monostate<
      typename tmc::detail::get_awaitable_traits<T>::result_type>>;
  using ResultTuple = std::tuple<ResultStorage<Awaitable>...>;
  ResultTuple result;

  using AwaitableTuple = std::tuple<Awaitable...>;

  friend aw_spawn_tuple<Awaitable...>;

  // coroutines are prepared and stored in an array, then submitted in bulk
  template <typename T>
  TMC_FORCE_INLINE inline void prepare_task(
    T&& Task, ResultStorage<T>* TaskResult, size_t idx, work_item& Task_out
  ) {
    tmc::detail::get_awaitable_traits<T>::set_continuation(Task, &continuation);
    tmc::detail::get_awaitable_traits<T>::set_continuation_executor(
      Task, &continuation_executor
    );
    if constexpr (IsEach) {
      tmc::detail::get_awaitable_traits<T>::set_done_count(Task, &sync_flags);
      tmc::detail::get_awaitable_traits<T>::set_flags(
        Task, tmc::detail::task_flags::EACH | idx
      );
    } else {
      tmc::detail::get_awaitable_traits<T>::set_done_count(Task, &done_count);
    }
    if constexpr (!std::is_void_v<typename tmc::detail::get_awaitable_traits<
                    T>::result_type>) {
      tmc::detail::get_awaitable_traits<T>::set_result_ptr(Task, TaskResult);
    }
    Task_out = std::move(Task);
  }

  // awaitables are submitted individually
  template <typename T>
  TMC_FORCE_INLINE inline void
  prepare_awaitable(T&& Task, ResultStorage<T>* TaskResult, size_t idx) {
    tmc::detail::get_awaitable_traits<T>::set_continuation(Task, &continuation);
    tmc::detail::get_awaitable_traits<T>::set_continuation_executor(
      Task, &continuation_executor
    );
    if constexpr (IsEach) {
      tmc::detail::get_awaitable_traits<T>::set_done_count(Task, &sync_flags);
      tmc::detail::get_awaitable_traits<T>::set_flags(
        Task, tmc::detail::task_flags::EACH | idx
      );
    } else {
      tmc::detail::get_awaitable_traits<T>::set_done_count(Task, &done_count);
    }
    if constexpr (!std::is_void_v<typename tmc::detail::get_awaitable_traits<
                    T>::result_type>) {
      tmc::detail::get_awaitable_traits<T>::set_result_ptr(Task, TaskResult);
    }
  }

  void set_done_count(size_t NumTasks) {
    if constexpr (IsEach) {
      remaining_count = NumTasks;
      sync_flags.store(
        tmc::detail::task_flags::EACH, std::memory_order_release
      );
    } else {
      done_count.store(NumTasks, std::memory_order_release);
    }
  }

  aw_spawn_tuple_impl(
    AwaitableTuple&& Tasks, tmc::ex_any* Executor,
    tmc::ex_any* ContinuationExecutor, size_t Prio, bool DoSymmetricTransfer
  )
      : continuation_executor{ContinuationExecutor} {
    if constexpr (!IsEach) {
      symmetric_task = nullptr;
    }

    std::array<work_item, WorkItemCount> taskArr;

    // Prepare each task as if I loops from [0..Count),
    // but using compile-time indexes and types.
    size_t taskIdx = 0;
    [&]<std::size_t... I>(std::index_sequence<I...>) {
      (([&]() {
         if constexpr (tmc::detail::get_awaitable_traits<
                         std::tuple_element_t<I, AwaitableTuple>>::mode ==
                         tmc::detail::TMC_TASK ||
                       tmc::detail::get_awaitable_traits<
                         std::tuple_element_t<I, AwaitableTuple>>::mode ==
                         tmc::detail::COROUTINE) {
           prepare_task(
             std::get<I>(std::move(Tasks)), &std::get<I>(result), I,
             taskArr[taskIdx]
           );
           ++taskIdx;
         } else if constexpr (tmc::detail::get_awaitable_traits<
                                std::tuple_element_t<I, AwaitableTuple>>::
                                mode == tmc::detail::ASYNC_INITIATE) {
           prepare_awaitable(
             std::get<I>(std::move(Tasks)), &std::get<I>(result), I
           );
         } else {
           // Wrap any unknown awaitable into a task
           prepare_task(
             tmc::detail::safe_wrap(std::get<I>(std::move(Tasks))),
             &std::get<I>(result), I, taskArr[taskIdx]
           );
           ++taskIdx;
         }
       }()),
       ...);
    }(std::make_index_sequence<Count>{});

    // Bulk submit the coroutines
    if constexpr (WorkItemCount != 0) {
      auto doneCount = Count;
      auto postCount = WorkItemCount;
      if (DoSymmetricTransfer) {
        symmetric_task = TMC_WORK_ITEM_AS_STD_CORO(taskArr[WorkItemCount - 1]);
        --doneCount;
        --postCount;
      }
      set_done_count(doneCount);
      tmc::detail::post_bulk_checked(Executor, taskArr.data(), postCount, Prio);
    } else {
      set_done_count(Count);
    }

    // Individually initiate the awaitables
    [&]<std::size_t... I>(std::index_sequence<I...>) {
      (([&]() {
         if constexpr (!tmc::detail::treat_as_coroutine<
                         std::tuple_element_t<I, AwaitableTuple>>::value) {
           tmc::detail::get_awaitable_traits<
             std::tuple_element_t<I, AwaitableTuple>>::
             async_initiate(std::get<I>(std::move(Tasks)), Executor, Prio);
         }
       }()),
       ...);
    }(std::make_index_sequence<Count>{});
  }

public:
  /*** SUPPORTS REGULAR AWAIT ***/
  /// Always suspends.
  inline bool await_ready() const noexcept
    requires(!IsEach)
  {
    // Always suspends, due to the possibility to resume on another executor.
    return false;
  }

  /// Suspends the outer coroutine, submits the wrapped task to the
  /// executor, and waits for it to complete.
  TMC_FORCE_INLINE inline std::coroutine_handle<>
  await_suspend(std::coroutine_handle<> Outer) noexcept
    requires(!IsEach)
  {
#ifndef NDEBUG
    assert(done_count.load() >= 0 && "You may only co_await this once.");
#endif
    continuation = Outer;
    std::coroutine_handle<> next;
    if (symmetric_task != nullptr) {
      // symmetric transfer to the last task IF it should run immediately
      next = symmetric_task;
    } else {
      // This logic is necessary because we submitted all child tasks before the
      // parent suspended. Allowing parent to be resumed before it suspends
      // would be UB. Therefore we need to block the resumption until here.
      auto remaining = done_count.fetch_sub(1, std::memory_order_acq_rel);
      // No symmetric transfer - all tasks were already posted.
      // Suspend if remaining > 0 (task is still running)
      if (remaining > 0) {
        next = std::noop_coroutine();
      } else { // Resume if remaining <= 0 (tasks already finished)
        if (continuation_executor == nullptr ||
            tmc::detail::this_thread::exec_is(continuation_executor)) {
          next = Outer;
        } else {
          // Need to resume on a different executor
          tmc::detail::post_checked(
            continuation_executor, std::move(Outer),
            tmc::detail::this_thread::this_task.prio
          );
          next = std::noop_coroutine();
        }
      }
    }
    return next;
  }

  /// Returns the value provided by the wrapped awaitables.
  /// Each awaitable has a slot in the tuple. If the awaitable would return
  /// void, its slot is represented by a std::monostate. If the awaitable would
  /// return a non-default-constructible type, that result will be wrapped in a
  /// std::optional.
  inline ResultTuple&& await_resume() noexcept
    requires(!IsEach)
  {
    return std::move(result);
  }
  /*** END REGULAR AWAIT ***/

  /*** SUPPORTS EACH() ***/
  /// Always suspends.
  inline bool await_ready() const noexcept
    requires(IsEach)
  {
    return tmc::detail::each_result_await_ready(remaining_count, sync_flags);
  }

  /// Suspends the outer coroutine, submits the wrapped task to the
  /// executor, and waits for it to complete.
  TMC_FORCE_INLINE inline bool await_suspend(std::coroutine_handle<> Outer
  ) noexcept
    requires(IsEach)
  {
    return tmc::detail::each_result_await_suspend(
      remaining_count, Outer, continuation, continuation_executor, sync_flags
    );
  }

  /// Returns the value provided by the wrapped tasks.
  /// Each task has a slot in the tuple. If the task would return void, its
  /// slot is represented by a std::monostate.
  inline size_t await_resume() noexcept
    requires(IsEach)
  {
    return tmc::detail::each_result_await_resume(remaining_count, sync_flags);
  }

  /// Provides a sentinel value that can be compared against the value returned
  /// from co_await.
  inline size_t end() noexcept
    requires(IsEach)
  {
    return 64;
  }

  // Gets the ready result at the given index.
  template <size_t I>
  inline std::tuple_element_t<I, ResultTuple>& get() noexcept
    requires(IsEach)
  {
    return std::get<I>(result);
  }
  /*** END EACH() ***/

  // This must be awaited and all child tasks completed before destruction.
#ifndef NDEBUG
  ~aw_spawn_tuple_impl() noexcept {
    if constexpr (IsEach) {
      assert(remaining_count == 0 && "You must submit or co_await this.");
    } else {
      assert(done_count.load() < 0 && "You must submit or co_await this.");
    }
  }
#endif

  // Not movable or copyable due to awaitables being initiated in constructor,
  // and having pointers to this.
  aw_spawn_tuple_impl& operator=(const aw_spawn_tuple_impl& other) = delete;
  aw_spawn_tuple_impl(const aw_spawn_tuple_impl& other) = delete;
  aw_spawn_tuple_impl& operator=(const aw_spawn_tuple_impl&& other) = delete;
  aw_spawn_tuple_impl(const aw_spawn_tuple_impl&& other) = delete;
};

template <typename... Result>
using aw_spawn_tuple_fork =
  tmc::detail::rvalue_only_awaitable<aw_spawn_tuple_impl<false, Result...>>;

template <typename... Result>
using aw_spawn_tuple_each = aw_spawn_tuple_impl<true, Result...>;

template <typename... Awaitable>
class [[nodiscard("You must await or initiate the result of spawn_tuple()."
)]] aw_spawn_tuple
    : public tmc::detail::run_on_mixin<aw_spawn_tuple<Awaitable...>>,
      public tmc::detail::resume_on_mixin<aw_spawn_tuple<Awaitable...>>,
      public tmc::detail::with_priority_mixin<aw_spawn_tuple<Awaitable...>> {
  friend class tmc::detail::run_on_mixin<aw_spawn_tuple<Awaitable...>>;
  friend class tmc::detail::resume_on_mixin<aw_spawn_tuple<Awaitable...>>;
  friend class tmc::detail::with_priority_mixin<aw_spawn_tuple<Awaitable...>>;

  static constexpr auto Count = sizeof...(Awaitable);

  static constexpr size_t WorkItemCount =
    std::tuple_size_v<typename tmc::detail::predicate_partition<
      tmc::detail::treat_as_coroutine, std::tuple, Awaitable...>::true_types>;

  using AwaitableTuple = std::tuple<Awaitable...>;
  AwaitableTuple wrapped;
  tmc::ex_any* executor;
  tmc::ex_any* continuation_executor;
  size_t prio;

#ifndef NDEBUG
  bool is_empty;
#endif

public:
  /// It is recommended to call `spawn_tuple()` instead of using this
  /// constructor directly.
  aw_spawn_tuple(std::tuple<Awaitable&&...> Tasks)
      : wrapped(std::move(Tasks)), executor(tmc::detail::this_thread::executor),
        continuation_executor(tmc::detail::this_thread::executor),
        prio(tmc::detail::this_thread::this_task.prio)
#ifndef NDEBUG
        ,
        is_empty(false)
#endif
  {
  }

  aw_spawn_tuple_impl<false, Awaitable...> operator co_await() && {
    bool doSymmetricTransfer = tmc::detail::this_thread::exec_is(executor) &&
                               tmc::detail::this_thread::prio_is(prio);
#ifndef NDEBUG
    if constexpr (Count != 0) {
      // Ensure that this was not previously moved-from
      assert(!is_empty && "You may only submit or co_await this once.");
    }
    is_empty = true;
#endif
    return aw_spawn_tuple_impl<false, Awaitable...>(
      std::move(wrapped), executor, continuation_executor, prio,
      doSymmetricTransfer
    );
  }

#if !defined(NDEBUG)
  ~aw_spawn_tuple() noexcept {
    if constexpr (Count != 0) {
      // This must be used, moved-from, or submitted for execution
      // in some way before destruction.
      assert(is_empty && "You must submit or co_await this.");
    }
  }
#endif
  aw_spawn_tuple(const aw_spawn_tuple&) = delete;
  aw_spawn_tuple& operator=(const aw_spawn_tuple&) = delete;
  aw_spawn_tuple(aw_spawn_tuple&& Other)
      : wrapped(std::move(Other.wrapped)), executor(std::move(Other.executor)),
        continuation_executor(std::move(Other.continuation_executor)),
        prio(Other.prio) {
#if !defined(NDEBUG)
    is_empty = Other.is_empty;
    Other.is_empty = true;
#endif
  }
  aw_spawn_tuple& operator=(aw_spawn_tuple&& Other) {
    wrapped = std::move(Other.wrapped);
    executor = std::move(Other.executor);
    continuation_executor = std::move(Other.continuation_executor);
    prio = Other.prio;
#if !defined(NDEBUG)
    is_empty = Other.is_empty;
    Other.is_empty = true;
#endif
    return *this;
  }

  /// Initiates the wrapped operations immediately. They cannot be awaited
  /// afterward. Precondition: Every wrapped operation must return void.
  void detach()
    requires(std::is_void_v<tmc::detail::awaitable_result_t<Awaitable>> && ...)
  {
    if constexpr (Count == 0) {
      return;
    }

    std::array<work_item, WorkItemCount> taskArr;

    size_t taskIdx = 0;
    [&]<std::size_t... I>(std::index_sequence<I...>) {
      (([&]() {
         if constexpr (tmc::detail::get_awaitable_traits<
                         std::tuple_element_t<I, AwaitableTuple>>::mode ==
                         tmc::detail::TMC_TASK ||
                       tmc::detail::get_awaitable_traits<
                         std::tuple_element_t<I, AwaitableTuple>>::mode ==
                         tmc::detail::COROUTINE) {
           taskArr[taskIdx] = std::get<I>(std::move(wrapped));
           ++taskIdx;
         } else if constexpr (tmc::detail::get_awaitable_traits<
                                std::tuple_element_t<I, AwaitableTuple>>::
                                mode == tmc::detail::ASYNC_INITIATE) {
           tmc::detail::get_awaitable_traits<
             std::tuple_element_t<I, AwaitableTuple>>::
             async_initiate(std::get<I>(std::move(wrapped)), executor, prio);
         } else {
           // wrap any unknown awaitable into a task
           taskArr[taskIdx] =
             tmc::detail::safe_wrap(std::get<I>(std::move(wrapped)));
           ++taskIdx;
         }
       }()),
       ...);
    }(std::make_index_sequence<Count>{});

#ifndef NDEBUG
    if constexpr (Count != 0) {
      // Ensure that this was not previously moved-from
      assert(!is_empty && "You may only submit or co_await this once.");
    }
    is_empty = true;
#endif
    tmc::detail::post_bulk_checked(
      executor, taskArr.data(), WorkItemCount, prio
    );
  }

  /// Submits the tasks to the executor immediately, without suspending the
  /// current coroutine. You must join the forked tasks by awaiting the returned
  /// awaitable before it goes out of scope.
  [[nodiscard(
    "You must co_await the fork() awaitable before it goes out of scope."
  )]] inline aw_spawn_tuple_fork<Awaitable...>
  fork() && {

#ifndef NDEBUG
    if constexpr (Count != 0) {
      // Ensure that this was not previously moved-from
      assert(!is_empty && "You may only submit or co_await this once.");
    }
    is_empty = true;
#endif
    return aw_spawn_tuple_fork<Awaitable...>(
      std::move(wrapped), executor, continuation_executor, prio, false
    );
  }

  /// Rather than waiting for all results at once, each result will be made
  /// available immediately as it becomes ready. Each time this is co_awaited,
  /// it will return the index of a single ready result. The result indexes
  /// correspond to the indexes of the originally submitted tasks, and the
  /// values can be accessed using `.get<index>()`. Results may become ready
  /// in any order, but when awaited repeatedly, each index from
  /// `[0..task_count)` will be returned exactly once. You must await this
  /// repeatedly until all results have been consumed, at which point the index
  /// returned will be equal to the value of `end()`.
  inline aw_spawn_tuple_each<Awaitable...> result_each() && {
#ifndef NDEBUG
    if constexpr (Count != 0) {
      // Ensure that this was not previously moved-from
      assert(!is_empty && "You may only submit or co_await this once.");
    }
    is_empty = true;
#endif
    return aw_spawn_tuple_each<Awaitable...>(
      std::move(wrapped), executor, continuation_executor, prio, false
    );
  }
};

/// Spawns multiple awaitables and returns an awaiter that allows you to await
/// all of the results. These awaitables may have different return types, and
/// the results will be returned in a tuple. If void-returning awaitable is
/// submitted, its result type will be replaced with std::monostate in the
/// tuple.
///
/// Does not support non-awaitable types (such as regular functors).
template <typename... Awaitable>
aw_spawn_tuple<Awaitable...> spawn_tuple(Awaitable&&... Tasks) {
  return aw_spawn_tuple<Awaitable...>(
    std::forward_as_tuple(std::forward<Awaitable>(Tasks)...)
  );
}

/// Spawns multiple awaitables and returns an awaiter that allows you to await
/// all of the results. These awaitables may have different return types, and
/// the results will be returned in a tuple. If a void-returning awaitable is
/// submitted, its result type will be replaced with std::monostate in the
/// tuple.
///
/// Does not support non-awaitable types (such as regular functors).
template <typename... Awaitable>
aw_spawn_tuple<Awaitable...> spawn_tuple(std::tuple<Awaitable...>&& Tasks) {
  return aw_spawn_tuple<Awaitable...>(
    std::forward<std::tuple<Awaitable...>>(Tasks)
  );
}

namespace detail {

template <typename... Awaitables>
struct awaitable_traits<aw_spawn_tuple<Awaitables...>> {
  static constexpr configure_mode mode = WRAPPER;

  using result_type = std::tuple<tmc::detail::void_to_monostate<
    typename tmc::detail::get_awaitable_traits<Awaitables>::result_type>...>;
  using self_type = aw_spawn_tuple<Awaitables...>;
  using awaiter_type = aw_spawn_tuple_impl<false, Awaitables...>;

  static awaiter_type get_awaiter(self_type&& Awaitable) {
    return std::forward<self_type>(Awaitable).operator co_await();
  }
};

template <typename... Result>
struct awaitable_traits<aw_spawn_tuple_each<Result...>> {
  static constexpr configure_mode mode = WRAPPER;

  using result_type = size_t;
  using self_type = aw_spawn_tuple_each<Result...>;
  using awaiter_type = self_type;

  static awaiter_type& get_awaiter(self_type& Awaitable) { return Awaitable; }
};

} // namespace detail

} // namespace tmc
