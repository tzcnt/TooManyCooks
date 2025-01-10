// Copyright (c) 2023-2024 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/aw_run_early.hpp"
#include "tmc/detail/concepts.hpp" // IWYU pragma: keep
#include "tmc/detail/mixins.hpp"
#include "tmc/detail/spawn_task_tuple_each.hpp"
#include "tmc/detail/thread_locals.hpp"
#include "tmc/task.hpp"

#include <cassert>
#include <coroutine>
#include <tuple>
#include <type_traits>

namespace tmc {

namespace detail {
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

} // namespace detail

template <typename... Awaitable> class aw_spawned_task_tuple_impl {
  static constexpr auto Count = sizeof...(Awaitable);

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
  using WorkItemTuple = detail::predicate_partition<
    treat_as_coroutine, std::tuple, Awaitable...>::true_types;

  std::coroutine_handle<> symmetric_task;
  std::coroutine_handle<> continuation;
  tmc::detail::type_erased_executor* continuation_executor;
  std::atomic<int64_t> done_count;
  using ResultTuple = std::tuple<detail::void_to_monostate<
    typename tmc::detail::awaitable_traits<Awaitable>::result_type>...>;
  ResultTuple result;
  friend aw_spawned_task_tuple<Awaitable...>;

  // coroutines are prepared and stored in an array, then submitted in bulk
  template <typename T>
  TMC_FORCE_INLINE inline void prepare_task(
    T&& Task,
    tmc::detail::void_to_monostate<
      typename tmc::detail::awaitable_traits<T>::result_type>* TaskResult,
    work_item& Task_out
  ) {
    tmc::detail::awaitable_traits<T>::set_continuation(Task, &continuation);
    tmc::detail::awaitable_traits<T>::set_continuation_executor(
      Task, &continuation_executor
    );
    tmc::detail::awaitable_traits<T>::set_done_count(Task, &done_count);
    if constexpr (!std::is_void_v<
                    typename tmc::detail::awaitable_traits<T>::result_type>) {
      tmc::detail::awaitable_traits<T>::set_result_ptr(Task, TaskResult);
    }
    Task_out = std::move(Task);
  }

  // unknown awaitables are wrapped into a task and then treated as tasks
  template <typename T>
  TMC_FORCE_INLINE inline void prepare_unknown(
    T&& Task,
    tmc::detail::void_to_monostate<
      typename tmc::detail::awaitable_traits<T>::result_type>* TaskResult,
    work_item& Task_out
  ) {
    prepare_task(
      [](T UnknownAwaitable
      ) -> tmc::task<typename tmc::detail::awaitable_traits<T>::result_type> {
        co_return co_await UnknownAwaitable;
      }(static_cast<T&&>(Task)),
      TaskResult, Task_out
    );
  }

  // awaitables are submitted individually
  template <typename T>
  TMC_FORCE_INLINE inline void prepare_awaitable(
    T&& Task,
    tmc::detail::void_to_monostate<
      typename tmc::detail::awaitable_traits<T>::result_type>* TaskResult
  ) {
    tmc::detail::awaitable_traits<T>::set_continuation(Task, &continuation);
    tmc::detail::awaitable_traits<T>::set_continuation_executor(
      Task, &continuation_executor
    );
    tmc::detail::awaitable_traits<T>::set_done_count(Task, &done_count);
    if constexpr (!std::is_void_v<
                    typename tmc::detail::awaitable_traits<T>::result_type>) {
      tmc::detail::awaitable_traits<T>::set_result_ptr(Task, TaskResult);
    }
  }

  aw_spawned_task_tuple_impl(
    std::tuple<Awaitable...>&& Tasks,
    tmc::detail::type_erased_executor* Executor,
    tmc::detail::type_erased_executor* ContinuationExecutor, size_t Prio,
    bool DoSymmetricTransfer
  )
      : symmetric_task{nullptr}, continuation_executor{ContinuationExecutor},
        done_count{0} {
    if constexpr (Count == 0) {
      return;
    }

    std::array<work_item, std::tuple_size<WorkItemTuple>::value> taskArr;

    // Prepare each task as if I loops from [0..Count),
    // but using compile-time indexes and types.
    size_t taskIdx = 0;
    [&]<std::size_t... I>(std::index_sequence<I...>) {
      (([&]() {
         if constexpr (tmc::detail::awaitable_traits<std::tuple_element_t<
                         I, std::tuple<Awaitable...>>>::mode ==
                       tmc::detail::COROUTINE) {
           prepare_task(
             std::get<I>(std::move(Tasks)), &std::get<I>(result),
             taskArr[taskIdx]
           );
           ++taskIdx;
         } else if constexpr (tmc::detail::awaitable_traits<
                                std::tuple_element_t<
                                  I, std::tuple<Awaitable...>>>::mode ==
                              tmc::detail::ASYNC_INITIATE) {
           prepare_awaitable(
             std::get<I>(std::move(Tasks)), &std::get<I>(result)
           );
         } else {
           prepare_unknown(
             std::get<I>(std::move(Tasks)), &std::get<I>(result),
             taskArr[taskIdx]
           );
           ++taskIdx;
         }
       }()),
       ...);
    }(std::make_index_sequence<Count>{});

    // Bulk submit the coroutines
    if constexpr (std::tuple_size<WorkItemTuple>::value != 0) {
      if (DoSymmetricTransfer) {
        symmetric_task = TMC_WORK_ITEM_AS_STD_CORO(taskArr[Count - 1]);
      }
      auto postCount = DoSymmetricTransfer ? Count - 1 : Count;
      done_count.store(
        static_cast<int64_t>(postCount), std::memory_order_release
      );

      if (postCount != 0) {
        tmc::detail::post_bulk_checked(
          Executor, taskArr.data(), postCount, Prio
        );
      }
    } else {
      done_count.store(static_cast<int64_t>(Count), std::memory_order_release);
    }

    // Individually initiate the awaitables
    [&]<std::size_t... I>(std::index_sequence<I...>) {
      (([&]() {
         if constexpr (!treat_as_coroutine<std::tuple_element_t<
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
  inline bool await_ready() const noexcept { return false; }

  /// Suspends the outer coroutine, submits the wrapped task to the
  /// executor, and waits for it to complete.
  TMC_FORCE_INLINE inline std::coroutine_handle<>
  await_suspend(std::coroutine_handle<> Outer) noexcept {
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
  /// void, its slot is represented by a std::monostate.
  inline ResultTuple&& await_resume() noexcept { return std::move(result); }
};

template <typename... Result>
using aw_spawned_task_tuple_run_early =
  tmc::detail::rvalue_only_awaitable<aw_spawned_task_tuple_impl<Result...>>;

// Primary template is forward-declared in "tmc/detail/aw_run_early.hpp".
template <typename... Awaitable>
class [[nodiscard(
  "You must use the aw_spawned_task_tuple<Awaitable> by one of: 1. "
  "co_await 2. run_early()"
)]] aw_spawned_task_tuple
    : public tmc::detail::run_on_mixin<aw_spawned_task_tuple<Awaitable...>>,
      public tmc::detail::resume_on_mixin<aw_spawned_task_tuple<Awaitable...>>,
      public tmc::detail::with_priority_mixin<
        aw_spawned_task_tuple<Awaitable...>> {
  friend class tmc::detail::run_on_mixin<aw_spawned_task_tuple<Awaitable...>>;
  friend class tmc::detail::resume_on_mixin<
    aw_spawned_task_tuple<Awaitable...>>;
  friend class tmc::detail::with_priority_mixin<
    aw_spawned_task_tuple<Awaitable...>>;

  static constexpr auto Count = sizeof...(Awaitable);
  std::tuple<Awaitable...> wrapped;
  tmc::detail::type_erased_executor* executor;
  tmc::detail::type_erased_executor* continuation_executor;
  size_t prio;

public:
  /// It is recommended to call `spawn()` instead of using this constructor
  /// directly.
  aw_spawned_task_tuple(std::tuple<Awaitable&&...> Tasks)
      : wrapped(std::move(Tasks)), executor(tmc::detail::this_thread::executor),
        continuation_executor(tmc::detail::this_thread::executor),
        prio(tmc::detail::this_thread::this_task.prio) {}

  aw_spawned_task_tuple_impl<Awaitable...> operator co_await() && {
    bool doSymmetricTransfer = tmc::detail::this_thread::exec_is(executor) &&
                               tmc::detail::this_thread::prio_is(prio);
    return aw_spawned_task_tuple_impl<Awaitable...>(
      std::move(wrapped), executor, continuation_executor, prio,
      doSymmetricTransfer
    );
  }

  // #if !defined(NDEBUG) && !defined(TMC_TRIVIAL_TASK)
  //   ~aw_spawned_task_tuple() noexcept {
  //     if constexpr (Count > 0) {
  //       // If you spawn a task that returns a non-void type,
  //       // then you must co_await the return of spawn!
  //       assert(!std::get<0>(wrapped));
  //     }
  //   }
  // #endif
  aw_spawned_task_tuple(const aw_spawned_task_tuple&) = delete;
  aw_spawned_task_tuple& operator=(const aw_spawned_task_tuple&) = delete;
  aw_spawned_task_tuple(aw_spawned_task_tuple&& Other)
      : wrapped(std::move(Other.wrapped)), executor(std::move(Other.executor)),
        continuation_executor(std::move(Other.continuation_executor)),
        prio(Other.prio) {}
  aw_spawned_task_tuple& operator=(aw_spawned_task_tuple&& Other) {
    wrapped = std::move(Other.wrapped);
    executor = std::move(Other.executor);
    continuation_executor = std::move(Other.continuation_executor);
    prio = Other.prio;
    return *this;
  }

  /// Submits the tasks to the executor immediately. They cannot be awaited
  /// afterward.
  // void detach()
  //   requires(
  //     std::is_void_v<
  //       typename tmc::detail::awaitable_traits<Awaitable>::result_type> &&
  //     ...
  //   )
  // {
  //   if constexpr (Count == 0) {
  //     return;
  //   }
  //   std::array<work_item, Count> taskArr;

  //   [&]<std::size_t... I>(std::index_sequence<I...>) {
  //     ((taskArr[I] = std::get<I>(std::move(wrapped))), ...);
  //   }(std::make_index_sequence<Count>{});

  //   tmc::detail::post_bulk_checked(executor, taskArr.data(), Count, prio);
  // }

  /// Submits the tasks to the executor immediately, without suspending the
  /// current coroutine. You must await the return type before destroying it.
  inline aw_spawned_task_tuple_run_early<Awaitable...> run_early() && {
    return aw_spawned_task_tuple_run_early<Awaitable...>(
      std::move(wrapped), executor, continuation_executor, prio, false
    );
  }

  /// Rather than waiting for all results at once, each result will be made
  /// available immediately as it becomes ready. Each time this is co_awaited,
  /// it will return the index of a single ready result. The result indexes
  /// correspond to the indexes of the originally submitted tasks, and the
  /// values can be accessed using `.get<index>()`. Results may become ready in
  /// any order, but when awaited repeatedly, each index from
  /// `[0..task_count)` will be returned exactly once. You must await this
  /// repeatedly until all tasks are complete, at which point the index returned
  /// will be equal to the value of `end()`.
  //   inline aw_spawned_task_tuple_each<Awaitable...> each() && {
  // #ifndef NDEBUG
  //     if constexpr (Count > 0) {
  //       // Ensure that this was not previously moved-from
  //       assert(std::get<0>(wrapped));
  //     }
  // #endif
  //     return aw_spawned_task_tuple_each<Awaitable...>(
  //       std::move(wrapped), executor, continuation_executor, prio
  //     );
  //   }
};

/// Spawns multiple tasks and returns an awaiter that allows you to await all of
/// the results. These tasks may have different return types, and the results
/// will be returned in a tuple. If a task<void> is submitted, its result type
/// will be replaced with std::monostate in the tuple.
template <typename... Awaitable>
aw_spawned_task_tuple<Awaitable...> spawn_tuple(Awaitable&&... Tasks) {
  return aw_spawned_task_tuple<Awaitable...>(
    std::forward_as_tuple(std::forward<Awaitable>(Tasks)...)
  );
}

/// Spawns multiple tasks and returns an awaiter that allows you to await all of
/// the results. These tasks may have different return types, and the results
/// will be returned in a tuple. If a task<void> is submitted, its result type
/// will be replaced with std::monostate in the tuple.
template <typename... Result>
aw_spawned_task_tuple<Result...> spawn_tuple(std::tuple<task<Result>...>&& Tasks
) {
  return aw_spawned_task_tuple<Result...>(
    std::forward<std::tuple<task<Result>...>>(Tasks)
  );
}

} // namespace tmc
