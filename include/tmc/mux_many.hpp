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
#include "tmc/detail/mux_shared.hpp"
#include "tmc/detail/thread_locals.hpp"
#include "tmc/detail/tsan.hpp"
#include "tmc/ex_any.hpp"
#include "tmc/work_item.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <coroutine>
#include <cstddef>
#include <iterator>
#include <type_traits>
#include <utility>

namespace tmc {
/// A reusable result-multiplexer over a fixed set of awaitable slots.
/// - Rather than returning all results at once, each result is made available as it
///   becomes ready: `co_await` on the mux returns the index of a single ready slot.
/// - All results must be of the same type.
/// - Passing the awaitables up front is optional. You can construct this empty and
///   `fork()` awaitables into it afterward. It is valid to await this while only a
///   partial set of the slots are active.
/// - Each `fork()`ed awaitable can be dispatched to a separate executor/priority.
/// - You must ensure that all awaitables are completed (co_await mux == mux.end())
///   before destroying this.
/// - Results are stored internally in a fixed-size `std::array`, limited to 63 (or 31, on
///   32-bit) slots. You can dispatch fewer awaitables than this (not every slot must be
///   filled). If no size is specified, the default maximum of 63 or 31 will be used.
///
/// After a slot's result has been consumed by `co_await`, you may call `fork(i)` to
/// launch a fresh awaitable into that slot. This allows you to maintain a fixed level of
/// concurrency (by always replacing a completed slot) or partial / conditional
/// concurrency (see the batch_processor.cpp example).
///
/// There are two families of construction:
///
/// 1. With awaitables (an iterator + count, a begin/end range, a begin/end range
/// + max count, or a range object). The `Result` type is deduced from the
/// awaitables via CTAD, and `Count` is left at its default. The awaitables are forked
/// eagerly.
/// ```
/// auto mux = tmc::mux_many(awaitables.begin(), awaitables.end());
/// for (size_t i = co_await mux; i != mux.end(); i = co_await mux) {
///   use(mux[i]);
/// }
/// ```
///
/// To right-size the `std::array` storage, name both template arguments explicitly:
/// ```
/// auto mux = tmc::mux_many<int, N>(awaitables.begin());                   // exactly N
/// auto mux = tmc::mux_many<int, N>(awaitables.begin(), awaitables.end()); // up to N
///
/// // Allocate N storage slots. Eagerly dispatch awaitables equal to the smallest of:
/// // - awaitables.end() - awaitables.begin() (total awaitable count)
/// // - maxCount
/// // - N
/// auto mux = tmc::mux_many<int, N>(awaitables.begin(), awaitables.end(), maxCount);
/// ```
///
/// If constructed with a partial workload (`End - Begin < Count` or `MaxCount < Count`)
/// the initiated awaitables will be at the low indexes, and higher indexes will be
/// empty and ready to `fork()` into immediately.
///
/// 2. Empty (the `Result` type must be provided explicitly; `Count` defaults if not
/// provided). This only allocates the result storage; no awaitables are initiated. You
/// start individual slots with `fork()` whenever you like, optionally choosing a specific
/// executor and priority for each one:
/// ```
/// auto mux = tmc::mux_many<int, 2>(); // storage for 2 slots
/// auto mux = tmc::mux_many<int>();    // storage for TMC_PLATFORM_BITS - 1 slots
/// mux.fork(0, make_int_task());
/// mux.fork(1, make_int_task());
/// for (size_t i = co_await mux; i != mux.end(); i = co_await mux) { ... }
/// ```
template <typename Result, size_t Count = TMC_PLATFORM_BITS - 1>
class mux_many : private tmc::detail::AwaitTagNoGroupCoAwaitLvalue {
  // Tasks are synchronized via an atomic bitmask with only 63 (or 31, on 32-bit)
  // slots for tasks.
  static_assert(
    Count < TMC_PLATFORM_BITS,
    "mux_many supports up to 63 awaitables (31 on 32-bit platforms)."
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

  // Non-default-constructible results are stored in a std::optional. This is not visible
  // to the user; when operator[] is called, a reference to the constructed object is
  // returned.
  using ResultStorage = tmc::detail::result_storage_t<Result>;
  struct empty {};
  using ResultArray =
    std::conditional_t<std::is_void_v<Result>, empty, std::array<ResultStorage, Count>>;
  TMC_NO_UNIQUE_ADDRESS ResultArray result_arr;

  // Prepares the work item but does not initiate it.
  template <typename T>
  TMC_FORCE_INLINE inline void
  prepare_work(T& Task, size_t Idx, size_t ContinuationPrio) {
    tmc::detail::get_awaitable_traits<T>::set_continuation(Task, &continuation);
    tmc::detail::get_awaitable_traits<T>::set_continuation_executor(
      Task, &continuation_executor
    );
    tmc::detail::get_awaitable_traits<T>::set_done_count(Task, &sync_flags);
    tmc::detail::get_awaitable_traits<T>::set_flags(
      Task, tmc::detail::task_flags::EACH |
              (Idx << tmc::detail::task_flags::TASKNUM_LOW_OFF) | ContinuationPrio
    );
    if constexpr (!std::is_void_v<Result>) {
      tmc::detail::get_awaitable_traits<T>::set_result_ptr(Task, &result_arr[Idx]);
    }
  }

  // Sets the owner-only (non-atomic) tracking variables.
  void init_active_counts(size_t NumTasks) {
    remaining_count = static_cast<ptrdiff_t>(NumTasks);
    active_slots = (TMC_ONE_BIT << NumTasks) - 1;
  }

  // A sentinel for initiate_eager() iterations that are bounded only by the
  // count; it never compares equal to the iterator.
  struct unbounded {
    template <typename TaskIter>
    friend bool operator!=(const TaskIter&, unbounded) noexcept {
      return true;
    }
  };

  // Eagerly initiates awaitables from `Iter` until `Size` have been initiated
  // or `Iter == End` (pass `unbounded{}` when there is no end iterator).
  // Returns the number initiated. Initiated awaitables interact only with
  // sync_flags as they complete, so the caller may set the tracking counts
  // from the returned value afterward.
  template <typename TaskIter, typename EndIter>
  size_t initiate_eager(TaskIter Iter, EndIter End, size_t Size) {
    using Awaitable = std::remove_cvref_t<std::iter_value_t<TaskIter>>;
    constexpr auto mode = tmc::detail::get_awaitable_traits<Awaitable>::mode;
    static_assert(
      mode != tmc::detail::UNKNOWN, "This doesn't appear to be an awaitable."
    );

    tmc::ex_any* executor = tmc::detail::this_thread::executor();
    size_t prio = tmc::detail::this_thread::this_task().prio;

    size_t taskCount = 0;
    if constexpr (mode == tmc::detail::ASYNC_INITIATE) {
      // ASYNC_INITIATE types may possibly not be stored in an array
      // (no default/copy constructor), so initiate them individually.
      while (Iter != End && taskCount < Size) {
        TMC_DISABLE_WARNING_PESSIMIZING_MOVE_BEGIN
        auto t = std::move(*Iter);
        TMC_DISABLE_WARNING_PESSIMIZING_MOVE_END
        prepare_work(t, taskCount, prio);
        tmc::detail::get_awaitable_traits<Awaitable>::async_initiate(
          std::move(t), executor, prio
        );
        ++Iter;
        ++taskCount;
      }
    } else {
      // Batch other types of awaitables into a work_item array and submit them
      // in bulk. The iterator could produce fewer than `Size` awaitables, so
      // count them; it could also produce more, so stop after taking `Size`.
      // The result storage is a fixed-size array, so result pointers are stable
      // and can be bound as each awaitable is prepared - no second pass or
      // separate type-preserving buffer (as an unsized std::vector would need)
      // is required, even for COROUTINE-mode awaitables.
      std::array<work_item, Count> taskArr;
      while (Iter != End && taskCount < Size) {
        TMC_DISABLE_WARNING_PESSIMIZING_MOVE_BEGIN
        auto t = tmc::detail::into_known<false>(std::move(*Iter));
        TMC_DISABLE_WARNING_PESSIMIZING_MOVE_END
        prepare_work(t, taskCount, prio);
        taskArr[taskCount] = tmc::detail::into_initiate(std::move(t));
        ++Iter;
        ++taskCount;
      }

      if (taskCount != 0) {
        tmc::detail::post_bulk_checked(executor, taskArr.data(), taskCount, prio);
      }
    }
    return taskCount;
  }

public:
  /// Creates the result storage but does not initiate any awaitables. The
  /// `Result` type must be provided explicitly; `Count` is optional and defaults
  /// to `TMC_PLATFORM_BITS - 1`, e.g. `tmc::mux_many<Result, Count>()`. Use
  /// `fork()` to initiate work into individual slots after construction.
  mux_many() { init_active_counts(0); }

  /// Eagerly initiates awaitables from `Iter`, stopping at the earlier of:
  /// - initiatedCount == AwaitableCount
  /// - initiatedCount == Count
  ///
  /// The `Result` type is deduced via CTAD; name both template arguments
  /// explicitly to right-size the fixed `std::array` storage.
  template <typename AwaitableIter>
    requires(requires(AwaitableIter a) {
      ++a;
      *a;
    })
  inline mux_many(AwaitableIter&& Iter, size_t AwaitableCount) {
    size_t size = AwaitableCount < Count ? AwaitableCount : Count;
    init_active_counts(
      initiate_eager(std::forward<AwaitableIter>(Iter), unbounded{}, size)
    );
  }

  /// Eagerly initiates awaitables from `Begin`, stopping at the earlier of:
  /// - iter == End
  /// - initiatedCount == MaxCount
  /// - initiatedCount == Count
  ///
  /// The `Result` type is deduced via CTAD; name both template arguments
  /// explicitly to right-size the fixed `std::array` storage.
  template <typename AwaitableIter>
    requires(requires(AwaitableIter a, AwaitableIter b) {
      ++a;
      *a;
      a != b;
    })
  inline mux_many(AwaitableIter&& Begin, AwaitableIter&& End, size_t MaxCount) {
    size_t size = MaxCount < Count ? MaxCount : Count;
    init_active_counts(initiate_eager(
      std::forward<AwaitableIter>(Begin), std::forward<AwaitableIter>(End), size
    ));
  }

  /// Eagerly initiates exactly `Count` awaitables from `[Begin, Begin + Count)`.
  /// Name both template arguments explicitly, e.g. `tmc::mux_many<Result, Count>(Begin)`.
  template <typename AwaitableIter>
    requires(
      !requires(std::remove_reference_t<AwaitableIter>& r) {
        r.begin();
        r.end();
      } &&
      requires(AwaitableIter a) {
        ++a;
        *a;
      }
    )
  inline mux_many(AwaitableIter&& Begin)
      : mux_many(std::forward<AwaitableIter>(Begin), Count) {}

  /// Eagerly initiates awaitables from `Begin`, stopping at the earlier of:
  /// - iter == End
  /// - initiatedCount == Count
  ///
  /// The `Result` type is deduced via CTAD; name both template arguments
  /// explicitly to right-size the fixed `std::array` storage.
  template <typename AwaitableIter>
    requires(requires(AwaitableIter a, AwaitableIter b) {
      ++a;
      *a;
      a != b;
    })
  inline mux_many(AwaitableIter&& Begin, AwaitableIter&& End)
      : mux_many(
          std::forward<AwaitableIter>(Begin), std::forward<AwaitableIter>(End), Count
        ) {}

  /// Eagerly initiates awaitables from `Range.begin()`, stopping at the earlier of:
  /// - iter == Range.end()
  /// - initiatedCount == Count
  ///
  /// The `Result` type is deduced via CTAD; name both template arguments
  /// explicitly to right-size the fixed `std::array` storage.
  template <typename AwaitableRange>
    requires(requires(std::remove_reference_t<AwaitableRange>& r) {
      r.begin();
      r.end();
    })
  inline mux_many(AwaitableRange&& Range) : mux_many(Range.begin(), Range.end(), Count) {}

  /// Always suspends.
  inline bool await_ready() const noexcept { return false; }

  /// Suspends the outer coroutine until a result becomes ready (or resumes
  /// immediately if one is already ready).
  inline bool await_suspend(std::coroutine_handle<> Outer) noexcept {
    // mux_many can be awaited in a different context than where it was
    // created. Therefore, capture the continuation_executor at the await point.
    continuation_executor = tmc::detail::this_thread::executor();
    return tmc::detail::mux_await_suspend(
      remaining_count, Outer, continuation, sync_flags
    );
  }

  /// Returns the index of a single ready slot. The result indexes correspond to
  /// the indexes of the originally submitted awaitables, and the values can be
  /// accessed using `operator[]`. Results may become ready in any order, but
  /// each consumed (or forked) slot index will be returned exactly once per
  /// submission. When no submitted results remain, the index returned will be
  /// equal to the value of `end()`.
  [[nodiscard]] inline size_t await_resume() noexcept {
    auto slot = tmc::detail::mux_await_resume(remaining_count, sync_flags);
    if (slot != end()) {
      active_slots &= ~(TMC_ONE_BIT << slot);
    }
    return slot;
  }

  /// This type must be awaited as an lvalue (it is awaited repeatedly and is not
  /// movable). The awaiter is the group itself.
  mux_many& operator co_await() & noexcept { return *this; }

  /// Non-suspending check for a ready result. There are three outcomes:
  /// - a result is ready: it is consumed and its index returned (like `co_await`).
  /// - results are still pending but none are ready: returns `none()`.
  /// - no submitted results remain: returns `end()`.
  [[nodiscard]] inline size_t poll() noexcept {
    auto slot = tmc::detail::mux_poll(remaining_count, sync_flags);
    if (slot < TMC_PLATFORM_BITS) {
      active_slots &= ~(TMC_ONE_BIT << slot);
    }
    return slot;
  }

  /// Provides a sentinel value that can be compared against the value returned
  /// from `co_await` or `poll()`.
  inline constexpr size_t end() const noexcept { return TMC_PLATFORM_BITS; }

  /// Provides a sentinel value returned by `poll()` to indicate that no result is ready
  /// right now, but may become ready later. This is distinct from `end()`, which
  /// indicates that all submitted results have been consumed.
  inline constexpr size_t none() const noexcept { return TMC_PLATFORM_BITS + 1; }

  /// Returns the capacity of the mux, equal to the `Count` template argument.
  /// This is the maximum number of awaitables that may be active concurrently.
  inline constexpr size_t capacity() const noexcept { return Count; }

  /// Gets the ready result at the given index.
  TMC_TSAN_NO_SPECULATE inline std::add_lvalue_reference_t<Result>
  operator[](size_t Idx) noexcept
    requires(!std::is_void_v<Result>)
  {
    assert(Idx < result_arr.size());
    assert(
      !is_active(Idx) && "You may only call operator[] on a slot after its result has "
                         "been returned from co_await."
    );
    if constexpr (std::is_default_constructible_v<Result>) {
      return result_arr[Idx];
    } else {
      return *result_arr[Idx];
    }
  }

  /// Provided for convenience only - to expose the same API as the non-void
  /// version. Does nothing.
  inline void operator[]([[maybe_unused]] size_t Idx) noexcept
    requires(std::is_void_v<Result>)
  {}

  /// Returns true if slot `idx` is currently active: it has been forked but its
  /// result has not yet been returned from `co_await`. An active slot may not be
  /// re-forked. This is the negation of the precondition checked by `fork()`.
  inline bool is_active(size_t Idx) const noexcept {
    assert(Idx < Count && "is_active() index out of range.");
    return 0 != (active_slots & (TMC_ONE_BIT << Idx));
  }

  /// Returns the raw bitmap of active slots: bit `idx` is set if slot `idx` has
  /// been forked but its result has not yet been returned from `co_await`.
  inline size_t active_bitset() const noexcept { return active_slots; }

  /// Starts a new awaitable in the given slot and initiates it immediately on
  /// the specified executor and priority. The slot must be empty - either it was
  /// never started (when constructed with an empty constructor), or its previous
  /// result has already been consumed by `co_await`. The replacement awaitable must
  /// produce the same `Result` type as the group.
  ///
  /// `fork()` destroys the previous result in the given slot before initiating the
  /// replacement. If you need to use the result after this, you should move it out before
  /// calling `fork()`.
  ///
  /// `Executor` defaults to the current executor.
  /// `Priority` defaults to the current priority.
  ///
  /// This method is not thread-safe to call concurrently with itself or `co_await`.
  template <typename Aw, typename Exec = tmc::ex_any*>
  inline void fork(
    size_t Idx, Aw&& Awaitable, Exec&& Executor = tmc::current_executor(),
    size_t Priority = tmc::current_priority()
  ) {
    assert(Idx < Count && "fork() index out of range.");

    auto slotBit = TMC_ONE_BIT << Idx;
    assert(
      0 == (active_slots & slotBit) &&
      "You may only fork a slot after its previous result has been awaited."
    );
    active_slots |= slotBit;
    ++remaining_count;

    decltype(auto) known = tmc::detail::into_known<false>(static_cast<Aw&&>(Awaitable));
    using KnownAwaitable = std::remove_reference_t<decltype(known)>;
    static_assert(
      std::is_same_v<
        typename tmc::detail::get_awaitable_traits<KnownAwaitable>::result_type, Result>,
      "Replacement awaitable must have the same result type as the mux_many "
      "group."
    );

    // Destroy the previously-consumed result before initiating the replacement. This
    // prevents issues with results that own a resource which the replacement awaitable
    // needs to re-acquire, such as zero-copy queue scopes.
    if constexpr (!std::is_void_v<Result>) {
      result_arr[Idx] = ResultStorage{};
    }

    tmc::ex_any* exec = tmc::detail::get_executor_traits<Exec>::type_erased(Executor);
    auto continuationPriority = tmc::detail::this_thread::this_task().prio;

    prepare_work(known, Idx, continuationPriority);

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
  /// For HALO to work, you must `co_await mux.fork_clang()` immediately.
  class TMC_CORO_AWAIT_ELIDABLE mux_many_fork_clang : tmc::detail::AwaitTagNoGroupAsIs {
  public:
    mux_many_fork_clang() {}

    /// Never suspends.
    bool await_ready() const noexcept { return true; }

    /// Does nothing.
    void await_suspend(std::coroutine_handle<>) noexcept {}

    /// Does nothing.
    void await_resume() noexcept {}
  };

  /// Similar to `fork()` but allows the forked awaitable's allocation to be elided
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
  /// tmc::mux_many<int, 2> mux(tasks.begin(), tasks.end());
  /// for (size_t i = co_await mux; i != mux.end(); i = co_await mux) {
  ///   switch (i) {
  ///   case 0:
  ///     process(mux[0]);
  ///     co_await mux.fork_clang(0, task(0));
  ///     break;
  ///   case 1:
  ///     process(mux[1]);
  ///     co_await mux.fork_clang(1, task(1));
  ///     break;
  ///   default:
  ///     std::unreachable();
  ///   }
  /// }
  /// ```
  template <typename Aw, typename Exec = tmc::ex_any*>
  [[nodiscard("You must co_await fork_clang() immediately for HALO to be possible.")]]
  mux_many_fork_clang fork_clang(
    size_t Idx, TMC_CORO_AWAIT_ELIDABLE_ARGUMENT Aw&& Awaitable,
    Exec&& Executor = tmc::current_executor(), size_t Priority = tmc::current_priority()
  ) {
    fork(Idx, static_cast<Aw&&>(Awaitable), static_cast<Exec&&>(Executor), Priority);
    return mux_many_fork_clang{};
  }

  // This must be awaited and all child awaitables completed before destruction.
#ifndef NDEBUG
  ~mux_many() noexcept {
    assert(
      remaining_count == 0 &&
      "You must wait for all mux_many awaitables to complete before destroying it."
    );
  }
#endif

  // Not movable or copyable due to awaitables being initiated with pointers to
  // this.
  mux_many& operator=(const mux_many& other) = delete;
  mux_many(const mux_many& other) = delete;
  mux_many& operator=(mux_many&& other) = delete;
  mux_many(mux_many&& other) = delete;
};

// Deduction guides for the eager construction forms. Each deduces the `Result`
// type from the awaitables and leaves `Count` at its default
// (`TMC_PLATFORM_BITS - 1`), so a full-size fixed `std::array<Result, Count>` is
// used. To right-size the storage, name both template arguments explicitly -
// CTAD cannot deduce only `Result` while you fix `Count`, so there is no guide
// for the explicit-`Count` forms.
//
// The eager constructors are member templates whose own parameters do not appear
// in the class's `Result`/`Count` parameter list, so their implicit deduction
// guides cannot deduce those parameters and are discarded - these written guides
// are the only viable ones.

/// Eagerly initiates items in range [Begin, Begin + AwaitableCount).
template <
  typename AwaitableIter, typename Awaitable = std::iter_value_t<AwaitableIter>,
  typename Result = tmc::detail::awaitable_result_t<Awaitable>>
mux_many(AwaitableIter&&, size_t) -> mux_many<Result>;

/// Eagerly initiates items in range [Begin, End).
template <
  typename AwaitableIter, typename Awaitable = std::iter_value_t<AwaitableIter>,
  typename Result = tmc::detail::awaitable_result_t<Awaitable>>
mux_many(AwaitableIter&&, AwaitableIter&&) -> mux_many<Result>;

/// Eagerly initiates items in range [Begin, min(Begin + MaxCount, End)).
template <
  typename AwaitableIter, typename Awaitable = std::iter_value_t<AwaitableIter>,
  typename Result = tmc::detail::awaitable_result_t<Awaitable>>
mux_many(AwaitableIter&&, AwaitableIter&&, size_t) -> mux_many<Result>;

/// Eagerly initiates items in range [Range.begin(), Range.end()). The
/// `range_iter` default template argument keeps this guide from matching a
/// non-range single argument.
template <
  typename AwaitableRange,
  typename AwaitableIter = tmc::detail::range_iter<AwaitableRange>::type,
  typename Awaitable = std::iter_value_t<AwaitableIter>,
  typename Result = tmc::detail::awaitable_result_t<Awaitable>>
mux_many(AwaitableRange&&) -> mux_many<Result>;

} // namespace tmc
