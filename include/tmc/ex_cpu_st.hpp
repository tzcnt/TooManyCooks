// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/aw_resume_on.hpp"
#include "tmc/current.hpp"
#include "tmc/detail/compat.hpp"
#include "tmc/detail/qu_inbox.hpp"
#include "tmc/detail/qu_lockfree.hpp"
#include "tmc/detail/thread_locals.hpp"
#include "tmc/detail/tiny_vec.hpp"
#include "tmc/ex_any.hpp"
#include "tmc/work_item.hpp"

#include <atomic>
#include <cassert>
#include <coroutine>
#include <functional>
#include <stop_token>
#include <thread>

namespace tmc {
class ex_cpu_st {
  enum class WorkerState : uint8_t { SLEEPING, WORKING, SPINNING };

  struct InitParams {
    size_t priority_count = 0;
    std::function<void(size_t)> thread_init_hook = nullptr;
    std::function<void(size_t)> thread_teardown_hook = nullptr;
  };
  struct alignas(64) ThreadState {
    std::atomic<size_t> yield_priority; // check to yield to a higher prio task
    std::atomic<int> sleep_wait;        // futex waker for this thread
    tmc::detail::qu_inbox<tmc::work_item, 4096>* inbox; // shared with group
  };
  using task_queue_t = tmc::queue::ConcurrentQueue<work_item>;
  tmc::detail::qu_inbox<tmc::work_item, 4096> inbox;

  InitParams* init_params; // accessed only during init()
  std::jthread worker_thread;
  tmc::ex_any type_erased_this;
  tmc::detail::tiny_vec<task_queue_t> work_queues; // size() == PRIORITY_COUNT
  // stop_source for the single worker thread
  std::stop_source thread_stopper;

  std::atomic<bool> initialized;
  std::atomic<WorkerState> thread_state;

  // TODO maybe shrink this by 1? prio 0 tasks cannot yield
  std::atomic<size_t>* task_stopper_bitsets; // array of size PRIORITY_COUNT

  ThreadState thread_state_data;

  // ref_count prevents a race condition between post() which resumes a task
  // that completes and destroys the ex_cpu_st before the post() call completes
  // - after the enqueue, before the notify_n step. This can only happen when
  // post() is called by non-executor threads; if an executor thread is still
  // running, the join() call in the destructor will block until it completes.
  std::atomic<size_t> ref_count;

#ifdef TMC_USE_HWLOC
  void* topology; // actually a hwloc_topology_t
#endif

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

  void notify_n(size_t Priority);
  void init_thread_locals(size_t Slot);
  void init_queue_iteration_order();
  void clear_thread_locals();

  // Returns a lambda closure that is executed on a worker thread
  auto make_worker(
    size_t Slot, std::atomic<int>& InitThreadsBarrier,
    // actually a hwloc_bitmap_t
    // will be nullptr if hwloc is not enabled
    void* CpuSet
  );

  // returns true if no tasks were found (caller should wait on cv)
  // returns false if thread stop requested (caller should exit)
  bool try_run_some(
    std::stop_token& ThreadStopToken, const size_t Slot, size_t& PrevPriority
  );

  void run_one(
    tmc::work_item& Item, const size_t Slot, const size_t Prio,
    size_t& PrevPriority, bool& WasSpinning
  );

  void set_state(WorkerState NewState);
  WorkerState get_state();

  std::coroutine_handle<>
  task_enter_context(std::coroutine_handle<> Outer, size_t Priority);

  friend class aw_ex_scope_enter<ex_cpu_st>;
  friend tmc::detail::executor_traits<ex_cpu_st>;

  // not movable or copyable due to type_erased_this pointer being accessible by
  // child threads
  ex_cpu_st& operator=(const ex_cpu_st& Other) = delete;
  ex_cpu_st(const ex_cpu_st& Other) = delete;
  ex_cpu_st& operator=(ex_cpu_st&& Other) = delete;
  ex_cpu_st(ex_cpu_st&& Other) = delete;

public:
#ifndef TMC_PRIORITY_COUNT
  /// Builder func to set the number of priority levels before calling `init()`.
  /// The value must be in the range [1, 16].
  /// The default is 1.
  ex_cpu_st& set_priority_count(size_t PriorityCount);
#endif
  /// Gets the number of worker threads. Only useful after `init()` has been
  /// called.
  size_t thread_count();

  /// Gets the number of priority levels. Only useful after `init()` has been
  /// called.
  size_t priority_count();

  /// Hook will be invoked at the startup of each thread owned by this executor,
  /// and passed the ordinal index (0..thread_count()-1) of the thread.
  ex_cpu_st& set_thread_init_hook(std::function<void(size_t)> Hook);

  /// Hook will be invoked before destruction of each thread owned by this
  /// executor, and passed the ordinal index (0..thread_count()-1) of the
  /// thread.
  ex_cpu_st& set_thread_teardown_hook(std::function<void(size_t)> Hook);

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
  /// pointer-based equality comparison against the thread-local
  /// `tmc::current_executor()`.
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
    if (!fromExecThread) {
      ++ref_count;
    }
    if (ThreadHint == 0) [[unlikely]] {
      size_t enqueuedCount = thread_state_data.inbox->try_push_bulk(
        static_cast<It&&>(Items), Count, Priority
      );
      if (enqueuedCount != 0) {
        Count -= enqueuedCount;
        if (!fromExecThread) {
          notify_n(Priority);
        }
      }
    }
    if (Count > 0) [[likely]] {
      if (fromExecThread) [[likely]] {
        work_queues[Priority].enqueue_bulk_ex_cpu(
          static_cast<It&&>(Items), Count, Priority
        );
      } else {
        work_queues[Priority].enqueue_bulk(static_cast<It&&>(Items), Count);
      }
      notify_n(Priority);
    }
    if (!fromExecThread) {
      --ref_count;
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
