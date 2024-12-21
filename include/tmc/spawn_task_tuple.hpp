// Copyright (c) 2023-2024 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/aw_run_early.hpp"
#include "tmc/detail/concepts.hpp" // IWYU pragma: keep
#include "tmc/detail/mixins.hpp"
#include "tmc/detail/thread_locals.hpp"
#include "tmc/task.hpp"

#include <cassert>
#include <coroutine>
#include <tuple>
#include <type_traits>
#include <variant>

namespace tmc {

template <typename T>
using void_to_monostate =
  std::conditional_t<std::is_void_v<T>, std::monostate, T>;

template <typename... Result> class aw_spawned_task_tuple_impl;

template <typename... Result> class aw_spawned_task_tuple_impl {
  std::tuple<task<Result>...> wrapped;
  detail::type_erased_executor* executor;
  detail::type_erased_executor* continuation_executor;
  size_t prio;
  std::atomic<uint64_t> sync_flags;
  int64_t remaining_count;
  std::tuple<void_to_monostate<Result>...> result;
  friend aw_spawned_task_tuple<Result...>;
  aw_spawned_task_tuple_impl(
    std::tuple<task<Result>...>&& Task, detail::type_erased_executor* Executor,
    detail::type_erased_executor* ContinuationExecutor, size_t Prio
  )
      : wrapped{std::move(Task)}, executor{Executor},
        continuation_executor{ContinuationExecutor}, prio{Prio} {
    // TODO submit in the constructor instead of await_suspend
  }

public:
  /// Always suspends.
  inline bool await_ready() const noexcept { return false; }

  template <typename T>
  TMC_FORCE_INLINE inline void
  submit_task(tmc::task<T> Task, T* TaskResult, std::coroutine_handle<> Outer) {
    auto& p = Task.promise();
    p.continuation = Outer.address();
    p.continuation_executor = continuation_executor;
    p.done_count = &sync_flags;
    p.result_ptr = TaskResult;
    // TODO collect tasks into a type-erased (work_item) array and bulk submit
    detail::post_checked(executor, std::move(Task), prio);
  }

  // template <size_t I>
  // TMC_FORCE_INLINE inline void submit_task(std::coroutine_handle<> Outer) {
  //   auto& task = std::get<I>(wrapped);
  //   auto& p = task.promise();
  //   p.continuation = Outer.address();
  //   p.continuation_executor = continuation_executor;
  //   p.done_count = &sync_flags;
  //   p.result_ptr = &std::get<I>(result);
  //   // TODO collect tasks into a type-erased (work_item) array and bulk
  //   submit detail::post_checked(executor, std::move(task), prio);
  // }

  /// Suspends the outer coroutine, submits the wrapped task to the
  /// executor, and waits for it to complete.
  TMC_FORCE_INLINE inline void await_suspend(std::coroutine_handle<> Outer
  ) noexcept {
    // std::apply(
    //   [this, Outer](auto&&... task) { ((submit_task(task, Outer)), ...); },
    //   wrapped
    // );
    // submit_task(
    //   std::make_index_sequence<std::tuple_size_v<std::tuple<task<Result>...>>>{}
    // );
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
      ((submit_task(std::get<Is>(wrapped), &std::get<Is>(result), Outer)), ...);
    }(std::make_index_sequence<std::tuple_size_v<std::tuple<task<Result>...>>>{}
    );
#ifndef TMC_TRIVIAL_TASK
    assert(wrapped);
#endif
  }

  /// Returns the value provided by the wrapped tasks.
  /// Each task has a slot in the tuple. If the task would return void, its
  /// slot is represented by a std::monostate.
  inline std::tuple<void_to_monostate<Result>&&...> await_resume() noexcept {
    return std::move(result);
  }
};

// Primary template is forward-declared in "tmc/detail/aw_run_early.hpp".
template <typename... Result>
class [[nodiscard(
  "You must use the aw_spawned_task_tuple<Result> by one of: 1. "
  "co_await 2. run_early()"
)]] aw_spawned_task_tuple
    : public detail::run_on_mixin<aw_spawned_task_tuple<Result...>>,
      public detail::resume_on_mixin<aw_spawned_task_tuple<Result...>>,
      public detail::with_priority_mixin<aw_spawned_task_tuple<Result...>> {
  friend class detail::run_on_mixin<aw_spawned_task_tuple<Result...>>;
  friend class detail::resume_on_mixin<aw_spawned_task_tuple<Result...>>;
  friend class detail::with_priority_mixin<aw_spawned_task_tuple<Result...>>;
  std::tuple<task<Result>...> wrapped;
  detail::type_erased_executor* executor;
  detail::type_erased_executor* continuation_executor;
  size_t prio;

public:
  /// It is recommended to call `spawn()` instead of using this constructor
  /// directly.
  aw_spawned_task_tuple(std::tuple<task<Result>...>&& Task)
      : wrapped(std::move(Task)), executor(detail::this_thread::executor),
        continuation_executor(detail::this_thread::executor),
        prio(detail::this_thread::this_task.prio) {}

  aw_spawned_task_tuple_impl<Result...> operator co_await() && {
    return aw_spawned_task_tuple_impl<Result...>(
      std::move(wrapped), executor, continuation_executor, prio
    );
  }

#if !defined(NDEBUG) && !defined(TMC_TRIVIAL_TASK)
  ~aw_spawned_task_tuple() noexcept {
    // If you spawn a task that returns a non-void type,
    // then you must co_await the return of spawn!
    // TODO fix this - probably need to check against get<0> instead
    assert(!wrapped);
  }
#endif
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

  /// Submits the wrapped task immediately, without suspending the current
  /// coroutine. You must await the return type before destroying it.
  // inline aw_run_early<Result> run_early() && {
  //   return aw_run_early<Result>(
  //     std::move(wrapped), executor, continuation_executor, prio
  //   );
  // }
};

/// `spawn()` allows you to customize the execution behavior of a task.
///
/// Before the task is submitted for execution, you may call any or all of
/// `run_on()`, `resume_on()`, `with_priority()`. The task must then be
/// submitted for execution by calling exactly one of: `co_await`, `run_early()`
/// or `detach()`.
// template <typename Result>
// aw_spawned_task_tuple<Result> spawn(task<Result>&& Task) {
//   return aw_spawned_task_tuple<Result>(std::move(Task));
// }

// TODO implement forwarding reference in the make_tuple call
// Tuple of forwarding references?
template <typename... Result>
aw_spawned_task_tuple<Result...> spawn_tuple(task<Result>&&... Args) {
  return aw_spawned_task_tuple<Result...>(std::make_tuple(Args...));
}

} // namespace tmc
