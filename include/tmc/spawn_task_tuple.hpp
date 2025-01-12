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

template <typename... Awaitable> class aw_spawned_task_tuple_impl {
  static constexpr auto Count = sizeof...(Awaitable);

  static constexpr size_t WorkItemCount =
    std::tuple_size_v<typename tmc::detail::predicate_partition<
      tmc::detail::treat_as_coroutine, std::tuple, Awaitable...>::true_types>;

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
           // Wrap any unknown awaitable into a task
           prepare_task(
             tmc::detail::safe_wrap(std::get<I>(std::move(Tasks))),
             &std::get<I>(result), taskArr[taskIdx]
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
      done_count.store(
        static_cast<int64_t>(doneCount), std::memory_order_release
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

  static constexpr size_t WorkItemCount =
    std::tuple_size_v<typename tmc::detail::predicate_partition<
      tmc::detail::treat_as_coroutine, std::tuple, Awaitable...>::true_types>;

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
    auto localExecutor = executor;
#ifndef NDEBUG
    if constexpr (Count != 0) {
      // Ensure that this was not previously moved-from
      assert(executor != nullptr);
    }
    executor = nullptr; // signal that we initiated the work in some way
#endif
    return aw_spawned_task_tuple_impl<Awaitable...>(
      std::move(wrapped), localExecutor, continuation_executor, prio,
      doSymmetricTransfer
    );
  }

#if !defined(NDEBUG)
  ~aw_spawned_task_tuple() noexcept {
    if constexpr (Count != 0) {
      // You must submit this for execution before destroying it.
      // If this assertion fails, it is because you did not submit this.
      assert(executor == nullptr);
    }
  }
#endif
  aw_spawned_task_tuple(const aw_spawned_task_tuple&) = delete;
  aw_spawned_task_tuple& operator=(const aw_spawned_task_tuple&) = delete;
  aw_spawned_task_tuple(aw_spawned_task_tuple&& Other)
      : wrapped(std::move(Other.wrapped)), executor(std::move(Other.executor)),
        continuation_executor(std::move(Other.continuation_executor)),
        prio(Other.prio) {
#if !defined(NDEBUG)
    Other.executor = nullptr;
#endif
  }
  aw_spawned_task_tuple& operator=(aw_spawned_task_tuple&& Other) {
    wrapped = std::move(Other.wrapped);
    executor = std::move(Other.executor);
    continuation_executor = std::move(Other.continuation_executor);
    prio = Other.prio;
#if !defined(NDEBUG)
    Other.executor = nullptr;
#endif
    return *this;
  }

  /// Initiates the wrapped operations immediately. They cannot be awaited
  /// afterward. Precondition: Every wrapped operation must return void.
  void detach()
    requires(
      std::is_void_v<
        typename tmc::detail::awaitable_traits<Awaitable>::result_type> &&
      ...
    )
  {
    if constexpr (Count == 0) {
      return;
    }

    std::array<work_item, WorkItemCount> taskArr;

    size_t taskIdx = 0;
    [&]<std::size_t... I>(std::index_sequence<I...>) {
      (([&]() {
         if constexpr (tmc::detail::awaitable_traits<std::tuple_element_t<
                         I, std::tuple<Awaitable...>>>::mode ==
                       tmc::detail::COROUTINE) {
           taskArr[taskIdx] = std::get<I>(std::move(wrapped));
           ++taskIdx;
         } else if constexpr (tmc::detail::awaitable_traits<
                                std::tuple_element_t<
                                  I, std::tuple<Awaitable...>>>::mode ==
                              tmc::detail::ASYNC_INITIATE) {
           tmc::detail::awaitable_traits<
             std::tuple_element_t<I, std::tuple<Awaitable...>>>::
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

    [[maybe_unused]] auto localExecutor = executor;
#ifndef NDEBUG
    if constexpr (Count != 0) {
      // Ensure that this was not previously moved-from
      assert(executor != nullptr);
    }
    executor = nullptr; // signal that we initiated the work in some way
#endif
    if constexpr (WorkItemCount != 0) {
      tmc::detail::post_bulk_checked(
        localExecutor, taskArr.data(), WorkItemCount, prio
      );
    }
  }

  /// Submits the tasks to the executor immediately, without suspending the
  /// current coroutine. You must await the return type before destroying it.
  inline aw_spawned_task_tuple_run_early<Awaitable...> run_early() && {
    auto localExecutor = executor;
#ifndef NDEBUG
    if constexpr (Count != 0) {
      // Ensure that this was not previously moved-from
      assert(executor != nullptr);
    }
    executor = nullptr; // signal that we initiated the work in some way
#endif
    return aw_spawned_task_tuple_run_early<Awaitable...>(
      std::move(wrapped), localExecutor, continuation_executor, prio, false
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
  inline aw_spawned_task_tuple_each<Awaitable...> each() && {
    auto localExecutor = executor;
#ifndef NDEBUG
    if constexpr (Count != 0) {
      // Ensure that this was not previously moved-from
      assert(executor != nullptr);
    }
    executor = nullptr; // signal that we initiated the work in some way
#endif
    return aw_spawned_task_tuple_each<Awaitable...>(
      std::move(wrapped), localExecutor, continuation_executor, prio
    );
  }
};

/// Spawns multiple awaitables and returns an awaiter that allows you to await
/// all of the results. These awaitables may have different return types, and
/// the results will be returned in a tuple. If void-returning awaitable is
/// submitted, its result type will be replaced with std::monostate in the
/// tuple.
template <typename... Awaitable>
aw_spawned_task_tuple<Awaitable...> spawn_tuple(Awaitable&&... Tasks) {
  return aw_spawned_task_tuple<Awaitable...>(
    std::forward_as_tuple(std::forward<Awaitable>(Tasks)...)
  );
}

/// Spawns multiple awaitables and returns an awaiter that allows you to await
/// all of the results. These awaitables may have different return types, and
/// the results will be returned in a tuple. If a void-returning awaitable is
/// submitted, its result type will be replaced with std::monostate in the
/// tuple.
template <typename... Awaitable>
aw_spawned_task_tuple<Awaitable...> spawn_tuple(std::tuple<Awaitable...>&& Tasks
) {
  return aw_spawned_task_tuple<Awaitable...>(
    std::forward<std::tuple<Awaitable...>>(Tasks)
  );
}

namespace detail {

template <typename... Awaitables>
struct awaitable_traits<aw_spawned_task_tuple<Awaitables...>> {
  static constexpr awaitable_mode mode = UNKNOWN;

  using result_type = std::tuple<detail::void_to_monostate<
    typename tmc::detail::awaitable_traits<Awaitables>::result_type>...>;
  using self_type = aw_spawned_task_tuple<Awaitables...>;
  using awaiter_type = aw_spawned_task_tuple_impl<Awaitables...>;

  static awaiter_type get_awaiter(self_type&& Awaitable) {
    return std::forward<self_type>(Awaitable).operator co_await();
  }
};

} // namespace detail

} // namespace tmc
