// Copyright (c) 2023-2024 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/aw_run_early.hpp"
#include "tmc/detail/concepts.hpp" // IWYU pragma: keep
#include "tmc/detail/thread_locals.hpp"
#include "tmc/task.hpp"

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

template <typename... T> using last_type_t = last_type<T...>::type;

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
    tmc::detail::awaitable_traits<T>::mode == tmc::detail::COROUTINE ||
    tmc::detail::awaitable_traits<T>::mode == tmc::detail::UNKNOWN;
};
} // namespace detail

template <typename... Awaitable> class aw_spawned_task_tuple_each_impl {
  static constexpr auto Count = sizeof...(Awaitable);

  static constexpr size_t WorkItemCount =
    std::tuple_size_v<typename tmc::detail::predicate_partition<
      tmc::detail::treat_as_coroutine, std::tuple, Awaitable...>::true_types>;

  // This class uses an atomic bitmask with only 63 slots for tasks.
  // each() doesn't seem like a good fit for larger task groups anyway.
  // If you really need this, please open a GitHub issue explaining why...
  static_assert(Count < 64);

  std::coroutine_handle<> continuation;
  tmc::detail::type_erased_executor* continuation_executor;
  std::atomic<uint64_t> sync_flags;
  int64_t remaining_count;
  using ResultTuple = std::tuple<detail::void_to_monostate<
    typename tmc::detail::awaitable_traits<Awaitable>::result_type>...>;
  ResultTuple result;
  friend aw_spawned_task_tuple<Awaitable...>;

  template <typename T>
  TMC_FORCE_INLINE inline void prepare_task(
    T&& Task,
    tmc::detail::void_to_monostate<
      typename tmc::detail::awaitable_traits<T>::result_type>* TaskResult,
    size_t I, work_item& Task_out
  ) {
    tmc::detail::awaitable_traits<T>::set_continuation(Task, &continuation);
    tmc::detail::awaitable_traits<T>::set_continuation_executor(
      Task, &continuation_executor
    );
    tmc::detail::awaitable_traits<T>::set_done_count(Task, &sync_flags);
    tmc::detail::awaitable_traits<T>::set_flags(
      Task, tmc::detail::task_flags::EACH | I
    );
    if constexpr (!std::is_void_v<T>) {
      tmc::detail::awaitable_traits<T>::set_result_ptr(Task, TaskResult);
    }
    Task_out = std::move(Task);
  }

  template <typename T>
  TMC_FORCE_INLINE inline void prepare_awaitable(
    T&& Task,
    tmc::detail::void_to_monostate<
      typename tmc::detail::awaitable_traits<T>::result_type>* TaskResult,
    size_t I
  ) {
    tmc::detail::awaitable_traits<T>::set_continuation(Task, &continuation);
    tmc::detail::awaitable_traits<T>::set_continuation_executor(
      Task, &continuation_executor
    );
    tmc::detail::awaitable_traits<T>::set_done_count(Task, &sync_flags);
    tmc::detail::awaitable_traits<T>::set_flags(
      Task, tmc::detail::task_flags::EACH | I
    );
    if constexpr (!std::is_void_v<T>) {
      tmc::detail::awaitable_traits<T>::set_result_ptr(Task, TaskResult);
    }
  }

  aw_spawned_task_tuple_each_impl(
    std::tuple<Awaitable...>&& Tasks,
    tmc::detail::type_erased_executor* Executor,
    tmc::detail::type_erased_executor* ContinuationExecutor, size_t Prio
  )
      : continuation_executor{ContinuationExecutor}, sync_flags{0},
        remaining_count{0} {
    if (Count == 0) {
      return;
    }
    std::array<work_item, WorkItemCount> taskArr;

    // Prepare each task as if I loops from [0..Count),
    // but using compile-time indexes and types.
    size_t taskIdx = 0;
    [&]<std::size_t... I>(std::index_sequence<I...>) {
      (([&]() {
         if constexpr (tmc::detail::awaitable_traits<std::tuple_element_t<
                         I, std::tuple<Awaitable...>>>::mode ==
                       tmc::detail::COROUTINE) {
           prepare_task(
             std::get<I>(std::move(Tasks)), &std::get<I>(result), I,
             taskArr[taskIdx]
           );
           ++taskIdx;
         } else if constexpr (tmc::detail::awaitable_traits<
                                std::tuple_element_t<
                                  I, std::tuple<Awaitable...>>>::mode ==
                              tmc::detail::ASYNC_INITIATE) {
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

    remaining_count = Count;
    sync_flags.store(tmc::detail::task_flags::EACH, std::memory_order_release);

    // Bulk submit the coroutines
    if constexpr (WorkItemCount != 0) {
      tmc::detail::post_bulk_checked(
        Executor, taskArr.data(), WorkItemCount, Prio
      );
    }

    // Individually initiate the awaitables
    [&]<std::size_t... I>(std::index_sequence<I...>) {
      (([&]() {
         if constexpr (!tmc::detail::treat_as_coroutine<std::tuple_element_t<
                         I, std::tuple<Awaitable...>>>::value) {
           tmc::detail::awaitable_traits<
             std::tuple_element_t<I, std::tuple<Awaitable...>>>::
             async_initiate(std::get<I>(std::move(Tasks)), Executor, Prio);
         }
       }()),
       ...);
    }(std::make_index_sequence<Count>{});
  }

public:
  /// Always suspends.
  inline bool await_ready() const noexcept {
    if (remaining_count == 0) {
      return true;
    }
    auto resumeState = sync_flags.load(std::memory_order_acquire);
    // High bit is set, because we are running
    assert((resumeState & tmc::detail::task_flags::EACH) != 0);
    auto readyBits = resumeState & ~tmc::detail::task_flags::EACH;
    return readyBits != 0;
  }

  /// Suspends the outer coroutine, submits the wrapped task to the
  /// executor, and waits for it to complete.
  TMC_FORCE_INLINE inline bool await_suspend(std::coroutine_handle<> Outer
  ) noexcept {
    continuation = Outer;
  // This logic is necessary because we submitted all child tasks before the
  // parent suspended. Allowing parent to be resumed before it suspends
  // would be UB. Therefore we need to block the resumption until here.
  // WARNING: We can use fetch_sub here because we know this bit wasn't set.
  // It generates xadd instruction which is slightly more efficient than
  // fetch_or. But not safe to use if the bit might already be set.
  TRY_SUSPEND:
    auto resumeState = sync_flags.fetch_sub(
      tmc::detail::task_flags::EACH, std::memory_order_acq_rel
    );
    assert((resumeState & tmc::detail::task_flags::EACH) != 0);
    auto readyBits = resumeState & ~tmc::detail::task_flags::EACH;
    if (readyBits == 0) {
      return true; // we suspended and no tasks were ready
    }
    // A result became ready, so try to resume immediately.
    auto resumeState2 = sync_flags.fetch_or(
      tmc::detail::task_flags::EACH, std::memory_order_acq_rel
    );
    bool didResume = (resumeState2 & tmc::detail::task_flags::EACH) == 0;
    if (!didResume) {
      return true; // Another thread already resumed
    }
    auto readyBits2 = resumeState2 & ~tmc::detail::task_flags::EACH;
    if (readyBits2 == 0) {
      // We resumed but another thread already consumed all the results
      goto TRY_SUSPEND;
    }
    if (continuation_executor != nullptr &&
        !tmc::detail::this_thread::exec_is(continuation_executor)) {
      // Need to resume on a different executor
      tmc::detail::post_checked(
        continuation_executor, std::move(Outer),
        tmc::detail::this_thread::this_task.prio
      );
      return true;
    }
    return false; // OK to resume inline
  }

  /// Returns the value provided by the wrapped tasks.
  /// Each task has a slot in the tuple. If the task would return void, its
  /// slot is represented by a std::monostate.
  inline size_t await_resume() noexcept {
    if (remaining_count == 0) {
      return end();
    }
    uint64_t resumeState = sync_flags.load(std::memory_order_acquire);
    assert((resumeState & tmc::detail::task_flags::EACH) != 0);
    // High bit is set, because we are resuming
    uint64_t slots = resumeState & ~tmc::detail::task_flags::EACH;
    assert(slots != 0);
#ifdef _MSC_VER
    size_t slot = static_cast<size_t>(_tzcnt_u64(slots));
#else
    size_t slot = static_cast<size_t>(__builtin_ctzll(slots));
#endif
    --remaining_count;
    sync_flags.fetch_sub(1ULL << slot, std::memory_order_release);
    return slot;
  }

  /// Provides a sentinel value that can be compared against the value returned
  /// from co_await.
  inline size_t end() noexcept { return Count + 1; }

  // Gets the ready result at the given index.
  template <size_t I>
  inline std::tuple_element_t<I, ResultTuple>& get() noexcept {
    return std::get<I>(result);
  }
};

template <typename... Result>
using aw_spawned_task_tuple_each = aw_spawned_task_tuple_each_impl<Result...>;

namespace detail {

template <typename... Result>
struct awaitable_traits<aw_spawned_task_tuple_each<Result...>> {
  static constexpr awaitable_mode mode = UNKNOWN;

  using result_type = size_t;
  using self_type = aw_spawned_task_tuple_each<Result...>;
  using awaiter_type = self_type;

  static awaiter_type get_awaiter(self_type&& Awaitable) {
    return std::forward<self_type>(Awaitable);
  }
};
} // namespace detail

} // namespace tmc
