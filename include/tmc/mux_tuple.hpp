// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/current.hpp"
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

/// A reusable result-multiplexer over a fixed set of awaitable slots.
/// - Rather than returning all results at once, each result is made available as it
///   becomes ready: `co_await`ing the mux returns the index of a single ready slot.
/// - Templated on result types, not awaitable types. `void` becomes `std::monostate`.
/// - Passing the awaitables up front is optional. You can construct this empty and
///   `fork()` awaitables into it afterward.
/// - After a result is consumed, its slot can be reused to `fork()` a new awaitable.
/// - Each `fork()`ed awaitable can be dispatched to a separate executor/priority.
/// - You must ensure that all awaitables are completed (co_await mux == mux.end())
///   before destroying this.
/// - Limited to 63 (or 31, on 32-bit) slots.
///
/// There are two ways to construct a `mux_tuple`:
///
/// 1. With awaitables (template arguments are deduced). This forks all of the provided
/// awaitables immediately.
/// ```
/// tmc::mux_tuple mux(task0(), task1());
/// ```
///
/// 2. Empty (result template arguments must be provided explicitly). This only
/// allocates the result storage; no awaitables are initiated. You start individual slots
/// with `fork<I>()` whenever you like, optionally choosing a specific executor and
/// priority for each one:
/// ```
/// tmc::mux_tuple<int, std::string> mux;
/// mux.fork<0>(make_int_task());
/// mux.fork<1>(make_string_task());
/// ```
///
/// After a slot's result has been consumed by `co_await`, you may
/// call `fork<I>()` to launch a fresh awaitable into that slot. The
/// replacement awaitable must produce the same result slot type as the slot's
/// declared awaitable. This allows you to maintain a fixed level of concurrency (by
/// always replacing a completed slot) or partial / conditional concurrency (see the
/// batch_processor.cpp example).
///
/// tmc::mux_tuple<int, std::string> mux;
/// bool
/// mux.fork<0>(make_int_task());
/// for (size_t i = co_await mux; i != mux.end(); i = co_await mux) {
/// switch (i) {
///   case 0:
///     process(mux.get<0>());
///     mux.fork<1>(make_string_task());
///     break;
///   case 1:
///     process(mux.get<1>());
///     break;
///   default:
///     std::unreachable();
///   }
/// }
template <typename... Result>
class mux_tuple : private tmc::detail::AwaitTagNoGroupCoAwaitLvalue {
  static constexpr auto Count = sizeof...(Result);

  // Tasks are synchronized via an atomic bitmask with only 63 (or 31, on 32-bit)
  // slots for tasks.
  static_assert(
    Count < TMC_PLATFORM_BITS,
    "mux_tuple supports at most 63 awaitables (31 on 32-bit platforms)."
  );

  std::coroutine_handle<> continuation;
  tmc::ex_any* continuation_executor;

  // Bitmap of slots that have been forked but not yet returned from co_await.
  // A set bit means the slot is active: its result is pending or ready but has
  // not been consumed, so the slot may not be re-forked. Non-atomic; only
  // mutated by the (single-threaded) owner.
  size_t active_slots;
  // equal to popcount(active_slots)
  ptrdiff_t remaining_count;

  // the atomic synchronization variable that coordinates between this and
  // awaitable_customizer (task's final_suspend)
  std::atomic<size_t> sync_flags;

  // Non-default-constructible results are stored in a std::optional. This is not visible
  // to the user; when get() is called, a reference to the constructed object is returned.
  template <typename R>
  using ResultStorage = tmc::detail::result_storage_t<tmc::detail::void_to_monostate<R>>;
  using ResultTuple = std::tuple<ResultStorage<Result>...>;
  // The underlying value type of each slot (before optional-wrapping), with void
  // slots represented as std::monostate. get<I>() returns a reference to this
  // type, unwrapping the optional storage for non-default-constructible values.
  using ValueTuple = std::tuple<tmc::detail::void_to_monostate<Result>...>;
  ResultTuple result;

  // coroutines are prepared and stored in an array, then submitted in bulk
  template <typename T, typename R>
  TMC_FORCE_INLINE inline void prepare_task(
    T&& Task, R* TaskResult, size_t Idx, size_t ContinuationPrio, work_item& Task_out
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
  template <typename T, typename R>
  TMC_FORCE_INLINE inline void
  prepare_awaitable(T&& Task, R* TaskResult, size_t Idx, size_t ContinuationPrio) {
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
    active_slots = (TMC_ONE_BIT << NumTasks) - 1;
    sync_flags.store(tmc::detail::task_flags::EACH, std::memory_order_release);
  }

public:
  /// Eagerly initiates all of the provided awaitables, like
  /// `tmc::spawn_tuple(Awaitables...)`, but makes each result available as it
  /// becomes ready. The result-type template arguments are deduced from the
  /// awaitable arguments.
  template <typename... Awaitable>
    requires(sizeof...(Awaitable) != 0)
  mux_tuple(Awaitable&&... Awaitables)
      : continuation_executor{tmc::detail::this_thread::executor()} {
    static_assert(
      std::is_same_v<
        ResultTuple, std::tuple<ResultStorage<typename tmc::detail::get_awaitable_traits<
                       Awaitable>::result_type>...>>,
      "The provided awaitables' result types (and their count) must match the "
      "mux_tuple's result-type template arguments."
    );

    // Hold awaitables by reference, as if by std::forward_as_tuple.
    // This is safe because the awaitables are initiated within this constructor,
    // not permanently stored.
    using AwaitableTuple = std::tuple<Awaitable&&...>;
    AwaitableTuple Tasks(static_cast<Awaitable&&>(Awaitables)...);

    // Compile-time count of the number of non-ASYNC_INITIATE awaitables so they can be
    // batched and submitted with a single post_bulk.
    constexpr size_t WorkItemCount =
      std::tuple_size_v<typename tmc::detail::predicate_partition<
        tmc::detail::treat_as_coroutine, std::tuple, Awaitable&&...>::true_types>;
    std::array<work_item, WorkItemCount> taskArr;

    tmc::ex_any* executor = tmc::detail::this_thread::executor();
    size_t prio = tmc::detail::this_thread::this_task().prio;
    size_t continuationPriority = tmc::detail::this_thread::this_task().prio;
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
  /// template arguments must be provided explicitly. Use `fork<I>()` to
  /// initiate work into individual slots.
  mux_tuple() : continuation_executor{tmc::detail::this_thread::executor()} {
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
  /// order, but each consumed (or forked) slot index will be returned
  /// exactly once per submission. When no submitted results remain, the index
  /// returned will be equal to the value of `end()`.
  TMC_AWAIT_RESUME inline size_t await_resume() noexcept {
    auto slot = tmc::detail::result_each_await_resume(remaining_count, sync_flags);
    if (slot != end()) {
      active_slots &= ~(TMC_ONE_BIT << slot);
    }
    return slot;
  }

  /// This type must be awaited as an lvalue (it is awaited repeatedly and is not
  /// movable). The awaiter is the group itself.
  mux_tuple& operator co_await() & noexcept { return *this; }

  /// Provides a sentinel value that can be compared against the value returned
  /// from co_await.
  inline size_t end() noexcept { return 64; }

  // Gets the ready result at the given index.
  template <size_t I> inline std::tuple_element_t<I, ValueTuple>& get() noexcept {
    using Value = std::tuple_element_t<I, ValueTuple>;
    if constexpr (std::is_default_constructible_v<Value>) {
      return std::get<I>(result);
    } else {
      return *std::get<I>(result);
    }
  }

  /// Returns true if slot `I` is currently active: it has been forked but its
  /// result has not yet been returned from `co_await`. An active slot may not be
  /// re-forked. This is the negation of the precondition checked by `fork<I>()`.
  template <size_t I> inline bool is_active() const noexcept {
    return 0 != (active_slots & (TMC_ONE_BIT << I));
  }

  /// Returns the raw bitmap of active slots: bit `I` is set if slot `I` has been
  /// forked but its result has not yet been returned from `co_await`.
  inline size_t active_bitset() const noexcept { return active_slots; }

  /// Starts a new awaitable in the given slot and initiates it immediately on the
  /// specified executor and priority. The slot must be empty - either it was
  /// never started (when constructed with the empty constructor), or its previous
  /// result has already been consumed by `co_await`. The replacement awaitable must
  /// produce the same result type as the slot's declared awaitable.
  ///
  /// `fork()` destroys the previous result in the given slot before initiating the
  /// replacement. If you need to use the result after this, you should move it out before
  /// calling `fork()`.
  ///
  /// `Executor` defaults to the current executor.
  /// `Priority` defaults to the current priority.
  ///
  /// Only the awaitable's dispatch is customized; regardless of where each slot
  /// runs, the awaiting coroutine always resumes on the executor that was current
  /// when this `mux_tuple` was constructed. The eager constructor does not offer
  /// this per-awaitable customization - it initiates every awaitable on the
  /// executor and priority current at construction. To customize each awaitable,
  /// use the empty constructor and `fork<I>()` each slot.
  ///
  /// This method is not thread-safe.
  template <size_t I, typename T, typename Exec = tmc::ex_any*>
  inline void fork(
    T&& Task, Exec&& Executor = tmc::current_executor(),
    size_t Priority = tmc::current_priority()
  ) {
    decltype(auto) known = tmc::detail::into_known<false>(static_cast<T&&>(Task));
    using KnownAwaitable = std::remove_reference_t<decltype(known)>;
    using KnownResult =
      typename tmc::detail::get_awaitable_traits<KnownAwaitable>::result_type;
    using SlotResult = std::tuple_element_t<I, ResultTuple>;
    static_assert(
      std::is_same_v<ResultStorage<KnownResult>, SlotResult>,
      "Replacement awaitable must have the same result type as the original "
      "slot."
    );

    constexpr size_t slotBit = TMC_ONE_BIT << I;
    assert(
      0 == (active_slots & slotBit) &&
      "You may only fork a slot after its previous result has been "
      "awaited."
    );
    active_slots |= slotBit;
    ++remaining_count;

    // Destroy the previously-consumed result before initiating the replacement. This
    // prevents issues with results that own a resource which the replacement awaitable
    // needs to re-acquire, such as zero-copy queue scopes.
    std::get<I>(result) = SlotResult{};

    tmc::ex_any* exec = tmc::detail::get_executor_traits<Exec>::type_erased(Executor);
    auto continuationPriority = tmc::detail::this_thread::this_task().prio;
    constexpr auto mode = tmc::detail::get_awaitable_traits<KnownAwaitable>::mode;
    if constexpr (mode == tmc::detail::ASYNC_INITIATE) {
      prepare_awaitable(
        static_cast<decltype(known)&&>(known), &std::get<I>(result), I,
        continuationPriority
      );
      // Forward known with its original value category.
      tmc::detail::get_awaitable_traits<KnownAwaitable>::async_initiate(
        static_cast<decltype(known)&&>(known), exec, Priority
      );
    } else {
      work_item item;
      prepare_task(
        static_cast<decltype(known)&&>(known), &std::get<I>(result), I,
        continuationPriority, item
      );
      tmc::detail::post_checked(exec, std::move(item), Priority);
    }
  }

  /// This is a dummy awaitable. Don't store this in a variable.
  /// For HALO to work, you must `co_await mux.fork_clang<I>()` immediately.
  class TMC_CORO_AWAIT_ELIDABLE mux_tuple_fork_clang : tmc::detail::AwaitTagNoGroupAsIs {
  public:
    mux_tuple_fork_clang() {}

    /// Never suspends.
    bool await_ready() const noexcept { return true; }

    /// Does nothing.
    void await_suspend(std::coroutine_handle<>) noexcept {}

    /// Does nothing.
    void await_resume() noexcept {}
  };

  /// Similar to `fork<I>()` but allows the forked task's allocation to be elided
  /// by combining it into the parent's allocation (HALO). This works by using
  /// specific attributes that are only available on Clang 20+. You can safely
  /// call this function on other compilers, but no HALO-specific optimizations
  /// will be applied.
  ///
  /// This method is not thread-safe.
  ///
  /// WARNING: Don't allow coroutines passed into this to cross a loop boundary,
  /// or Clang will try to reuse the same allocation for multiple active
  /// coroutines.
  ///
  /// IMPORTANT: This returns a dummy awaitable. For HALO to work, you should
  /// not store the dummy awaitable. Instead, `co_await` this expression
  /// immediately. Proper usage:
  /// ```
  /// tmc::mux_tuple<int, int> mux;
  /// co_await mux.fork_clang<0>(task(0));
  /// co_await mux.fork_clang<1>(task(1));
  /// for (size_t i = co_await mux; i != mux.end(); i = co_await mux) { ... }
  /// ```
  template <size_t I, typename T, typename Exec = tmc::ex_any*>
  [[nodiscard("You must co_await fork_clang() immediately for HALO to be possible.")]]
  mux_tuple_fork_clang fork_clang(
    TMC_CORO_AWAIT_ELIDABLE_ARGUMENT T&& Task, Exec&& Executor = tmc::current_executor(),
    size_t Priority = tmc::current_priority()
  ) {
    fork<I>(static_cast<T&&>(Task), static_cast<Exec&&>(Executor), Priority);
    return mux_tuple_fork_clang{};
  }

  // This must be awaited and all child tasks completed before destruction.
#ifndef NDEBUG
  ~mux_tuple() noexcept {
    assert(remaining_count == 0 && "You must submit or co_await this.");
  }
#endif

  // Not movable or copyable due to awaitables being initiated with pointers to this.
  mux_tuple& operator=(const mux_tuple& other) = delete;
  mux_tuple(const mux_tuple& other) = delete;
  mux_tuple& operator=(mux_tuple&& other) = delete;
  mux_tuple(mux_tuple&& other) = delete;
};

// Deduces the slots' result-type template parameters from the constructor
// arguments. Each slot's type is the result the corresponding awaitable produces when
// awaited.
template <typename... Awaitable>
  requires(sizeof...(Awaitable) != 0)
mux_tuple(Awaitable&&...)
  -> mux_tuple<typename tmc::detail::get_awaitable_traits<Awaitable>::result_type...>;

} // namespace tmc
