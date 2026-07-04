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
#include "tmc/detail/tsan.hpp"
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
#include <utility>

namespace tmc {
/// A reusable result-multiplexer over a fixed set of awaitable slots.
/// - Rather than returning all results at once, each result is made available as it
///   becomes ready: `co_await` on the mux returns the index of a single ready slot.
/// - Templated on result types, not awaitable types. `void` becomes `std::monostate`.
/// - Passing the awaitables up front is optional. You can construct this empty and
///   `fork()` awaitables into it afterward. It is valid to await this while only a
///   partial set of the slots are active.
/// - Each `fork()`ed awaitable can be dispatched to a separate executor/priority.
/// - You must ensure that all awaitables are completed (co_await mux == mux.end())
///   before destroying this.
/// - Limited to 63 (or 31, on 32-bit) slots.
///
/// After a slot's result has been consumed by `co_await`, you may call `fork<I>()` to
/// launch a fresh awaitable into that slot. The replacement awaitable must produce the
/// same result type as the slot. This allows you to maintain a fixed level of concurrency
/// (by always replacing a completed slot) or partial / conditional concurrency (see the
/// batch_processor.cpp example).
///
/// There are two constructors: one taking all the awaitables, and deducing the result
/// types, and the other taking no awaitables, which requires you to specify the result
/// type for each slot:
/// ```
/// // Result types deduced to <int, data>. Both awaitables are forked eagerly.
/// mux_tuple mux0(int_awaitable, data_task);
///
/// // Result types explicitly specified. Individual awaitables can be forked afterward.
/// mux_tuple<int, data> mux1;
/// ```
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
  std::atomic<size_t> sync_flags = tmc::detail::task_flags::EACH;

  // Non-default-constructible results are stored in a std::optional. This is not
  // visible to the user; when get() is called, a reference to the constructed object is
  // returned.
  template <typename R>
  using ResultStorage = tmc::detail::result_storage_t<tmc::detail::void_to_monostate<R>>;
  using ResultTuple = std::tuple<ResultStorage<Result>...>;
  // The underlying value type of each slot (before optional-wrapping), with void
  // slots represented as std::monostate. get<I>() returns a reference to this
  // type, unwrapping the optional storage for non-default-constructible values.
  using ValueTuple = std::tuple<tmc::detail::void_to_monostate<Result>...>;
  ResultTuple result;

  // Prepares the work item but does not initiate it.
  template <typename T, typename R>
  TMC_FORCE_INLINE inline void
  prepare_work(T& Task, R* TaskResult, size_t Idx, size_t ContinuationPrio) {
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

  // Sets the owner-only (non-atomic) tracking variables.
  void init_active_counts(size_t NumTasks) {
    remaining_count = static_cast<ptrdiff_t>(NumTasks);
    active_slots = (TMC_ONE_BIT << NumTasks) - 1;
  }

public:
  /// Creates the result storage but does not initiate any awaitables. The
  /// Result template types must be provided explicitly. Use `fork<I>()` to
  /// initiate work into individual slots.
  mux_tuple() { init_active_counts(0); }

  /// Eagerly initiates all of the provided awaitables. The Result template types
  /// are deduced from the awaitable arguments.
  template <typename... Awaitable>
    requires(sizeof...(Awaitable) != 0)
  mux_tuple(Awaitable&&... Awaitables) {
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

    init_active_counts(Count);
    tmc::ex_any* executor = tmc::detail::this_thread::executor();
    size_t prio = tmc::detail::this_thread::this_task().prio;

    // Prepare each task as if I loops from [0..Count),
    // but using compile-time indexes and types.
    size_t taskIdx = 0;
    [&]<std::size_t... I>(std::index_sequence<I...>) {
      (([&]() {
         using Elem = std::tuple_element_t<I, AwaitableTuple>;
         constexpr auto mode = tmc::detail::get_awaitable_traits<Elem>::mode;
         static_assert(
           mode != tmc::detail::UNKNOWN, "This doesn't appear to be an awaitable."
         );
         // The std::move is a bit misleading here - std::get actually forwards
         // the awaitable from within the tuple with its original value category.
         if constexpr (mode == tmc::detail::ASYNC_INITIATE) {
           // Individually initiate the awaitables.
           decltype(auto) t = std::get<I>(std::move(Tasks));
           prepare_work(t, &std::get<I>(result), I, prio);
           tmc::detail::get_awaitable_traits<Elem>::async_initiate(
             static_cast<decltype(t)&&>(t), executor, prio
           );
         } else {
           // Batch the coroutines into an array so they can be submitted in bulk.
           decltype(auto) known =
             tmc::detail::into_known<false>(std::get<I>(std::move(Tasks)));
           prepare_work(known, &std::get<I>(result), I, prio);
           taskArr[taskIdx] =
             tmc::detail::into_initiate(static_cast<decltype(known)&&>(known));
           ++taskIdx;
         }
       }()),
       ...);
    }(std::make_index_sequence<Count>{});

    // Bulk submit the coroutines
    if constexpr (WorkItemCount != 0) {
      tmc::detail::post_bulk_checked(executor, taskArr.data(), WorkItemCount, prio);
    }
  }

  /// Always suspends.
  inline bool await_ready() const noexcept { return false; }

  /// Suspends the outer coroutine until a result becomes ready (or resumes
  /// immediately if one is already ready).
  inline bool await_suspend(std::coroutine_handle<> Outer) noexcept {
    // mux_tuple can be awaited in a different context than where it was
    // created. Therefore, capture the continuation_executor at the await point.
    continuation_executor = tmc::detail::this_thread::executor();
    return tmc::detail::result_each_await_suspend(
      remaining_count, Outer, continuation, sync_flags
    );
  }

  /// Returns the index of a single ready slot. Results may become ready in any
  /// order, but each consumed (or forked) slot index will be returned
  /// exactly once per submission. When no submitted results remain, the index
  /// returned will be equal to the value of `end()`.
  [[nodiscard]] inline size_t await_resume() noexcept {
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

  /// Gets the ready result at the given index.
  template <size_t I>
  TMC_TSAN_NO_SPECULATE inline std::tuple_element_t<I, ValueTuple>& get() noexcept {
    assert(
      !is_active<I>() && "You may only call get() on a slot after its result "
                         "has been returned from co_await."
    );
    using Value = std::tuple_element_t<I, ValueTuple>;
    if constexpr (std::is_default_constructible_v<Value>) {
      return std::get<I>(result);
    } else {
      return *std::get<I>(result);
    }
  }

  /// Returns true if slot `I` is currently active: it has been forked but its
  /// result has not yet been returned from `co_await`. An active slot may not be
  /// re-forked.
  template <size_t I> inline bool is_active() const noexcept {
    static_assert(I < Count, "is_active() index out of range.");
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
  /// This method is not thread-safe to call concurrently with itself or `co_await`.
  template <size_t I, typename Aw, typename Exec = tmc::ex_any*>
  inline void fork(
    Aw&& Awaitable, Exec&& Executor = tmc::current_executor(),
    size_t Priority = tmc::current_priority()
  ) {
    decltype(auto) known = tmc::detail::into_known<false>(static_cast<Aw&&>(Awaitable));
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

    prepare_work(known, &std::get<I>(result), I, continuationPriority);

    constexpr auto mode = tmc::detail::get_awaitable_traits<KnownAwaitable>::mode;
    if constexpr (mode == tmc::detail::ASYNC_INITIATE) {
      // Forward known with its original value category.
      tmc::detail::get_awaitable_traits<KnownAwaitable>::async_initiate(
        static_cast<decltype(known)&&>(known), exec, Priority
      );
    } else {
      auto item = tmc::detail::into_initiate(static_cast<decltype(known)&&>(known));
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

  /// Similar to `fork<I>()` but allows the forked awaitable's allocation to be elided
  /// by combining it into the parent's allocation (HALO). This works by using
  /// specific attributes that are only available on Clang 20+. You can safely
  /// call this function on other compilers, but no HALO-specific optimizations
  /// will be applied.
  ///
  /// This method is not thread-safe to call concurrently with itself or `co_await`.
  ///
  /// WARNING: You may safely call this in a loop *only if* you use a switch statement so
  /// that each index has a unique call site. This ensures Clang will create independent
  /// awaitable storage for each slot (keyed to the call site).
  ///
  /// IMPORTANT: This returns a dummy awaitable. For HALO to work, you should
  /// not store the dummy awaitable. Instead, `co_await` this expression
  /// immediately.
  ///
  /// Proper usage, taking both of the above into account:
  /// ```
  /// tmc::mux_tuple mux(task(0), task(1));
  /// for (size_t i = co_await mux; i != mux.end(); i = co_await mux) {
  ///   switch (i) {
  ///   case 0:
  ///     process(mux.get<0>());
  ///     co_await mux.fork_clang<0>(task(0));
  ///     break;
  ///   case 1:
  ///     process(mux.get<1>());
  ///     co_await mux.fork_clang<1>(task(1));
  ///     break;
  ///   default:
  ///     std::unreachable();
  ///   }
  /// }
  /// ```
  template <size_t I, typename Aw, typename Exec = tmc::ex_any*>
  [[nodiscard("You must co_await fork_clang() immediately for HALO to be possible.")]]
  mux_tuple_fork_clang fork_clang(
    TMC_CORO_AWAIT_ELIDABLE_ARGUMENT Aw&& Awaitable,
    Exec&& Executor = tmc::current_executor(), size_t Priority = tmc::current_priority()
  ) {
    fork<I>(static_cast<Aw&&>(Awaitable), static_cast<Exec&&>(Executor), Priority);
    return mux_tuple_fork_clang{};
  }

// This must be awaited and all child awaitables completed before destruction.
#ifndef NDEBUG
  ~mux_tuple() noexcept {
    assert(
      remaining_count == 0 &&
      "You must wait for all mux_tuple awaitables to complete before destroying it."
    );
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
