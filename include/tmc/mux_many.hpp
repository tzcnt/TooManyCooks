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
#include "tmc/ex_any.hpp"
#include "tmc/work_item.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <coroutine>
#include <cstddef>
#include <iterator>
#include <type_traits>

namespace tmc {

/// A standalone, reusable result-multiplexer over a homogeneous set of awaitable
/// slots. Like `tmc::spawn_many()` it initiates a set of awaitables that all
/// produce the same `Result` type, but rather than returning all results at
/// once, each result is made available as it becomes ready: `co_await`ing the
/// group returns the index of a single ready slot. Unlike `spawn_many()`,
/// passing the awaitables up front is optional.
///
/// Results are always stored in a fixed-size `std::array<Result, Count>`. The
/// group is synchronized by an atomic bitmask, so a `mux_many` holds at most
/// `TMC_PLATFORM_BITS - 1` slots (63 on 64-bit platforms, 31 on 32-bit). `Count`
/// defaults to that maximum; provide a smaller value to shrink the storage when
/// you know the group will hold fewer slots.
///
/// There are two families of construction:
///
/// 1. With awaitables (an iterator + count, a begin/end range, a begin/end range
/// + max count, or a range object). The `Result` type is deduced from the
/// awaitables via CTAD, and `Count` is left at its default. This eagerly
/// initiates the awaitables, exactly like `tmc::spawn_many(...)`, but their
/// results are consumed one at a time as they become ready:
/// ```
/// auto mux = tmc::mux_many(tasks.begin(), tasks.end());
/// for (size_t i = co_await mux; i != mux.end(); i = co_await mux) {
///   use(mux[i]);
/// }
/// ```
/// To right-size the `std::array` storage, name both template arguments
/// explicitly - CTAD cannot deduce only `Result` while you fix `Count`:
/// ```
/// auto mux = tmc::mux_many<int, N>(tasks.begin());              // exactly N
/// auto mux = tmc::mux_many<int, N>(tasks.begin(), tasks.end()); // up to N
/// ```
///
/// 2. Empty (the `Result` type must be provided explicitly; `Count` is
/// optional). This only allocates the result storage; no awaitables are
/// initiated. You start individual slots with `fork()` whenever you like,
/// optionally choosing a specific executor and priority for each one:
/// ```
/// auto mux = tmc::mux_many<int, 2>(); // storage for 2 slots
/// auto mux = tmc::mux_many<int>();    // storage for TMC_PLATFORM_BITS - 1 slots
/// mux.fork(0, make_int_task());
/// mux.fork(1, make_int_task());
/// for (size_t i = co_await mux; i != mux.end(); i = co_await mux) { ... }
/// ```
///
/// In both cases, after a slot's result has been consumed by `co_await`, you may
/// call `fork()` to launch a fresh awaitable into that slot. The replacement
/// awaitable must produce the same `Result` type. This is what allows a
/// `mux_many` to maintain a fixed level of concurrency: as each result is
/// consumed, replace it with new work.
///
/// `Result` is the result type produced by every awaitable in the group.
/// void-returning awaitables are supported (no result storage is allocated, and
/// `operator[]` is a no-op).
template <typename Result, size_t Count = TMC_PLATFORM_BITS - 1>
class mux_many : private tmc::detail::AwaitTagNoGroupCoAwaitLvalue {
  // Tasks are synchronized via an atomic bitmask with only 63 (or 31, on 32-bit)
  // slots for tasks.
  static_assert(
    Count > 0 && Count < TMC_PLATFORM_BITS,
    "mux_many supports between 1 and 63 awaitables (31 on 32-bit platforms)."
  );

  // The count of submitted-but-not-yet-consumed results.
  ptrdiff_t remaining_count;
  std::coroutine_handle<> continuation;
  tmc::ex_any* continuation_executor;
  // The number of slots (the valid index range for fork()). For the eager
  // constructors this is the number of awaitables actually initiated; for the
  // empty constructors it is the requested capacity.
  size_t task_count;
  // Bitmap of slots that have been forked but not yet returned from co_await.
  // A set bit means the slot is active: its result is pending or ready but has
  // not been consumed, so the slot may not be re-forked. Non-atomic; only
  // mutated by the (single-threaded) owner.
  size_t active_slots;
  // the atomic synchronization variable that coordinates between this and
  // awaitable_customizer (task's final_suspend)
  std::atomic<size_t> sync_flags;

  // Non-default-constructible results are stored in a std::optional. This is not visible
  // to the user; when get() is called, a reference to the constructed object is returned.
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

  void set_done_count(size_t NumTasks) {
    remaining_count = static_cast<ptrdiff_t>(NumTasks);
    task_count = NumTasks;
    active_slots = (TMC_ONE_BIT << NumTasks) - 1;
    sync_flags.store(tmc::detail::task_flags::EACH, std::memory_order_release);
  }

public:
  /// Creates the result storage but does not initiate any awaitables. The
  /// `Result` type must be provided explicitly; `Count` is optional and defaults
  /// to `TMC_PLATFORM_BITS - 1`, e.g. `tmc::mux_many<Result, Count>()`. Use
  /// `fork()` to initiate work into individual slots.
  mux_many() : continuation_executor{tmc::detail::this_thread::executor()} {
    set_done_count(0);
    task_count = Count;
  }

  /// Eagerly initiates `min(TaskCount, Count)` awaitables from `[Iter, ...)`.
  template <typename TaskIter>
  inline mux_many(TaskIter Iter, size_t TaskCount)
      : continuation_executor{tmc::detail::this_thread::executor()} {
    tmc::ex_any* executor = tmc::detail::this_thread::executor();
    size_t prio = tmc::detail::this_thread::this_task().prio;
    using Awaitable = std::remove_cvref_t<std::iter_value_t<TaskIter>>;
    constexpr auto mode = tmc::detail::get_awaitable_traits<Awaitable>::mode;
    static_assert(
      mode != tmc::detail::UNKNOWN, "This doesn't appear to be an awaitable."
    );

    using WorkItem =
      std::conditional_t<mode == tmc::detail::ASYNC_INITIATE, Awaitable, work_item>;
    using WorkItemArray = std::array<WorkItem, Count>;

    size_t size = TaskCount < Count ? TaskCount : Count;
    size_t continuationPriority = tmc::detail::this_thread::this_task().prio;

    if constexpr (mode == tmc::detail::ASYNC_INITIATE) {
      // ASYNC_INITIATE types may possibly not be stored in an array
      // (no default/copy constructor), so initiate them individually.
      set_done_count(size);
      for (size_t i = 0; i < size; ++i) {
        TMC_DISABLE_WARNING_PESSIMIZING_MOVE_BEGIN
        auto t = std::move(*Iter);
        TMC_DISABLE_WARNING_PESSIMIZING_MOVE_END
        prepare_work(t, i, continuationPriority);
        tmc::detail::get_awaitable_traits<Awaitable>::async_initiate(
          std::move(t), executor, prio
        );
        ++Iter;
      }
    } else { // mode != ASYNC_INITIATE
      // Batch other types of awaitables into a work_item array
      // and submit them in bulk.
      WorkItemArray taskArr;

      for (size_t i = 0; i < size; ++i) {
        TMC_DISABLE_WARNING_PESSIMIZING_MOVE_BEGIN
        auto t = tmc::detail::into_known<false>(std::move(*Iter));
        TMC_DISABLE_WARNING_PESSIMIZING_MOVE_END
        prepare_work(t, i, continuationPriority);
        taskArr[i] = tmc::detail::into_initiate(std::move(t));
        ++Iter;
      }

      set_done_count(size);
      if (size != 0) {
        tmc::detail::post_bulk_checked(executor, taskArr.data(), size, prio);
      }
    }
  }

  /// Eagerly initiates awaitables from `[Begin, min(Begin + MaxCount, End))`,
  /// never more than `Count`.
  template <typename TaskIter>
  inline mux_many(TaskIter Begin, TaskIter End, size_t MaxCount)
    requires(requires(TaskIter a, TaskIter b) {
              ++a;
              *a;
              a != b;
            })
      : continuation_executor{tmc::detail::this_thread::executor()} {
    tmc::ex_any* executor = tmc::detail::this_thread::executor();
    size_t prio = tmc::detail::this_thread::this_task().prio;
    size_t size = MaxCount < Count ? MaxCount : Count;

    using Awaitable = std::remove_cvref_t<std::iter_value_t<TaskIter>>;
    constexpr auto mode = tmc::detail::get_awaitable_traits<Awaitable>::mode;
    static_assert(
      mode != tmc::detail::UNKNOWN, "This doesn't appear to be an awaitable."
    );

    using WorkItem =
      std::conditional_t<mode == tmc::detail::ASYNC_INITIATE, Awaitable, work_item>;
    using WorkItemArray = std::array<WorkItem, Count>;

    size_t continuationPriority = tmc::detail::this_thread::this_task().prio;

    size_t taskCount = 0;
    if constexpr (mode == tmc::detail::ASYNC_INITIATE &&
                  requires(TaskIter a, TaskIter b) { a - b; }) {
      // ASYNC_INITIATE types may possibly not be stored in an array (no
      // default/copy constructor). Try to sidestep this by initiating them
      // individually. For this block we also need to be able to calculate the
      // actual size beforehand.
      size_t actualSize = static_cast<size_t>(End - Begin);
      if (size < actualSize) {
        actualSize = size;
      }
      set_done_count(actualSize);
      while (Begin != End && taskCount < actualSize) {
        TMC_DISABLE_WARNING_PESSIMIZING_MOVE_BEGIN
        auto t = std::move(*Begin);
        TMC_DISABLE_WARNING_PESSIMIZING_MOVE_END
        prepare_work(t, taskCount, continuationPriority);
        tmc::detail::get_awaitable_traits<Awaitable>::async_initiate(
          std::move(t), executor, prio
        );
        ++Begin;
        ++taskCount;
      }
    } else {
      // Batch other types of awaitables into a work_item array and submit them
      // in bulk. The iterator could produce fewer than `size` awaitables, so
      // count them; it could also produce more, so stop after taking `size`.
      // The result storage is a fixed-size array, so result pointers are stable
      // and can be bound as each awaitable is prepared - no second pass or
      // separate type-preserving buffer (as an unsized std::vector would need)
      // is required, even for COROUTINE-mode awaitables.
      WorkItemArray taskArr;
      while (Begin != End && taskCount < size) {
        TMC_DISABLE_WARNING_PESSIMIZING_MOVE_BEGIN
        auto t = tmc::detail::into_known<false>(std::move(*Begin));
        TMC_DISABLE_WARNING_PESSIMIZING_MOVE_END
        prepare_work(t, taskCount, continuationPriority);
        taskArr[taskCount] = tmc::detail::into_initiate(std::move(t));
        ++Begin;
        ++taskCount;
      }

      if (taskCount == 0) {
        set_done_count(0);
        return;
      }
      if constexpr (mode == tmc::detail::ASYNC_INITIATE) {
        set_done_count(taskCount);
        for (size_t i = 0; i < taskCount; ++i) {
          tmc::detail::get_awaitable_traits<Awaitable>::async_initiate(
            std::move(taskArr[i]), executor, prio
          );
        }
      } else {
        set_done_count(taskCount);
        tmc::detail::post_bulk_checked(executor, taskArr.data(), taskCount, prio);
      }
    }
  }

  /// Eagerly initiates exactly `Count` awaitables from `[Begin, Begin + Count)`.
  /// Name both template arguments explicitly, e.g.
  /// `tmc::mux_many<Result, Count>(Begin)`. The constraint rejects range objects
  /// (which have `begin()`/`end()`) so that a single range argument selects the
  /// range constructor below instead.
  template <typename AwaitableIter>
    requires(!requires(std::remove_reference_t<AwaitableIter>& r) {
      r.begin();
      r.end();
    })
  inline mux_many(AwaitableIter&& Begin)
      : mux_many(std::forward<AwaitableIter>(Begin), Count) {}

  /// Eagerly initiates awaitables from `[Begin, End)` (never more than `Count`).
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
          std::forward<AwaitableIter>(Begin), std::forward<AwaitableIter>(End),
          TMC_ALL_ONES
        ) {}

  /// Eagerly initiates awaitables from `[Range.begin(), Range.end())` (never more
  /// than `Count`). The `Result` type is deduced via CTAD.
  template <typename AwaitableRange>
    requires(requires(std::remove_reference_t<AwaitableRange>& r) {
      r.begin();
      r.end();
    })
  inline mux_many(AwaitableRange&& Range)
      : mux_many(Range.begin(), Range.end(), TMC_ALL_ONES) {}

  /// Suspends if there are no ready results.
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

  /// Returns the index of a single ready slot. The result indexes correspond to
  /// the indexes of the originally submitted awaitables, and the values can be
  /// accessed using `operator[]`. Results may become ready in any order, but
  /// each consumed (or forked) slot index will be returned exactly once per
  /// submission. When no submitted results remain, the index returned will be
  /// equal to the value of `end()`.
  TMC_AWAIT_RESUME inline size_t await_resume() noexcept {
    auto slot = tmc::detail::result_each_await_resume(remaining_count, sync_flags);
    if (slot != end()) {
      active_slots &= ~(TMC_ONE_BIT << slot);
    }
    return slot;
  }

  /// This type must be awaited as an lvalue (it is awaited repeatedly and is not
  /// movable). The awaiter is the group itself.
  mux_many& operator co_await() & noexcept { return *this; }

  /// Provides a sentinel value that can be compared against the value returned
  /// from co_await.
  inline size_t end() noexcept { return 64; }

  /// Gets the ready result at the given index.
  inline std::add_lvalue_reference_t<Result> operator[](size_t idx) noexcept
    requires(!std::is_void_v<Result>)
  {
    assert(idx < result_arr.size());
    if constexpr (std::is_default_constructible_v<Result>) {
      return result_arr[idx];
    } else {
      return *result_arr[idx];
    }
  }

  /// Provided for convenience only - to expose the same API as the non-void
  /// version. Does nothing.
  inline void operator[]([[maybe_unused]] size_t idx) noexcept
    requires(std::is_void_v<Result>)
  {}

  /// Returns true if slot `idx` is currently active: it has been forked but its
  /// result has not yet been returned from `co_await`. An active slot may not be
  /// re-forked. This is the negation of the precondition checked by `fork()`.
  inline bool is_active(size_t idx) const noexcept {
    return 0 != (active_slots & (TMC_ONE_BIT << idx));
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
  /// Only the awaitable's dispatch is customized; regardless of where each slot
  /// runs, the awaiting coroutine always resumes on the executor that was
  /// current when this `mux_many` was constructed. The eager constructors do not
  /// offer this per-awaitable customization - they initiate every awaitable on
  /// the executor and priority current at construction. To customize each
  /// awaitable, use an empty constructor and `fork()` each slot.
  ///
  /// This method is not thread-safe.
  template <typename T, typename Exec = tmc::ex_any*>
  inline void fork(
    size_t idx, T&& Task, Exec&& Executor = tmc::current_executor(),
    size_t Priority = tmc::current_priority()
  ) {
    if (idx >= task_count) [[unlikely]] {
      assert(false && "fork() index out of range.");
      return;
    }
    auto slotBit = TMC_ONE_BIT << idx;
    assert(
      0 == (active_slots & slotBit) &&
      "You may only fork a slot after its previous result has been awaited."
    );
    active_slots |= slotBit;
    ++remaining_count;

    decltype(auto) known = tmc::detail::into_known<false>(static_cast<T&&>(Task));
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
      result_arr[idx] = ResultStorage{};
    }

    tmc::ex_any* exec = tmc::detail::get_executor_traits<Exec>::type_erased(Executor);
    auto continuationPriority = tmc::detail::this_thread::this_task().prio;

    constexpr auto mode = tmc::detail::get_awaitable_traits<KnownAwaitable>::mode;
    if constexpr (mode == tmc::detail::ASYNC_INITIATE) {
      prepare_work(known, idx, continuationPriority);
      // Forward known with its original value category.
      tmc::detail::get_awaitable_traits<KnownAwaitable>::async_initiate(
        static_cast<decltype(known)&&>(known), exec, Priority
      );
    } else {
      prepare_work(known, idx, continuationPriority);
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

  /// Similar to `fork()` but allows the forked task's allocation to be elided
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
  /// auto mux = tmc::mux_many<int, 2>();
  /// co_await mux.fork_clang(0, task(0));
  /// co_await mux.fork_clang(1, task(1));
  /// for (size_t i = co_await mux; i != mux.end(); i = co_await mux) { ... }
  /// ```
  template <typename T, typename Exec = tmc::ex_any*>
  [[nodiscard("You must co_await fork_clang() immediately for HALO to be possible.")]]
  mux_many_fork_clang fork_clang(
    size_t idx, TMC_CORO_AWAIT_ELIDABLE_ARGUMENT T&& Task,
    Exec&& Executor = tmc::current_executor(), size_t Priority = tmc::current_priority()
  ) {
    fork(idx, static_cast<T&&>(Task), static_cast<Exec&&>(Executor), Priority);
    return mux_many_fork_clang{};
  }

  // This must be awaited and all child tasks completed before destruction.
#ifndef NDEBUG
  ~mux_many() noexcept {
    assert(remaining_count == 0 && "You must submit or co_await this.");
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
//
// Note: You must ensure the iterators/range remain in scope until the group has
// been `co_await` ed.

/// Eagerly initiates items in range [Begin, Begin + TaskCount).
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
