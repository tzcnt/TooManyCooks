// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/awaitable_customizer.hpp"
#include "tmc/detail/compat.hpp"
#include "tmc/detail/concepts_awaitable.hpp" // IWYU pragma: keep
#include "tmc/detail/concepts_work_item.hpp"
#include "tmc/detail/result_each.hpp"
#include "tmc/detail/thread_locals.hpp"
#include "tmc/detail/tuple_helpers.hpp"
#include "tmc/ex_any.hpp"
#include "tmc/work_item.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <coroutine>
#include <cstddef>
#include <tuple>
#include <type_traits>

namespace tmc {

/// A standalone, reusable result-multiplexer over a fixed set of awaitable
/// slots. Like `tmc::spawn_tuple()` it initiates a fixed set of awaitables, but
/// rather than returning all results at once, each result is made available as
/// it becomes ready: `co_await`ing the group returns the index of a single
/// ready slot. Unlike `spawn_tuple()`, passing the awaitables up front is
/// optional.
///
/// There are two ways to construct a `mux_tuple`:
///
/// 1. With awaitables (template arguments are deduced). This eagerly initiates
/// all of the provided awaitables, exactly like `tmc::spawn_tuple(awaitables...)`
/// - but their results are consumed one at a time as they become ready:
/// ```
/// tmc::mux_tuple mux(task0(), task1(), task2());
/// for (size_t i = co_await mux; i != mux.end(); i = co_await mux) { ... }
/// ```
///
/// 2. Empty (template arguments must be provided explicitly). This only
/// allocates the result storage; no awaitables are initiated. You start
/// individual slots with `restart<I>()` whenever you like:
/// ```
/// tmc::mux_tuple<tmc::task<int>, tmc::task<std::string>> mux;
/// mux.restart<0>(make_int_task());
/// mux.restart<1>(make_string_task());
/// for (size_t i = co_await mux; i != mux.end(); i = co_await mux) { ... }
/// ```
///
/// In both cases, after a slot's result has been consumed by `co_await`, you may
/// call `restart<I>()` to launch a fresh awaitable into that slot. The
/// replacement awaitable must produce the same result slot type as the slot's
/// declared awaitable. This is what allows a `mux_tuple` to maintain a fixed
/// level of concurrency: as each result is consumed, replace it with new work.
///
/// The template parameters are awaitable types (the same types you would pass to
/// `tmc::spawn_tuple()`); each one's result type determines the type of its
/// result slot. void-returning awaitables are represented by std::monostate, and
/// non-default-constructible results are wrapped in a std::optional.
template <typename... Awaitable>
class mux_tuple : private tmc::detail::AwaitTagNoGroupCoAwaitLvalue {
  static constexpr auto Count = sizeof...(Awaitable);

  // Tasks are synchronized via an atomic bitmask with only 63 (or 31, on 32-bit)
  // slots for tasks.
  static_assert(
    Count < TMC_PLATFORM_BITS,
    "mux_tuple supports at most 63 awaitables (31 on 32-bit platforms)."
  );

  static constexpr size_t WorkItemCount =
    std::tuple_size_v<typename tmc::detail::predicate_partition<
      tmc::detail::treat_as_coroutine, std::tuple, Awaitable...>::true_types>;

  // The count of submitted-but-not-yet-consumed results.
  ptrdiff_t remaining_count;
  std::coroutine_handle<> continuation;
  tmc::ex_any* executor;
  tmc::ex_any* continuation_executor;
  size_t prio;
#ifndef NDEBUG
  size_t pending_or_ready_slots;
#endif
  // the atomic synchronization variable that coordinates between this and
  // awaitable_customizer (task's final_suspend)
  std::atomic<size_t> sync_flags;

  template <typename T>
  using ResultStorage = tmc::detail::result_storage_t<tmc::detail::void_to_monostate<
    typename tmc::detail::get_awaitable_traits<T>::result_type>>;
  using ResultTuple = std::tuple<ResultStorage<Awaitable>...>;
  ResultTuple result;

  using AwaitableTuple = std::tuple<Awaitable...>;

  // coroutines are prepared and stored in an array, then submitted in bulk
  template <typename T>
  TMC_FORCE_INLINE inline void prepare_task(
    T&& Task, ResultStorage<T>* TaskResult, size_t Idx, size_t ContinuationPrio,
    work_item& Task_out
  ) {
    tmc::detail::get_awaitable_traits<T>::set_continuation(Task, &continuation);
    tmc::detail::get_awaitable_traits<T>::set_continuation_executor(
      Task, &continuation_executor
    );
    tmc::detail::get_awaitable_traits<T>::set_done_count(Task, &sync_flags);
    tmc::detail::get_awaitable_traits<T>::set_flags(
      Task, tmc::detail::task_flags::EACH |
              (Idx << tmc::detail::task_flags::TASKNUM_LOW_OFF) | ContinuationPrio
    );
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
  TMC_FORCE_INLINE inline void prepare_awaitable(
    T&& Task, ResultStorage<T>* TaskResult, size_t Idx, size_t ContinuationPrio
  ) {
    tmc::detail::get_awaitable_traits<T>::set_continuation(Task, &continuation);
    tmc::detail::get_awaitable_traits<T>::set_continuation_executor(
      Task, &continuation_executor
    );
    tmc::detail::get_awaitable_traits<T>::set_done_count(Task, &sync_flags);
    tmc::detail::get_awaitable_traits<T>::set_flags(
      Task, tmc::detail::task_flags::EACH |
              (Idx << tmc::detail::task_flags::TASKNUM_LOW_OFF) | ContinuationPrio
    );
    if constexpr (!std::is_void_v<
                    typename tmc::detail::get_awaitable_traits<T>::result_type>) {
      tmc::detail::get_awaitable_traits<T>::set_result_ptr(Task, TaskResult);
    }
  }

  void set_done_count(size_t NumTasks) {
    remaining_count = static_cast<ptrdiff_t>(NumTasks);
#ifndef NDEBUG
    pending_or_ready_slots = (TMC_ONE_BIT << NumTasks) - 1;
#endif
    sync_flags.store(tmc::detail::task_flags::EACH, std::memory_order_release);
  }

public:
  /// Eagerly initiates all of the provided awaitables, like
  /// `tmc::spawn_tuple(Awaitables...)`, but makes each result available as it
  /// becomes ready. The template arguments are deduced from the awaitable
  /// arguments.
  mux_tuple(Awaitable&&... Awaitables)
    requires(Count != 0)
      : executor{tmc::detail::this_thread::executor()},
        continuation_executor{tmc::detail::this_thread::executor()},
        prio{tmc::detail::this_thread::this_task().prio} {
    AwaitableTuple Tasks(static_cast<Awaitable&&>(Awaitables)...);

    size_t continuationPriority = tmc::detail::this_thread::this_task().prio;

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
           prepare_awaitable(
             std::get<I>(std::move(Tasks)), &std::get<I>(result), I, continuationPriority
           );
         } else {
           prepare_task(
             tmc::detail::into_known<false>(std::get<I>(std::move(Tasks))),
             &std::get<I>(result), I, continuationPriority, taskArr[taskIdx]
           );
           ++taskIdx;
         }
       }()),
       ...);
    }(std::make_index_sequence<Count>{});

    set_done_count(Count);

    // Bulk submit the coroutines
    if constexpr (WorkItemCount != 0) {
      tmc::detail::post_bulk_checked(executor, taskArr.data(), WorkItemCount, prio);
    }

    // Individually initiate the awaitables
    [&]<std::size_t... I>(std::index_sequence<I...>) {
      (([&]() {
         if constexpr (!tmc::detail::treat_as_coroutine<
                         std::tuple_element_t<I, AwaitableTuple>>::value) {
           // The std::move is a bit misleading here - this actually forwards the
           // awaitable from within the tuple with its original value category.
           tmc::detail::get_awaitable_traits<std::tuple_element_t<I, AwaitableTuple>>::
             async_initiate(std::get<I>(std::move(Tasks)), executor, prio);
         }
       }()),
       ...);
    }(std::make_index_sequence<Count>{});
  }

  /// Creates the result storage but does not initiate any awaitables. The
  /// template arguments must be provided explicitly. Use `restart<I>()` to
  /// initiate work into individual slots.
  mux_tuple()
      : executor{tmc::detail::this_thread::executor()},
        continuation_executor{tmc::detail::this_thread::executor()},
        prio{tmc::detail::this_thread::this_task().prio} {
    set_done_count(0);
  }

  /// Always suspends.
  inline bool await_ready() const noexcept {
    return tmc::detail::result_each_await_ready();
  }

  /// Suspends the outer coroutine until a result becomes ready (or resumes
  /// immediately if one is already ready).
  inline bool await_suspend(std::coroutine_handle<> Outer) noexcept {
    return tmc::detail::result_each_await_suspend(
      remaining_count, Outer, continuation, continuation_executor, sync_flags
    );
  }

  /// Returns the index of a single ready slot. Results may become ready in any
  /// order, but each consumed (or restarted) slot index will be returned
  /// exactly once per submission. When no submitted results remain, the index
  /// returned will be equal to the value of `end()`.
  TMC_AWAIT_RESUME inline size_t await_resume() noexcept {
    auto slot = tmc::detail::result_each_await_resume(remaining_count, sync_flags);
#ifndef NDEBUG
    if (slot != end()) {
      pending_or_ready_slots &= ~(TMC_ONE_BIT << slot);
    }
#endif
    return slot;
  }

  /// This type must be awaited as an lvalue (it is awaited repeatedly and is not
  /// movable). The awaiter is the group itself.
  mux_tuple& operator co_await() & noexcept { return *this; }

  /// Provides a sentinel value that can be compared against the value returned
  /// from co_await.
  inline size_t end() noexcept { return 64; }

  // Gets the ready result at the given index.
  template <size_t I> inline std::tuple_element_t<I, ResultTuple>& get() noexcept {
    return std::get<I>(result);
  }

  /// Starts a new awaitable in the given slot and initiates it immediately. The
  /// slot must be empty - either it was never started (when constructed with the
  /// empty constructor), or its previous result has already been consumed by
  /// `co_await`. Read the current result before calling this, since the next
  /// completion will overwrite the slot. The replacement awaitable must produce
  /// the same result slot type as the slot's declared awaitable.
  template <size_t I, typename T> inline void restart(T&& Task) {
#ifndef NDEBUG
    constexpr size_t slotBit = TMC_ONE_BIT << I;
    assert(
      0 == (pending_or_ready_slots & slotBit) &&
      "You may only restart a slot after its previous result has been "
      "awaited."
    );
#endif

    decltype(auto) known = tmc::detail::into_known<false>(static_cast<T&&>(Task));
    using KnownAwaitable = std::remove_reference_t<decltype(known)>;
    using SlotResult = std::tuple_element_t<I, ResultTuple>;
    static_assert(
      std::is_same_v<ResultStorage<KnownAwaitable>, SlotResult>,
      "Replacement awaitable must have the same result type as the original "
      "slot."
    );

    auto continuationPriority = tmc::detail::this_thread::this_task().prio;
#ifndef NDEBUG
    pending_or_ready_slots |= slotBit;
#endif
    ++remaining_count;

    constexpr auto mode = tmc::detail::get_awaitable_traits<KnownAwaitable>::mode;
    if constexpr (mode == tmc::detail::ASYNC_INITIATE) {
      prepare_awaitable(
        static_cast<decltype(known)&&>(known), &std::get<I>(result), I,
        continuationPriority
      );
      // Forward known with its original value category.
      tmc::detail::get_awaitable_traits<KnownAwaitable>::async_initiate(
        static_cast<decltype(known)&&>(known), executor, prio
      );
    } else {
      work_item item;
      prepare_task(
        static_cast<decltype(known)&&>(known), &std::get<I>(result), I,
        continuationPriority, item
      );
      tmc::detail::post_checked(executor, std::move(item), prio);
    }
  }

  // This must be awaited and all child tasks completed before destruction.
#ifndef NDEBUG
  ~mux_tuple() noexcept {
    assert(remaining_count == 0 && "You must submit or co_await this.");
  }
#endif

  // Not movable or copyable due to awaitables being initiated with pointers to
  // this.
  mux_tuple& operator=(const mux_tuple& other) = delete;
  mux_tuple(const mux_tuple& other) = delete;
  mux_tuple& operator=(mux_tuple&& other) = delete;
  mux_tuple(mux_tuple&& other) = delete;
};

// Deduces the awaitable template parameters from the constructor arguments,
// mirroring tmc::spawn_tuple(): lvalues are stored by reference; movable
// rvalues are stored by value; non-movable rvalues are stored by rvalue
// reference.
//
// The `requires` clause is load-bearing. The eager constructor
// `mux_tuple(Awaitable&&...) requires(Count != 0)` generates its own implicit
// deduction guide whose result type is `mux_tuple<Awaitable...>` - without the
// forward_awaitable transformation - which would store a non-movable rvalue
// by value (and fail to compile). Because that constructor is constrained and
// this guide would otherwise be unconstrained, overload resolution's
// "more constrained" tiebreaker would pick the constructor's guide *before*
// reaching the "prefer the written deduction guide" tiebreaker. Constraining
// this guide identically makes those two tie on constraints, so the written
// guide wins and forward_awaitable is applied.
template <typename... Awaitable>
  requires(sizeof...(Awaitable) != 0)
mux_tuple(Awaitable&&...) -> mux_tuple<tmc::detail::forward_awaitable<Awaitable>...>;

} // namespace tmc
