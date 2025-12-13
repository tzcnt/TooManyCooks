// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/aw_resume_on.hpp"
#include "tmc/detail/compat.hpp"
#include "tmc/detail/init_params.hpp"
#include "tmc/detail/qu_mpsc.hpp"
#include "tmc/detail/thread_locals.hpp"
#include "tmc/detail/tiny_vec.hpp"
#include "tmc/ex_any.hpp"
#include "tmc/topology.hpp"
#include "tmc/work_item.hpp"

#include <atomic>
#include <cassert>
#include <coroutine>
#include <functional>
#include <stop_token>
#include <thread>

namespace tmc {
/// A single-threaded executor.
/// Behaves the same as `ex_cpu` with `.set_thread_count(1)`, but with better
/// round-trip latency, and better internal execution performance, as it does
/// not need internal synchronization mechanisms.
class ex_cpu_st {
  struct qu_cfg : tmc::detail::qu_mpsc_default_config {
    static inline constexpr size_t BlockSize = 16384;
    static inline constexpr size_t PackingLevel = 1;
    // static inline constexpr bool EmbedFirstBlock = false;
  };
  enum class WorkerState : uint8_t { SLEEPING, WORKING, SPINNING };

  tmc::detail::InitParams* init_params; // accessed only during init()

  std::jthread worker_thread;
  tmc::ex_any type_erased_this;

  using task_queue_t = tmc::detail::qu_mpsc<work_item, qu_cfg>;
  tmc::detail::tiny_vec<task_queue_t> work_queues; // size() == PRIORITY_COUNT

  tmc::detail::tiny_vec<std::vector<work_item>>
    private_work; // size() == PRIORITY_COUNT
  // stop_source for the single worker thread
  std::stop_source thread_stopper;

  std::atomic<bool> initialized;
  std::atomic<WorkerState> thread_state;

  struct ThreadState {
    std::atomic<size_t> yield_priority; // check to yield to a higher prio task
    std::atomic<int> sleep_wait;        // futex waker for this thread
  };
  ThreadState thread_state_data;

  // capitalized variables are constant while ex_cpu_st is initialized & running
#ifdef TMC_PRIORITY_COUNT
  static constexpr size_t PRIORITY_COUNT = TMC_PRIORITY_COUNT;
  static constexpr size_t NO_TASK_RUNNING = TMC_PRIORITY_COUNT;
  // the maximum number of priority levels is 16
  static_assert(PRIORITY_COUNT <= 16);
#else
  size_t PRIORITY_COUNT;
  size_t NO_TASK_RUNNING;
#endif

  bool is_initialized();

  void clamp_priority(size_t& Priority);

  void notify_n(size_t Priority, bool FromExecThread);
  void init_thread_locals(size_t Slot);
  void clear_thread_locals();

  // Returns a lambda closure that is executed on a worker thread
  auto make_worker(
    size_t Slot, std::atomic<int>& InitThreadsBarrier,
    // actually a hwloc_topology_t
    // will be nullptr if hwloc is not enabled
    void* Topology,
    // actually a hwloc_bitmap_t
    // will be nullptr if hwloc is not enabled
    void* CpuSet
  );

  // returns true if no tasks were found (caller should wait on cv)
  // returns false if thread stop requested (caller should exit)
  bool try_run_some(std::stop_token& ThreadStopToken, size_t& PrevPriority);

  void run_one(
    tmc::work_item& Item, const size_t Prio, size_t& PrevPriority,
    bool& WasSpinning
  );

  void set_state(WorkerState NewState);
  WorkerState get_state();

  std::coroutine_handle<>
  task_enter_context(std::coroutine_handle<> Outer, size_t Priority);

  tmc::detail::InitParams* set_init_params();

  friend class aw_ex_scope_enter<ex_cpu_st>;
  friend tmc::detail::executor_traits<ex_cpu_st>;

  // not movable or copyable due to type_erased_this pointer being accessible by
  // child threads
  ex_cpu_st& operator=(const ex_cpu_st& Other) = delete;
  ex_cpu_st(const ex_cpu_st& Other) = delete;
  ex_cpu_st& operator=(ex_cpu_st&& Other) = delete;
  ex_cpu_st(ex_cpu_st&& Other) = delete;

public:
#ifdef TMC_USE_HWLOC
  ex_cpu_st& set_topology_filter(tmc::topology::TopologyFilter Filter);
#endif

#ifndef TMC_PRIORITY_COUNT
  /// Builder func to set the number of priority levels before calling `init()`.
  /// The value must be in the range [1, 16].
  /// The default is 1.
  ex_cpu_st& set_priority_count(size_t PriorityCount);
#endif

  /// Gets the number of priority levels. Only useful after `init()` has been
  /// called.
  size_t priority_count();

  /// Gets the number of worker threads. Always returns 1.
  size_t thread_count();

  /// Hook will be invoked at the startup of the executor thread, and passed
  /// the ordinal index of the thread (which is always 0, since this is a
  /// single-threaded executor).
  ex_cpu_st& set_thread_init_hook(std::function<void(size_t)> Hook);

  /// Hook will be invoked before destruction of each thread owned by this
  /// executor, and passed the ordinal index of the thread (which is always
  /// 0, since this is a single-threaded executor).
  ex_cpu_st& set_thread_teardown_hook(std::function<void(size_t)> Hook);

  /// Initializes the executor. If you want to customize the behavior, call the
  /// `set_X()` functions before calling `init()`.
  ///
  /// If the executor is already initialized, calling `init()` will do nothing.
  void init();

  /// Stops the executor, joins the worker thread, and destroys resources.
  /// Restores the executor to an uninitialized state. After calling
  /// `teardown()`, you may call `set_X()` to reconfigure the executor and call
  /// `init()` again.
  ///
  /// If the executor is not initialized, calling `teardown()` will do nothing.
  void teardown();

  /// After constructing, you must call `init()` before use.
  ex_cpu_st();

  /// Invokes `teardown()`.
  ~ex_cpu_st();

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
        notify_n(Priority, fromExecThread);
      } else {
        auto handle = work_queues[Priority].get_hazard_ptr();
        auto& haz = handle.value;
        work_queues[Priority].post_bulk(&haz, static_cast<It&&>(Items), Count);
        notify_n(Priority, fromExecThread);
        // Hold the handle until after notify_n() to prevent race
        // with destructor on another thread
        handle.release();
      }
    }
  }
};

namespace detail {
template <> struct executor_traits<tmc::ex_cpu_st> {
  static void post(
    tmc::ex_cpu_st& ex, tmc::work_item&& Item, size_t Priority,
    size_t ThreadHint
  );

  template <typename It>
  static inline void post_bulk(
    tmc::ex_cpu_st& ex, It&& Items, size_t Count, size_t Priority,
    size_t ThreadHint
  ) {
    ex.post_bulk(static_cast<It&&>(Items), Count, Priority, ThreadHint);
  }

  static tmc::ex_any* type_erased(tmc::ex_cpu_st& ex);

  static std::coroutine_handle<> task_enter_context(
    tmc::ex_cpu_st& ex, std::coroutine_handle<> Outer, size_t Priority
  );
};
} // namespace detail
} // namespace tmc

#ifdef TMC_IMPL
#include "tmc/detail/ex_cpu_st.ipp"
#endif
