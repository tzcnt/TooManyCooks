// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once
#include "tmc/detail/impl.hpp" // IWYU pragma: keep

#include "tmc/aw_resume_on.hpp"
#include "tmc/detail/compat.hpp"
#include "tmc/detail/concepts_awaitable.hpp"
#include "tmc/ex_any.hpp"
#include "tmc/qu_mpsc_unbounded.hpp"
#include "tmc/task.hpp"
#include "tmc/utils.hpp"
#include "tmc/work_item.hpp"

#include <cassert>
#include <coroutine>
#include <memory>

namespace tmc {
namespace detail {
struct braid_work_item {
  work_item item;
  size_t prio;
};
struct braid_qu_config : tmc::qu_mpsc_unbounded_default_config {
  static inline constexpr size_t BlockSize = 8192;
  static inline constexpr size_t PackingLevel = 1;
  static inline constexpr bool EmbedFirstBlock = false;
  static inline constexpr bool ConsumerCanSuspend = true;
};
} // namespace detail

/// A serializing executor. `ex_braid` does not own any threads; rather, a
/// single thread from its parent executor will execute tasks. Which thread is
/// currently executing on the braid may change, but only 1 thread is allowed to
/// enter the braid at a time.
///
/// It's similar in function to `tmc::mutex`, but with different
/// characteristics: `ex_braid` is an executor, so child tasks that are
/// created within a braid will be bound to it also.
///
/// Additionally, while a `tmc::mutex` can be held across a suspension point,
/// this will not. If a task suspends or switches to another executor while
/// running on a braid, another task may enter the braid and begin executing.
class ex_braid {
  friend class aw_ex_scope_enter<ex_braid>;
  friend tmc::detail::executor_traits<ex_braid>;

  using task_queue_t = tmc::qu_mpsc_unbounded<
    tmc::detail::braid_work_item, tmc::detail::braid_qu_config>;

  std::shared_ptr<task_queue_t> queue;

  tmc::ex_any type_erased_this;

  /// The main loop of the braid; only 1 thread is allowed to enter the inner
  /// loop. If the lock is already taken, other threads will return immediately.
  TMC_DECL tmc::task<void> run_loop(std::shared_ptr<task_queue_t> Queue);

  TMC_DECL std::coroutine_handle<>
  dispatch(std::coroutine_handle<> Outer, size_t Priority);

public:
  /// Submits a single work_item to the braid, and attempts to take the lock and
  /// start executing tasks on the braid.
  ///
  /// Rather than calling this directly, it is recommended to use the
  /// `tmc::post()` free function template.
  TMC_DECL void
  post(work_item&& Item, size_t Priority = 0, size_t ThreadHint = NO_HINT);

  /// Submits `count` items to the braid, and attempts to take the lock and
  /// start executing tasks on the braid. `It` must be an iterator
  /// type that implements `operator*()` and `It& operator++()`.
  ///
  /// Rather than calling this directly, it is recommended to use the
  /// `tmc::post_bulk()` free function template.
  template <typename It>
  void post_bulk(
    It&& Items, size_t Count, size_t Priority = 0,
    [[maybe_unused]] size_t ThreadHint = NO_HINT
  ) {
    queue->post_bulk(
      tmc::iter_adapter(
        std::forward<It>(Items),
        [Priority](auto Item) -> tmc::detail::braid_work_item {
#ifndef NDEBUG
          auto item = tmc::detail::braid_work_item{std::move(*Item), Priority};
          return item;
#else
          return tmc::detail::braid_work_item{std::move(*Item), Priority};
#endif
        }
      ),
      Count
    );
  }

  /// Returns a pointer to the type erased `ex_any` version of this executor.
  /// This object shares a lifetime with this executor, and can be used for
  /// pointer-based equality comparison against
  /// the thread-local `tmc::current_executor()`.
  inline tmc::ex_any* type_erased() { return &type_erased_this; }

private:
  TMC_DECL ex_braid(tmc::ex_any* Parent);

public:
  /// Construct a braid with the current executor as its parent. It is an error
  /// to call this from a thread that is not running on an executor.
  TMC_DECL ex_braid();

  /// Construct a braid with the specified executor as its parent.
  template <typename Executor>
  ex_braid(Executor& Parent)
      : ex_braid(
          tmc::detail::get_executor_traits<Executor>::type_erased(Parent)
        ) {}

  /// You must ensure that all tasks running on the braid have completed before
  /// destroying it.
  TMC_DECL ~ex_braid();

private:
  ex_braid& operator=(const ex_braid& Other) = delete;
  ex_braid(const ex_braid& Other) = delete;
  // not movable due to this_executor pointer being accessible by child threads
  ex_braid& operator=(const ex_braid&& Other) = delete;
  ex_braid(const ex_braid&& Other) = delete;
};

// I would LOVE to implement a scoped_lock but until we get async destructor
// that won't happen See https://wg21.link/p1662/status
// struct scoped_braid_lock
// {
//   ex_braid &b;
//   scoped_braid_lock(ex_braid &Braid) : b(Braid) {}
//   ~scoped_braid_lock() {
//     co_await b.exit();
//   }
// };

namespace detail {
template <> struct executor_traits<tmc::ex_braid> {
  static TMC_DECL void post(
    tmc::ex_braid& ex, tmc::work_item&& Item, size_t Priority, size_t ThreadHint
  );

  template <typename It>
  static inline void post_bulk(
    tmc::ex_braid& ex, It&& Items, size_t Count, size_t Priority,
    size_t ThreadHint
  ) {
    ex.post_bulk(std::forward<It>(Items), Count, Priority, ThreadHint);
  }

  static TMC_DECL tmc::ex_any* type_erased(tmc::ex_braid& ex);

  static TMC_DECL std::coroutine_handle<>
  dispatch(tmc::ex_braid& ex, std::coroutine_handle<> Outer, size_t Priority);
};
} // namespace detail
} // namespace tmc

#if !defined(TMC_STANDALONE_COMPILATION) || defined(TMC_IMPL)
#include "tmc/detail/ex_braid.ipp"
#endif
