// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/compat.hpp"
#include "tmc/detail/concepts_awaitable.hpp" // IWYU pragma: keep
#include "tmc/detail/mixins.hpp"
#include "tmc/detail/thread_locals.hpp"
#include "tmc/ex_any.hpp"
#include "tmc/spawn_many.hpp"
#include "tmc/task.hpp"

#include <array>
#include <cassert>
#include <coroutine>
#include <type_traits>
#include <vector>

namespace tmc {
/// Similar to `tmc::spawn_many()`, but allows for imperative construction of
/// the task group. Awaitables will be collected, but not initiated until you
/// co_await this. Unlike `tmc::fork_group`, this type is movable, so it can be
/// returned from functions or passed around.
///
/// `Awaitable` is the type of awaitable that will be added to the group.
/// All awaitables added to the group must be of the same type.
///
/// `MaxCount` is the maximum number of awaitables that may be added.
/// - If `MaxCount` is non-zero, a fixed-size `std::array` will be used.
/// You can add up to this many awaitables to the group. If less than
/// MaxCount awaitables are added, the remaining results in the result array
/// will be default-initialized.
/// - If `MaxCount` is zero, an unlimited number of awaitables can be added. The
/// results will be returned in a right-sized `std::vector`.
template <size_t MaxCount, typename Awaitable>
class aw_spawn_group
    : public tmc::detail::run_on_mixin<aw_spawn_group<MaxCount, Awaitable>>,
      public tmc::detail::resume_on_mixin<aw_spawn_group<MaxCount, Awaitable>>,
      public tmc::detail::with_priority_mixin<
        aw_spawn_group<MaxCount, Awaitable>> {
  friend class tmc::detail::run_on_mixin<aw_spawn_group<MaxCount, Awaitable>>;
  friend class tmc::detail::resume_on_mixin<
    aw_spawn_group<MaxCount, Awaitable>>;
  friend class tmc::detail::with_priority_mixin<
    aw_spawn_group<MaxCount, Awaitable>>;

  using Result = tmc::detail::awaitable_result_t<Awaitable>;
  using AwaitableArray = std::conditional_t<
    MaxCount == 0, std::vector<Awaitable>, std::array<Awaitable, MaxCount>>;
  AwaitableArray tasks;
  size_t task_count;
  size_t prio;
  tmc::ex_any* executor;
  tmc::ex_any* continuation_executor;
  std::coroutine_handle<> continuation;

public:
  /// Constructs an empty spawn group. It is recommended to use
  /// `tmc::spawn_group()` instead of this constructor.
  aw_spawn_group()
      : task_count{0}, prio{tmc::detail::this_thread::this_task.prio},
        executor{tmc::detail::this_thread::executor},
        continuation_executor{tmc::detail::this_thread::executor} {}

  /// Constructs an empty spawn group. It is recommended to use
  /// `tmc::spawn_group(Awaitable&& Aw)` instead of this constructor.
  [[nodiscard("You must co_await spawn_group before it goes out of scope.")]]
  aw_spawn_group(Awaitable&& Aw)
      : task_count{1}, prio{tmc::detail::this_thread::this_task.prio},
        executor{tmc::detail::this_thread::executor},
        continuation_executor{tmc::detail::this_thread::executor} {
    if constexpr (MaxCount == 0) {
      tasks.push_back(std::move(Aw));
    } else {
      tasks[0] = std::move(Aw);
    }
  }

  aw_spawn_group(aw_spawn_group&& Other)
      : tasks(std::move(Other.tasks)), task_count(std::move(Other.task_count)),
        prio(std::move(Other.prio)), executor(std::move(Other.executor)),
        continuation_executor(std::move(Other.continuation_executor)) {}
  aw_spawn_group& operator=(aw_spawn_group&& Other) {
    tasks = std::move(Other.tasks);
    task_count = std::move(Other.task_count);
    prio = std::move(Other.prio);
    executor = std::move(Other.executor);
    continuation_executor = std::move(Other.continuation_executor);
    return *this;
  }
  aw_spawn_group(const aw_spawn_group&) = delete;
  aw_spawn_group& operator=(const aw_spawn_group&) = delete;

  /// Adds an awaitable to the group.
  /// The awaitable will be initiated when the group is co_awaited.
  ///
  /// This method is not thread-safe. If multiple threads need to add work
  /// to the same `spawn_group`, they must be externally synchronized.
  void add(Awaitable&& Aw) {
    if constexpr (MaxCount == 0) {
      tasks.push_back(std::move(Aw));
    } else {
      assert(task_count < MaxCount && "Cannot add more tasks than MaxCount.");
      tasks[task_count] = std::move(Aw);
    }
    ++task_count;
  }

  /// This is a dummy awaitable. Don't store this in a variable.
  /// For HALO to work, you must `co_await sg.add_clang()` immediately.
  class TMC_CORO_AWAIT_ELIDABLE aw_spawn_group_add_clang
      : tmc::detail::AwaitTagNoGroupAsIs {
  public:
    aw_spawn_group_add_clang() {}

    /// Never suspends.
    bool await_ready() const noexcept { return true; }

    /// Does nothing.
    void await_suspend(std::coroutine_handle<>) noexcept {}

    /// Does nothing.
    void await_resume() noexcept {}
  };

  /// Similar to `add()` but  allows the child task's
  /// allocation to be elided by combining it into the parent's allocation
  /// (HALO). This works by using specific attributes that are only available on
  /// Clang 20+. You can safely call this function on other compilers, but no
  /// HALO-specific optimizations will be applied.
  ///
  /// This method is not thread-safe. If multiple threads need to add work
  /// to the same `spawn_group`, they must be externally synchronized.
  ///
  /// WARNING: Don't allow coroutines passed into this to cross a loop boundary,
  /// or Clang will try to reuse the same allocation for multiple active
  /// coroutines.
  /// ```
  /// // the following usage will make your program CRASH!
  /// auto sg = tmc::spawn_group();
  /// for (int i = 0; i < 2; i++) {
  ///   co_await sg.add_clang(task(i));
  /// }
  /// co_await std::move(sg);
  /// ```
  ///
  /// IMPORTANT: This returns a dummy awaitable.  For HALO to work, you should
  /// not store the dummy awaitable. Instead, `co_await` this expression
  /// immediately. Proper usage:
  /// ```
  /// // note that this is the same as the prior example
  /// // but without the loop, it works fine
  /// auto sg = tmc::spawn_group();
  /// co_await sg.add_clang(task(0));
  /// co_await sg.add_clang(task(1));
  /// co_await std::move(sg);
  /// ```
  [[nodiscard(
    "You must co_await add_clang() immediately for HALO to be possible."
  )]]
  aw_spawn_group_add_clang
  add_clang(TMC_CORO_AWAIT_ELIDABLE_ARGUMENT Awaitable&& Aw) {
    add(std::move(Aw));
    return aw_spawn_group_add_clang{};
  }

  /// Initiates all of the wrapped awaitables and waits for them to complete.
  aw_spawn_many_impl<Result, MaxCount, false, false>
  operator co_await() && noexcept {
    if constexpr (MaxCount == 0) {
      return tmc::spawn_many(tasks.begin(), tasks.size())
        .with_priority(prio)
        .run_on(executor)
        .resume_on(continuation_executor)
        .operator co_await();
    } else {
      return tmc::spawn_many<MaxCount>(
               tasks.begin(), tasks.begin() + static_cast<ptrdiff_t>(task_count)
      )
        .with_priority(prio)
        .run_on(executor)
        .resume_on(continuation_executor)
        .operator co_await();
    }
  }

  /// Initiates all of the wrapped awaitables, without suspending the
  /// current coroutine. You must join them by awaiting the
  /// returned awaitable before it goes out of scope.
  [[nodiscard(
    "You must co_await the fork() awaitable before it goes out of scope."
  )]]
  aw_spawn_many_fork<Result, MaxCount, false> fork() && noexcept {
    if constexpr (MaxCount == 0) {
      return tmc::spawn_many(tasks.begin(), tasks.size())
        .with_priority(prio)
        .run_on(executor)
        .resume_on(continuation_executor)
        .fork();
    } else {
      return tmc::spawn_many<MaxCount>(
               tasks.begin(), tasks.begin() + static_cast<ptrdiff_t>(task_count)
      )
        .with_priority(prio)
        .run_on(executor)
        .resume_on(continuation_executor)
        .fork();
    }
  }

  /// After you `co_await` this group, you may call `reset()` to make it usable
  /// again. This allows you to accumulate and `co_await` another group of
  /// awaitables.
  ///
  /// It is also valid to call this before `co_await` ing for the first time.
  void reset() noexcept {
    if constexpr (MaxCount == 0) {
      tasks.clear();
    }
    task_count = 0;
  }

  /// Returns the maximum capacity of the spawn_group as determined by the
  /// MaxCount parameter.
  ///
  /// If the capacity is unlimited, this will return
  /// `std::numeric_limits<size_t>::max()`, i.e. `static_cast<size_t>(-1)`.
  size_t capacity() const noexcept {
    if constexpr (MaxCount == 0) {
      return static_cast<size_t>(-1);
    } else {
      return MaxCount;
    }
  }

  /// Returns the number of awaitables actually posted to the spawn_group.
  /// This value will be reset to 0 when `reset()` is called.
  size_t size() const noexcept { return task_count; }
};

/// Constructs an empty spawn group with default template parameters.
template <size_t MaxCount = 0, typename Awaitable = tmc::task<void>>
aw_spawn_group<MaxCount, std::remove_cvref_t<Awaitable>> spawn_group() {
  return aw_spawn_group<MaxCount, std::remove_cvref_t<Awaitable>>{};
}

/// Constructs a spawn group with the first awaitable, deducing the awaitable
/// type from the argument.
///
/// `MaxCount` is the maximum number of awaitables that will be added.
/// - If `MaxCount` is non-zero, a fixed-size `std::array` will be allocated.
/// - If `MaxCount` is zero, a `std::vector` will be used, allowing an unlimited
/// number of awaitables to be added.
///
/// `Awaitable` is automatically deduced from the argument.
template <size_t MaxCount = 0, typename Awaitable>
[[nodiscard("You must co_await spawn_group before it goes out of scope.")]]
aw_spawn_group<MaxCount, std::remove_cvref_t<Awaitable>>
spawn_group(Awaitable&& Aw) {
  return aw_spawn_group<MaxCount, std::remove_cvref_t<Awaitable>>(
    static_cast<Awaitable&&>(Aw)
  );
}

namespace detail {
template <size_t MaxCount, typename Awaitable>
struct awaitable_traits<aw_spawn_group<MaxCount, Awaitable>> {
  static constexpr configure_mode mode = WRAPPER;
  using Result = tmc::detail::awaitable_result_t<Awaitable>;
  using result_type = std::conditional_t<
    std::is_void_v<Result>, void,
    std::conditional_t<
      MaxCount == 0, std::vector<tmc::detail::result_storage_t<Result>>,
      std::array<tmc::detail::result_storage_t<Result>, MaxCount>>>;
  using self_type = aw_spawn_group<MaxCount, Awaitable>;
  using awaiter_type = aw_spawn_group<MaxCount, Awaitable>&&;

  static awaiter_type get_awaiter(self_type&& Aw) noexcept {
    return static_cast<self_type&&>(Aw);
  }
};
} // namespace detail
} // namespace tmc
