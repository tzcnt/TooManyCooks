// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/current.hpp"
#include "tmc/detail/compat.hpp"
#include "tmc/detail/concepts_awaitable.hpp" // IWYU pragma: keep
#include "tmc/detail/concepts_work_item.hpp"
#include "tmc/detail/mixins.hpp"
#include "tmc/detail/thread_locals.hpp"
#include "tmc/ex_any.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <coroutine>
#include <type_traits>
#include <vector>

namespace tmc {

/// A container for imperatively forking awaitables, initiating each
/// awaitable immediately, and joining them all at a later time.
///
/// `Result` is the result type of the awaitables that will be forked.
/// Different types of awaitables may be forked with a single fork_group, as
/// long as they all return the same Result type.
///
/// `MaxCount` determines the result storage strategy:
/// - If `Result` is void, then `MaxCount` must be 0. No storage is needed, so
///   an unlimited number of awaitables can be forked.
/// - If `Result` is non-void and `MaxCount` is non-zero, a fixed-size
///   `std::array<Result, MaxCount>` is used. You can fork up to MaxCount
///   awaitables. If less than MaxCount awaitables are forked, the remaining
///   results in the result array are default-initialized.
/// - If `Result` is non-void and `MaxCount` is zero, a `std::vector<Result>`
///   is used. The size of this vector is fixed at construction time by passing
///   the `RuntimeMaxCount` parameter. You can fork up to RuntimeMaxCount
///   awaitables. If less than RuntimeMaxCount awaitables are forked, the
///   remaining results in the result vector are default-initialized.
template <size_t MaxCount, typename Result>
class aw_fork_group
    : public tmc::detail::resume_on_mixin<aw_fork_group<MaxCount, Result>> {
  friend class tmc::detail::resume_on_mixin<aw_fork_group<MaxCount, Result>>;

  size_t task_count;
  std::coroutine_handle<> continuation;
  tmc::ex_any* continuation_executor;
  std::atomic<ptrdiff_t> done_count;

  struct empty {};
  using ResultArray = std::conditional_t<
    std::is_void_v<Result>, empty,
    std::conditional_t<
      MaxCount == 0, std::vector<tmc::detail::result_storage_t<Result>>,
      std::array<tmc::detail::result_storage_t<Result>, MaxCount>>>;
  TMC_NO_UNIQUE_ADDRESS ResultArray result_arr;

  template <typename T>
  TMC_FORCE_INLINE void prepare_work(T& Task, size_t Idx) {
    tmc::detail::get_awaitable_traits<T>::set_continuation(Task, &continuation);
    tmc::detail::get_awaitable_traits<T>::set_continuation_executor(
      Task, &continuation_executor
    );
    tmc::detail::get_awaitable_traits<T>::set_done_count(Task, &done_count);

    if constexpr (!std::is_void_v<Result>) {
      tmc::detail::get_awaitable_traits<T>::set_result_ptr(
        Task, &result_arr[Idx]
      );
    }
  }

public:
  /// Constructs an empty fork group. It is recommended to use
  /// `tmc::fork_group()` instead of this constructor.
  aw_fork_group()
      : task_count{0},
        continuation_executor{tmc::detail::this_thread::executor},
        done_count{0} {}

  /// Constructs an empty fork group with runtime size.
  /// It is recommended to use `tmc::fork_group(RuntimeMaxCount)` instead of
  /// this constructor.
  aw_fork_group(size_t RuntimeMaxCount)
      : task_count{0},
        continuation_executor{tmc::detail::this_thread::executor},
        done_count{0} {
    if constexpr (MaxCount == 0 && !std::is_void_v<Result>) {
      assert(
        RuntimeMaxCount > 0 && "If Result is non-void, either MaxCount or "
                               "RuntimeMaxCount must be non-zero."
      );
      result_arr.resize(RuntimeMaxCount);
    }
  }

  /// Constructs a fork group with the first awaitable.
  /// It is recommended to use `tmc::fork_group(Awaitable)` instead of this
  /// constructor.
  template <typename Awaitable>
  [[nodiscard("You must co_await fork_group before it goes out of scope.")]]
  aw_fork_group(Awaitable&& Aw)
      : task_count{0},
        continuation_executor{tmc::detail::this_thread::executor},
        done_count{0} {
    fork(static_cast<Awaitable&&>(Aw));
  }

  /// Constructs a fork group with runtime size and the first awaitable.
  /// It is recommended to use `tmc::fork_group(RuntimeMaxCount, Awaitable)`
  /// instead of this constructor.
  template <typename Awaitable>
  [[nodiscard("You must co_await fork_group before it goes out of scope.")]]
  aw_fork_group(size_t RuntimeMaxCount, Awaitable&& Aw)
      : task_count{0},
        continuation_executor{tmc::detail::this_thread::executor},
        done_count{0} {
    if constexpr (MaxCount == 0 && !std::is_void_v<Result>) {
      assert(
        RuntimeMaxCount > 0 &&
        "RuntimeMaxCount must be non-zero for non-void Result."
      );
      result_arr.resize(RuntimeMaxCount);
    }
    fork(static_cast<Awaitable&&>(Aw));
  }

  // Not movable or copyable due to child tasks having pointers to this.
  aw_fork_group(aw_fork_group&&) = delete;
  aw_fork_group& operator=(aw_fork_group&&) = delete;
  aw_fork_group(const aw_fork_group&) = delete;
  aw_fork_group& operator=(const aw_fork_group&) = delete;

  /// Initiates Awaitable immediately on the specified executor and priority.
  /// `Awaitable` can be any awaitable type as long as its result type
  /// matches the fork_group's Result type.
  ///
  /// `Executor` defaults to the current executor.
  /// `Priority` defaults to the current priority.
  template <typename Awaitable, typename Exec = tmc::ex_any*>
  void fork(
    Awaitable&& Aw, Exec&& Executor = tmc::current_executor(),
    size_t Priority = tmc::current_priority()
  ) {
    static_assert(
      std::is_same_v<tmc::detail::awaitable_result_t<Awaitable>, Result>,
      "Awaitable result type must match fork_group Result type."
    );
    if constexpr (MaxCount != 0) {
      assert(task_count < MaxCount && "Cannot fork more tasks than MaxCount.");
    } else if constexpr (!std::is_void_v<Result>) {
      assert(
        task_count < result_arr.size() &&
        "Cannot fork more tasks than RuntimeMaxCount."
      );
    }
    tmc::ex_any* executor =
      tmc::detail::get_executor_traits<Exec>::type_erased(Executor);

    if constexpr (tmc::detail::get_awaitable_traits<Awaitable>::mode ==
                  tmc::detail::ASYNC_INITIATE) {
      // ASYNC_INITIATE types are initiated directly
      prepare_work(Aw, task_count);
      ++task_count;
      ++done_count;
      tmc::detail::get_awaitable_traits<Awaitable>::async_initiate(
        static_cast<Awaitable&&>(Aw), executor, Priority
      );
    } else if constexpr (tmc::detail::get_awaitable_traits<Awaitable>::mode ==
                         tmc::detail::WRAPPER) {
      // WRAPPER types need to be converted into a tmc::task before being
      // customized
      auto task = tmc::detail::into_known<false>(static_cast<Awaitable&&>(Aw));
      prepare_work(task, task_count);
      ++task_count;
      ++done_count;
      auto workItem = tmc::detail::into_initiate(std::move(task));
      tmc::detail::post_checked(
        executor, std::move(workItem), Priority, NO_HINT
      );
    } else {
      // TMC_TASK and COROUTINE are converted to work_item and posted
      prepare_work(Aw, task_count);
      ++task_count;
      ++done_count;
      auto workItem = tmc::detail::into_initiate(static_cast<Awaitable&&>(Aw));
      tmc::detail::post_checked(
        executor, std::move(workItem), Priority, NO_HINT
      );
    }
  }

  /// This is a dummy awaitable. Don't store this in a variable.
  /// For HALO to work, you must `co_await fg.fork_clang()` immediately.
  class TMC_CORO_AWAIT_ELIDABLE aw_fork_group_fork_clang
      : tmc::detail::AwaitTagNoGroupAsIs {
  public:
    aw_fork_group_fork_clang() {}

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
  /// WARNING: Don't allow coroutines passed into this to cross a loop boundary,
  /// or Clang will try to reuse the same allocation for multiple active
  /// coroutines.
  /// ```
  /// // the following usage will make your program CRASH!
  /// auto fg = tmc::fork_group();
  /// for (int i = 0; i < 2; i++) {
  ///   co_await fg.fork_clang(task(i));
  /// }
  /// co_await std::move(fg);
  /// ```
  ///
  /// IMPORTANT: This returns a dummy awaitable. For HALO to work, you should
  /// not store the dummy awaitable. Instead, `co_await` this expression
  /// immediately. Proper usage:
  /// ```
  /// // note that this is the same as the prior example
  /// // but without the loop, it works fine
  /// auto fg = tmc::fork_group();
  /// co_await fg.fork_clang(task(0));
  /// co_await fg.fork_clang(task(1));
  /// co_await std::move(fg);
  /// ```
  template <typename Awaitable, typename Exec = tmc::ex_any*>
  [[nodiscard(
    "You must co_await fork_clang() immediately for HALO to be possible."
  )]]
  aw_fork_group_fork_clang fork_clang(
    TMC_CORO_AWAIT_ELIDABLE_ARGUMENT Awaitable&& Aw,
    Exec&& Executor = tmc::current_executor(),
    size_t Priority = tmc::current_priority()
  ) {
    fork(static_cast<Awaitable&&>(Aw), static_cast<Exec&&>(Executor), Priority);
    return aw_fork_group_fork_clang{};
  }

  /// Always suspends.
  bool await_ready() const noexcept {
    // Always suspends, due to the possibility to resume on another executor.
    return false;
  }

  /// Suspends the outer coroutine and waits for the forked awaitables to
  /// complete.
  std::coroutine_handle<>
  await_suspend(std::coroutine_handle<> Outer) noexcept {
#ifndef NDEBUG
    assert(done_count.load() >= 0 && "You may only co_await this once.");
#endif
    continuation = Outer;
    std::coroutine_handle<> next;
    // This logic is necessary because we submitted all child tasks before
    // the parent suspended. Allowing parent to be resumed before it
    // suspends would be UB. Therefore we need to block the resumption until
    // here.
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
    return next;
  }

  /// Returns the result array.
  TMC_AWAIT_RESUME std::add_rvalue_reference_t<ResultArray>
  await_resume() noexcept
    requires(!std::is_void_v<Result>)
  {
    return std::move(result_arr);
  }

  /// Does nothing.
  void await_resume() noexcept
    requires(std::is_void_v<Result>)
  {}

  /// After you `co_await` this group, you may call `reset()` to make it usable
  /// again. This allows you to accumulate and `co_await` another group of
  /// awaitables.
  ///
  /// For fixed-size fork_groups (MaxCount != 0), this resets the task count
  /// and done count, allowing the same result storage to be reused.
  ///
  /// For runtime-sized fork_groups (MaxCount == 0 with non-void Result), this
  /// overload cannot be used. Instead, use `reset(RuntimeMaxCount)` to specify
  /// a new capacity.
  void reset() noexcept
    requires(MaxCount != 0 || std::is_void_v<Result>)
  {
    task_count = 0;
    done_count = 0;
  }

  /// After you `co_await` this group, you may call `reset(RuntimeMaxCount)` to
  /// make it usable again with a new capacity.
  ///
  /// Only available for runtime-sized fork_groups (MaxCount == 0 with non-void
  /// Result). The result vector is resized to the new RuntimeMaxCount.
  void reset(size_t RuntimeMaxCount) noexcept
    requires(MaxCount == 0 && !std::is_void_v<Result>)
  {
    task_count = 0;
    done_count = 0;
    result_arr.resize(RuntimeMaxCount);
  }

  /// Returns the maximum capacity of the fork_group as determined by the
  /// MaxCount or RuntimeMaxCount parameters. Note that this function is only
  /// guaranteed to return a valid value before this is `co_await` ed.
  ///
  /// If the capacity is unlimited, this will return
  /// `std::numeric_limits<size_t>::max()`, i.e. `static_cast<size_t>(-1)`.
  size_t capacity() noexcept {
    if constexpr (std::is_void_v<Result>) {
      return static_cast<size_t>(-1);
    } else {
      // After co_await, if this is a vector, its size may be set to 0.
      return result_arr.size();
    }
  }

  /// Returns the number of awaitables actually posted to the fork_group.
  /// This value will be reset to 0 when `reset()` is called.
  size_t size() noexcept { return task_count; }
};

/// Constructs an empty fork group with default template parameters.
///
/// `MaxCount` is the maximum number of awaitables that will be forked.
/// - If `Result` is void (default), then `MaxCount` must be 0. This allows you
/// to fork an unlimited number of void-returning awaitables.
/// - If `Result` is non-void and `MaxCount` is non-zero, a fixed-size result
/// array is used. If less than `MaxCount` awaitables are forked, the remaining
/// results will be default-initialized.
/// - If `Result` is non-void and `MaxCount` is 0, you must use the
///   `fork_group<Result>(RuntimeMaxCount)` overload instead.
///
/// `Result` is the result type of the awaitables that will be forked.
template <size_t MaxCount = 0, typename Result = void>
aw_fork_group<MaxCount, Result> fork_group()
  requires(MaxCount != 0 || std::is_void_v<Result>)
{
  static_assert(
    (MaxCount == 0) == std::is_void_v<Result>,
    "Result storage is only needed for non-void results."
  );
  return aw_fork_group<MaxCount, Result>{};
}

/// Constructs a fork group with the first awaitable, deducing the result type
/// from the awaitable's return type.
///
/// `MaxCount` is the maximum number of awaitables that will be forked.
/// - If `Result` is void, then `MaxCount` must be 0. This allows you
/// to fork an unlimited number of void-returning awaitables.
/// - If `Result` is non-void and `MaxCount` is non-zero, a fixed-size result
/// array is used. If less than `MaxCount` awaitables are forked, the remaining
/// results will be default-initialized.
/// - If `Result` is non-void and `MaxCount` is 0, you must use the
///   `fork_group<Result>(RuntimeMaxCount)` overload instead.
template <
  size_t MaxCount = 0, typename Awaitable,
  typename Result = tmc::detail::awaitable_result_t<Awaitable>>
[[nodiscard("You must co_await fork_group before it goes out of scope.")]]
aw_fork_group<MaxCount, Result> fork_group(Awaitable&& Aw)
  requires(MaxCount != 0 || std::is_void_v<Result>)
{
  static_assert(
    (MaxCount == 0) == std::is_void_v<Result>,
    "Result storage is only needed for non-void results."
  );
  return aw_fork_group<MaxCount, Result>(static_cast<Awaitable&&>(Aw));
}

/// Constructs an empty fork group with runtime-specified maximum size.
///
/// `Result` is the result type of the awaitables that will be forked.
/// `Result` must be non-void.
///
/// `RuntimeMaxCount` is the maximum number of awaitables that may be forked. A
/// `std::vector` of this size will be pre-allocated to store the results.
/// If less than `RuntimeMaxCount` awaitables are forked, the remaining results
/// will be default-initialized.
template <typename Result>
  requires(!std::is_void_v<Result>)
[[nodiscard("You must co_await fork_group before it goes out of scope.")]]
aw_fork_group<0, Result> fork_group(size_t RuntimeMaxCount) {
  return aw_fork_group<0, Result>(RuntimeMaxCount);
}

/// Constructs a fork group with runtime-specified maximum size and the first
/// awaitable, deducing the result type from the awaitable. The deduced result
/// type must be non-void.
///
/// `RuntimeMaxCount` is the maximum number of awaitables that may be forked.
/// A `std::vector` of this size will be pre-allocated to store the results.
/// If less than `RuntimeMaxCount` awaitables are forked, the remaining results
/// will be default-initialized.
template <
  typename Awaitable,
  typename Result = tmc::detail::awaitable_result_t<Awaitable>>
  requires(!std::is_void_v<Result>)
[[nodiscard("You must co_await fork_group before it goes out of scope.")]]
aw_fork_group<0, Result> fork_group(size_t RuntimeMaxCount, Awaitable&& Aw) {
  return aw_fork_group<0, Result>(
    RuntimeMaxCount, static_cast<Awaitable&&>(Aw)
  );
}

namespace detail {
template <typename Result, size_t MaxCount>
struct awaitable_traits<aw_fork_group<MaxCount, Result>> {
  static constexpr configure_mode mode = WRAPPER;
  using result_type = std::conditional_t<
    std::is_void_v<Result>, void,
    std::conditional_t<
      MaxCount == 0, std::vector<tmc::detail::result_storage_t<Result>>,
      std::array<tmc::detail::result_storage_t<Result>, MaxCount>>>;
  using self_type = aw_fork_group<MaxCount, Result>;
  using awaiter_type = aw_fork_group<MaxCount, Result>&&;

  static awaiter_type get_awaiter(self_type&& Aw) noexcept {
    return std::forward<self_type>(Aw);
  }
};
} // namespace detail
} // namespace tmc
