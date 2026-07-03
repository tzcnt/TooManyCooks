// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/compat.hpp"
#include "tmc/detail/concepts_awaitable.hpp" // IWYU pragma: keep
#include "tmc/detail/concepts_work_item.hpp"
#include "tmc/detail/mixins.hpp"
#include "tmc/detail/thread_locals.hpp"
#include "tmc/detail/tuple_helpers.hpp"
#include "tmc/ex_any.hpp"
#include "tmc/work_item.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <coroutine>
#include <tuple>
#include <type_traits>

namespace tmc {
/// The customizable task wrapper / awaitable type returned by
/// `tmc::spawn_tuple()`.
template <typename... Result> class aw_spawn_tuple;

template <typename... Awaitable> class aw_spawn_tuple_impl {
  static constexpr auto Count = sizeof...(Awaitable);

  static constexpr size_t WorkItemCount =
    std::tuple_size_v<typename tmc::detail::predicate_partition<
      tmc::detail::treat_as_coroutine, std::tuple, Awaitable...>::true_types>;

  // If the last task is to be run immediately via symmetric transfer, it is
  // stored here instead of being posted to the executor.
  std::coroutine_handle<> symmetric_task;
  std::coroutine_handle<> continuation;
  tmc::ex_any* continuation_executor;
  // the atomic synchronization variable that coordinates between this and
  // awaitable_customizer (task's final_suspend)
  std::atomic<ptrdiff_t> done_count;

  template <typename T>
  using ResultStorage = tmc::detail::result_storage_t<tmc::detail::void_to_monostate<
    typename tmc::detail::get_awaitable_traits<T>::result_type>>;
  using ResultTuple = std::tuple<ResultStorage<Awaitable>...>;
  ResultTuple result;

  using AwaitableTuple = std::tuple<Awaitable...>;

  friend aw_spawn_tuple<Awaitable...>;

  // coroutines are prepared and stored in an array, then submitted in bulk
  template <typename T>
  TMC_FORCE_INLINE inline void
  prepare_task(T&& Task, ResultStorage<T>* TaskResult, work_item& Task_out) {
    tmc::detail::get_awaitable_traits<T>::set_continuation(Task, &continuation);
    tmc::detail::get_awaitable_traits<T>::set_continuation_executor(
      Task, &continuation_executor
    );
    tmc::detail::get_awaitable_traits<T>::set_done_count(Task, &done_count);
    if constexpr (!std::is_void_v<
                    typename tmc::detail::get_awaitable_traits<T>::result_type>) {
      tmc::detail::get_awaitable_traits<T>::set_result_ptr(Task, TaskResult);
    }

    // This type erasure is necessary when TMC_WORK_ITEM=FUNC,
    // so that func.target <std::coroutine_handle<>>() works. Otherwise,
    // the func target would be of the real type (tmc::task).
    Task_out = std::coroutine_handle<>(static_cast<T&&>(Task));
  }

  // awaitables are submitted individually
  template <typename T>
  TMC_FORCE_INLINE inline void prepare_awaitable(T&& Task, ResultStorage<T>* TaskResult) {
    tmc::detail::get_awaitable_traits<T>::set_continuation(Task, &continuation);
    tmc::detail::get_awaitable_traits<T>::set_continuation_executor(
      Task, &continuation_executor
    );
    tmc::detail::get_awaitable_traits<T>::set_done_count(Task, &done_count);
    if constexpr (!std::is_void_v<
                    typename tmc::detail::get_awaitable_traits<T>::result_type>) {
      tmc::detail::get_awaitable_traits<T>::set_result_ptr(Task, TaskResult);
    }
  }

  void set_done_count(size_t NumTasks) {
    done_count.store(static_cast<ptrdiff_t>(NumTasks), std::memory_order_release);
  }

  aw_spawn_tuple_impl(
    AwaitableTuple&& Tasks, tmc::ex_any* Executor, tmc::ex_any* ContinuationExecutor,
    size_t Prio, bool DoSymmetricTransfer
  )
      : symmetric_task{nullptr}, continuation_executor{ContinuationExecutor} {
    std::array<work_item, WorkItemCount> taskArr;

    // Prepare each task as if I loops from [0..Count),
    // but using compile-time indexes and types.
    size_t taskIdx = 0;
    [&]<std::size_t... I>(std::index_sequence<I...>) {
      (([&]() {
         constexpr auto mode = tmc::detail::get_awaitable_traits<
           std::tuple_element_t<I, AwaitableTuple>>::mode;
         static_assert(
           mode != tmc::detail::UNKNOWN, "This doesn't appear to be an awaitable."
         );
         if constexpr (mode == tmc::detail::ASYNC_INITIATE) {
           // The std::move is a bit misleading here - this actually forwards the
           // awaitable from within the tuple with its original value category.
           prepare_awaitable(std::get<I>(std::move(Tasks)), &std::get<I>(result));
         } else {
           prepare_task(
             tmc::detail::into_known<false>(std::get<I>(std::move(Tasks))),
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
           // The std::move is a bit misleading here - this actually forwards the
           // awaitable from within the tuple with its original value category.
           tmc::detail::get_awaitable_traits<std::tuple_element_t<I, AwaitableTuple>>::
             async_initiate(std::get<I>(std::move(Tasks)), Executor, Prio);
         }
       }()),
       ...);
    }(std::make_index_sequence<Count>{});
  }

public:
  /// Always suspends.
  inline bool await_ready() const noexcept {
    // Always suspends, due to the possibility to resume on another executor.
    return false;
  }

  /// Suspends the outer coroutine, submits the wrapped task to the
  /// executor, and waits for it to complete.
  inline std::coroutine_handle<> await_suspend(std::coroutine_handle<> Outer) noexcept {
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
            tmc::detail::this_thread::this_task().prio
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
  TMC_AWAIT_RESUME inline ResultTuple&& await_resume() noexcept {
    return std::move(result);
  }

  // This must be awaited and all child tasks completed before destruction.
#ifndef NDEBUG
  ~aw_spawn_tuple_impl() noexcept {
    assert(done_count.load() < 0 && "You must submit or co_await this.");
  }
#endif

  // Not movable or copyable due to awaitables being initiated in constructor,
  // and having pointers to this.
  aw_spawn_tuple_impl& operator=(const aw_spawn_tuple_impl& other) = delete;
  aw_spawn_tuple_impl(const aw_spawn_tuple_impl& other) = delete;
  aw_spawn_tuple_impl& operator=(aw_spawn_tuple_impl&& other) = delete;
  aw_spawn_tuple_impl(aw_spawn_tuple_impl&& other) = delete;
};

template <typename... Result>
using aw_spawn_tuple_fork =
  tmc::detail::rvalue_only_awaitable<aw_spawn_tuple_impl<Result...>>;

template <typename... Awaitable>
class [[nodiscard(
  "You must await or initiate the result of spawn_tuple()."
)]] TMC_CORO_AWAIT_ELIDABLE aw_spawn_tuple
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
  aw_spawn_tuple(std::tuple<Awaitable&&...>&& Tasks)
      : wrapped(static_cast<std::tuple<Awaitable&&...>&&>(Tasks)),
        executor(tmc::detail::this_thread::executor()),
        continuation_executor(tmc::detail::this_thread::executor()),
        prio(tmc::detail::this_thread::this_task().prio)
#ifndef NDEBUG
        ,
        is_empty(false)
#endif
  {
  }

  /// Initiates all of the wrapped tasks and waits for them to complete.
  aw_spawn_tuple_impl<Awaitable...> operator co_await() && noexcept {
    bool doSymmetricTransfer = tmc::detail::this_thread::exec_prio_is(executor, prio);
#ifndef NDEBUG
    if constexpr (Count != 0) {
      // Ensure that this was not previously moved-from
      assert(!is_empty && "You may only submit or co_await this once.");
    }
    is_empty = true;
#endif
    return aw_spawn_tuple_impl<Awaitable...>(
      std::move(wrapped), executor, continuation_executor, prio, doSymmetricTransfer
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
        continuation_executor(std::move(Other.continuation_executor)), prio(Other.prio) {
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
                       tmc::detail::ASYNC_INITIATE) {
           // The std::move is a bit misleading here - this actually forwards the
           // awaitable from within the tuple with its original value category.
           tmc::detail::get_awaitable_traits<std::tuple_element_t<I, AwaitableTuple>>::
             async_initiate(std::get<I>(std::move(wrapped)), executor, prio);
         } else {
           taskArr[taskIdx] = tmc::detail::into_initiate(
             tmc::detail::into_known<false>(std::get<I>(std::move(wrapped)))
           );
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
    tmc::detail::post_bulk_checked(executor, taskArr.data(), WorkItemCount, prio);
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
};

/// Spawns multiple awaitables and returns an awaiter that allows you to await
/// all of the results. These awaitables may have different return types, and
/// the results will be returned in a tuple. If void-returning awaitable is
/// submitted, its result type will be replaced with std::monostate in the
/// tuple.
///
/// Does not support non-awaitable types (such as regular functors).
template <typename... Awaitable>
aw_spawn_tuple<tmc::detail::forward_awaitable<Awaitable>...>
spawn_tuple(TMC_CORO_AWAIT_ELIDABLE_ARGUMENT Awaitable&&... Awaitables) {
  return aw_spawn_tuple<tmc::detail::forward_awaitable<Awaitable>...>(
    std::forward_as_tuple(static_cast<Awaitable&&>(Awaitables)...)
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
aw_spawn_tuple<Awaitable...>
spawn_tuple(TMC_CORO_AWAIT_ELIDABLE_ARGUMENT std::tuple<Awaitable...>&& Awaitables) {
  return aw_spawn_tuple<Awaitable...>(
    static_cast<std::tuple<Awaitable...>&&>(Awaitables)
  );
}

namespace detail {
template <typename... Awaitables> struct awaitable_traits<aw_spawn_tuple<Awaitables...>> {
  static constexpr configure_mode mode = WRAPPER;

  using result_type = std::tuple<tmc::detail::void_to_monostate<
    typename tmc::detail::get_awaitable_traits<Awaitables>::result_type>...>;
  using self_type = aw_spawn_tuple<Awaitables...>;
  using awaiter_type = aw_spawn_tuple_impl<Awaitables...>;

  static awaiter_type get_awaiter(self_type&& Awaitable) noexcept {
    return static_cast<self_type&&>(Awaitable).operator co_await();
  }
};
} // namespace detail

/// This is a dummy awaitable. Don't store this in a variable.
/// For HALO to work, you must `co_await tmc::fork_tuple_clang()` immediately,
/// which will return the real awaitable (that you can await later to join the
/// forked task).
template <typename... Awaitable>
class TMC_CORO_AWAIT_ELIDABLE aw_fork_tuple_clang : tmc::detail::AwaitTagNoGroupAsIs {
  using AwaitableTuple = std::tuple<Awaitable...>;
  AwaitableTuple wrapped;

public:
  aw_fork_tuple_clang(std::tuple<Awaitable&&...>&& Awaitables)
      : wrapped(static_cast<std::tuple<Awaitable&&...>&&>(Awaitables)) {}

  /// Never suspends.
  inline bool await_ready() const noexcept { return true; }

  /// Never suspends.
  inline void await_suspend(std::coroutine_handle<>) noexcept {}

  /// Returns the value provided by the wrapped function.
  TMC_AWAIT_RESUME inline aw_spawn_tuple_fork<Awaitable...> await_resume() noexcept {
    return tmc::spawn_tuple<Awaitable...>(static_cast<AwaitableTuple&&>(wrapped)).fork();
  }
};

/// Similar to `tmc::spawn_tuple(Awaitables...).fork()` but allows the child
/// tasks' allocations to be elided by combining them into the parent's
/// allocation (HALO). This works by using specific attributes that are only
/// available on Clang 20+. You can safely call this function on other compilers
/// but no HALO-specific optimizations will be applied.
///
/// IMPORTANT: This returns a dummy awaitable. For HALO to work, you should
/// not store the dummy awaitable. Instead, `co_await` this expression
/// immediately, which returns the real awaitable that you
/// can use to join the forked task later. Proper usage:
/// ```
/// auto forked_task_tuple = co_await tmc::fork_tuple_clang(task1(), task2());
/// do_some_other_work();
/// auto result_tuple = co_await std::move(forked_task_tuple);
/// ```
template <typename... Awaitable>
[[nodiscard("You must co_await fork_tuple_clang() immediately for HALO to be possible.")]]
aw_fork_tuple_clang<tmc::detail::forward_awaitable<Awaitable>...>
fork_tuple_clang(TMC_CORO_AWAIT_ELIDABLE_ARGUMENT Awaitable&&... Awaitables) {
  return aw_fork_tuple_clang<tmc::detail::forward_awaitable<Awaitable>...>(
    std::forward_as_tuple(static_cast<Awaitable&&>(Awaitables)...)
  );
}

} // namespace tmc
