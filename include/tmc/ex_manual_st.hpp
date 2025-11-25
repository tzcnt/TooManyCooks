// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/aw_resume_on.hpp"
#include "tmc/detail/compat.hpp"
#include "tmc/detail/qu_mpsc.hpp"
#include "tmc/detail/thread_locals.hpp"
#include "tmc/detail/tiny_vec.hpp"
#include "tmc/ex_any.hpp"
#include "tmc/work_item.hpp"

#include <atomic>
#include <cassert>
#include <coroutine>
#include <vector>

namespace tmc {
/// An executor that does not own any threads. Work can be posted to this
/// executor at any time, but it will only be executed when you call one of
/// the `run_*()` functions. The caller of `run_*()` will execute work inline on
/// the current thread.
///
/// It is safe to post work from any number of threads concurrently, but
/// `run_*()` must only be called from 1 thread at a time.
class ex_manual_st {
  struct qu_cfg : tmc::detail::qu_mpsc_default_config {
    static inline constexpr size_t BlockSize = 16384;
    static inline constexpr size_t PackingLevel = 1;
    // static inline constexpr bool EmbedFirstBlock = false;
  };

  struct InitParams {
    size_t priority_count = 0;
  };
  InitParams* init_params; // accessed only during init()

  tmc::ex_any type_erased_this;

  using task_queue_t = tmc::detail::qu_mpsc<work_item, qu_cfg>;
  tmc::detail::tiny_vec<task_queue_t> work_queues; // size() == PRIORITY_COUNT

  tmc::detail::tiny_vec<std::vector<work_item>>
    private_work; // size() == PRIORITY_COUNT

  std::atomic<bool> initialized;
  std::atomic<size_t> yield_priority; // check to yield to a higher prio task

#ifdef TMC_USE_HWLOC
  void* topology; // actually a hwloc_topology_t
#endif

  // capitalized variables are constant while ex_manual_st is initialized &
  // running
#ifdef TMC_PRIORITY_COUNT
  static constexpr size_t PRIORITY_COUNT = TMC_PRIORITY_COUNT;
  // the maximum number of priority levels is 16
  static_assert(PRIORITY_COUNT <= 16);
#else
  size_t PRIORITY_COUNT;
#endif

  bool is_initialized();

  void clamp_priority(size_t& Priority);

  std::coroutine_handle<>
  task_enter_context(std::coroutine_handle<> Outer, size_t Priority);

  void notify_n(size_t Priority);

  bool try_get_work(work_item& Item, size_t& Prio);

  friend class aw_ex_scope_enter<ex_manual_st>;
  friend tmc::detail::executor_traits<ex_manual_st>;

  // not movable or copyable due to type_erased_this pointer being accessible by
  // other threads
  ex_manual_st& operator=(const ex_manual_st& Other) = delete;
  ex_manual_st(const ex_manual_st& Other) = delete;
  ex_manual_st& operator=(ex_manual_st&& Other) = delete;
  ex_manual_st(ex_manual_st&& Other) = delete;

public:
  /// Attempt to run 1 work item from the executor's queue. Work items will be
  /// executed on the current thread. Returns true if any work was waiting, and
  /// 1 work item was executed. Returns false if the executor's queue was empty.
  bool run_one();

  /// Run all work items from the executor's queue. Work items will be
  /// executed on the current thread. Returns the number of work items that were
  /// executed (0 if it was empty).
  ///
  /// The returned count may be larger than the number of work items originally
  /// posted, because awaitables may resume suspended tasks by posting them back
  /// to the executor queue.
  size_t run_all();

  /// Run up to MaxCount work items from the executor's queue. Work items will
  /// be executed on the current thread. Returns the number of work items that
  /// were executed (0 if it was empty). MaxCount must be non-zero.
  ///
  /// The returned count may be larger than the number of work items originally
  /// posted, because awaitables may resume suspended tasks by posting them back
  /// to the executor queue.
  size_t run_n(const size_t MaxCount);

  /// Returns true if the executor's queue appears to be empty at the current
  /// moment.
  bool empty();

#ifndef TMC_PRIORITY_COUNT
  /// Builder func to set the number of priority levels before calling `init()`.
  /// The value must be in the range [1, 16].
  /// The default is 1.
  ex_manual_st& set_priority_count(size_t PriorityCount);
#endif

  /// Gets the number of priority levels. Only useful after `init()` has been
  /// called.
  size_t priority_count();

  /// Initializes the executor. If you want to customize the behavior, call the
  /// `set_X()` functions before calling `init()`. By default, uses hwloc to
  /// automatically generate threads, and creates 1 (or TMC_PRIORITY_COUNT)
  /// priority levels.
  ///
  /// If the executor is already initialized, calling `init()` will do nothing.
  void init();

  /// Stops the executor, joins the worker threads, and destroys resources.
  /// Restores the executor to an uninitialized state. After calling
  /// `teardown()`, you may call `set_X()` to reconfigure the executor and call
  /// `init()` again.
  ///
  /// If the executor is not initialized, calling `teardown()` will do nothing.
  void teardown();

  /// After constructing, you must call `init()` before use.
  ex_manual_st();

  /// Invokes `teardown()`.
  ~ex_manual_st();

  /// Submits a single work_item to the executor.
  ///
  /// Rather than calling this directly, it is recommended to use the
  /// `tmc::post()` free function template.
  void post(work_item&& Item, size_t Priority = 0, size_t ThreadHint = NO_HINT);

  /// Returns a pointer to the type erased `ex_any` version of this executor.
  /// This object shares a lifetime with this executor, and can be used for
  /// pointer-based equality comparison against
  /// the thread-local `tmc::current_executor()`.
  tmc::ex_any* type_erased();

  /// Submits `count` items to the executor. `It` is expected to be an iterator
  /// type that implements `operator*()` and `It& operator++()`.
  ///
  /// Rather than calling this directly, it is recommended to use the
  /// `tmc::post_bulk()` free function template.
  template <typename It>
  void post_bulk(
    It&& Items, size_t Count, size_t Priority = 0, size_t ThreadHint = NO_HINT
  ) {
    clamp_priority(Priority);
    bool fromExecThread =
      tmc::detail::this_thread::executor == &type_erased_this;
    if (Count > 0) [[likely]] {
      if (fromExecThread && ThreadHint != 0) [[likely]] {
        for (size_t i = 0; i < Count; ++i) {
          private_work[Priority].push_back(std::move(*Items));
          ++Items;
        }
        notify_n(Priority);
      } else {
        auto handle = work_queues[Priority].get_hazard_ptr();
        auto& haz = handle.value;
        work_queues[Priority].post_bulk(&haz, static_cast<It&&>(Items), Count);
        notify_n(Priority);
        // Hold the handle until after notify_n() to prevent race
        // with destructor on another thread
        handle.release();
      }
    }
  }
};

namespace detail {
template <> struct executor_traits<tmc::ex_manual_st> {
  static void post(
    tmc::ex_manual_st& ex, tmc::work_item&& Item, size_t Priority,
    size_t ThreadHint
  );

  template <typename It>
  static inline void post_bulk(
    tmc::ex_manual_st& ex, It&& Items, size_t Count, size_t Priority,
    size_t ThreadHint
  ) {
    ex.post_bulk(static_cast<It&&>(Items), Count, Priority, ThreadHint);
  }

  static tmc::ex_any* type_erased(tmc::ex_manual_st& ex);

  static std::coroutine_handle<> task_enter_context(
    tmc::ex_manual_st& ex, std::coroutine_handle<> Outer, size_t Priority
  );
};
} // namespace detail
} // namespace tmc

#ifdef TMC_IMPL
#include "tmc/detail/ex_manual_st.ipp"
#endif
