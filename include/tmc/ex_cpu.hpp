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
#include "tmc/task.hpp"
#include "tmc/work_item.hpp"

#include <atomic>
#include <cassert>
#include <functional>
#include <stop_token>
#include <thread>
#ifdef TMC_USE_HWLOC
#include <hwloc.h>
#endif

namespace tmc {
class ex_cpu {
  struct InitParams {
    size_t priority_count = 0;
    size_t thread_count = 0;
    float thread_occupancy = 1.0f;
    std::function<void(size_t)> thread_init_hook = nullptr;
    std::function<void(size_t)> thread_teardown_hook = nullptr;
  };
  struct alignas(64) ThreadState {
    std::atomic<size_t> yield_priority; // check to yield to a higher prio task
    std::atomic<int> sleep_wait;        // futex waker for this thread
    size_t group_size; // count of threads in this thread's group
    tmc::detail::qu_inbox<tmc::work_item, 4096>* inbox; // shared with group
  };
  using task_queue_t = tmc::queue::ConcurrentQueue<work_item>;
  // One inbox per thread group
  tmc::detail::tiny_vec<tmc::detail::qu_inbox<tmc::work_item, 4096>> inboxes;

  InitParams* init_params;                     // accessed only during init()
  tmc::detail::tiny_vec<std::jthread> threads; // size() == thread_count()
  tmc::ex_any type_erased_this;
  tmc::detail::tiny_vec<task_queue_t> work_queues; // size() == PRIORITY_COUNT
  // stop_sources that correspond to this pool's threads
  tmc::detail::tiny_vec<std::stop_source> thread_stoppers;

  std::atomic<bool> initialized;
  std::atomic<size_t> working_threads_bitset;
  std::atomic<size_t> spinning_threads_bitset;

  // TODO maybe shrink this by 1? prio 0 tasks cannot yield
  std::atomic<size_t>* task_stopper_bitsets; // array of size PRIORITY_COUNT

  ThreadState* thread_states; // array of size thread_count()
  std::vector<size_t> waker_matrix;

#ifdef TMC_USE_HWLOC
  tmc::detail::tiny_vec<size_t> pu_to_thread;
  hwloc_topology_t topology;
#endif

  // capitalized variables are constant while ex_cpu is initialized & running
#ifdef TMC_PRIORITY_COUNT
  static constexpr size_t PRIORITY_COUNT = TMC_PRIORITY_COUNT;
  static constexpr size_t NO_TASK_RUNNING = TMC_PRIORITY_COUNT;
#else
  size_t PRIORITY_COUNT;
  size_t NO_TASK_RUNNING;
#endif

  bool is_initialized();

  void clamp_priority(size_t& Priority);

  void notify_n(
    size_t Count, size_t Priority, size_t ThreadHint, bool FromExecThread,
    bool FromPost
  );
  void init_thread_locals(size_t Slot);
  void init_queue_iteration_order(std::vector<size_t> const& Forward);
  void clear_thread_locals();

  // returns true if no tasks were found (caller should wait on cv)
  // returns false if thread stop requested (caller should exit)
  bool try_run_some(
    std::stop_token& ThreadStopToken, const size_t Slot,
    const size_t MinPriority, size_t& PreviousPrio
  );
  size_t set_spin(size_t Slot);
  size_t clr_spin(size_t Slot);
  size_t set_work(size_t Slot);
  size_t clr_work(size_t Slot);

  std::coroutine_handle<>
  task_enter_context(std::coroutine_handle<> Outer, size_t Priority);

  size_t* wake_nearby_thread_order(size_t ThreadIdx);

  friend class aw_ex_scope_enter<ex_cpu>;
  friend tmc::detail::executor_traits<ex_cpu>;

  // not movable or copyable due to type_erased_this pointer being accessible by
  // child threads
  ex_cpu& operator=(const ex_cpu& Other) = delete;
  ex_cpu(const ex_cpu& Other) = delete;
  ex_cpu& operator=(ex_cpu&& Other) = delete;
  ex_cpu(ex_cpu&& Other) = delete;

public:
  /// Builder func to set the number of threads before calling `init()`.
  /// The default is 0, which will cause `init()` to automatically create 1
  /// thread per physical core (with hwloc), or create
  /// std::thread::hardware_concurrency() threads (without hwloc).
  ex_cpu& set_thread_count(size_t ThreadCount);
#ifdef TMC_USE_HWLOC
  /// Builder func to set the number of threads per core before calling
  /// `init()`. Requires TMC_USE_HWLOC. The default is 1.0f, which will cause
  /// `init()` to automatically create threads equal to the number of physical
  /// cores. If you want full SMT, set it to 2.0. Smaller increments (1.5, 1.75)
  /// are also valid to increase thread occupancy without full saturation.
  ex_cpu& set_thread_occupancy(float ThreadOccupancy);
#endif
#ifndef TMC_PRIORITY_COUNT
  /// Builder func to set the number of priority levels before calling `init()`.
  /// The default is 1.
  ex_cpu& set_priority_count(size_t PriorityCount);
#endif
  /// Gets the number of worker threads. Only useful after `init()` has been
  /// called.
  size_t thread_count();

  /// Gets the number of priority levels. Only useful after `init()` has been
  /// called.
  size_t priority_count();

  /// Hook will be invoked at the startup of each thread owned by this executor,
  /// and passed the ordinal index (0..thread_count()-1) of the thread.
  ex_cpu& set_thread_init_hook(std::function<void(size_t)> Hook);

  /// Hook will be invoked before destruction of each thread owned by this
  /// executor, and passed the ordinal index (0..thread_count()-1) of the
  /// thread.
  ex_cpu& set_thread_teardown_hook(std::function<void(size_t)> Hook);

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
  ex_cpu();

  /// Invokes `teardown()`.
  ~ex_cpu();

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
    if (ThreadHint < thread_count()) {
      size_t enqueuedCount = thread_states[ThreadHint].inbox->try_push_bulk(
        static_cast<It&&>(Items), Count
      );
      if (enqueuedCount != 0) {
        Count -= enqueuedCount;
        if (!fromExecThread || ThreadHint != tmc::current_thread_index()) {
          notify_n(1, Priority, ThreadHint, fromExecThread, true);
        }
      }
    }
    if (Count > 0) {
      if (fromExecThread) {
        work_queues[Priority].enqueue_bulk_ex_cpu(
          static_cast<It&&>(Items), Count, Priority
        );
      } else {
        work_queues[Priority].enqueue_bulk(static_cast<It&&>(Items), Count);
      }
      notify_n(Count, Priority, NO_HINT, fromExecThread, true);
    }
  }
};

namespace detail {
template <> struct executor_traits<tmc::ex_cpu> {
  static void post(
    tmc::ex_cpu& ex, tmc::work_item&& Item, size_t Priority, size_t ThreadHint
  );

  template <typename It>
  static inline void post_bulk(
    tmc::ex_cpu& ex, It&& Items, size_t Count, size_t Priority,
    size_t ThreadHint
  ) {
    ex.post_bulk(std::forward<It>(Items), Count, Priority, ThreadHint);
  }

  static tmc::ex_any* type_erased(tmc::ex_cpu& ex);

  static std::coroutine_handle<> task_enter_context(
    tmc::ex_cpu& ex, std::coroutine_handle<> Outer, size_t Priority
  );
};

inline ex_cpu g_ex_cpu;
} // namespace detail

/// Returns a reference to the global instance of `tmc::ex_cpu`.
constexpr ex_cpu& cpu_executor() { return tmc::detail::g_ex_cpu; }
namespace detail {
tmc::task<void> client_main_awaiter(
  tmc::task<int> ClientMainTask, std::atomic<int>* ExitCode_out
);
}

/// A convenience function that initializes `tmc::cpu_executor()`, submits the
/// ClientMainTask parameter to `tmc::cpu_executor()`, and then waits for it to
/// complete. The int value returned by the submitted task will be returned from
/// this function, so that you can use it as an exit code.
int async_main(tmc::task<int>&& ClientMainTask);
} // namespace tmc

#ifdef TMC_IMPL
#include "tmc/detail/ex_cpu.ipp"
#endif
