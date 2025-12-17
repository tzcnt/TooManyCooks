// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/aw_resume_on.hpp"
#include "tmc/current.hpp"
#include "tmc/detail/compat.hpp"
#include "tmc/detail/hwloc_unique_bitmap.hpp"
#include "tmc/detail/init_params.hpp"
#include "tmc/detail/qu_inbox.hpp"
#include "tmc/detail/qu_lockfree.hpp"
#include "tmc/detail/thread_locals.hpp"
#include "tmc/detail/tiny_vec.hpp"
#include "tmc/ex_any.hpp"
#include "tmc/task.hpp"
#include "tmc/topology.hpp"
#include "tmc/work_item.hpp"

#include <atomic>
#include <cassert>
#include <coroutine>
#include <functional>
#include <stop_token>
#include <thread>
#include <vector>

namespace tmc {
/// The default multi-threaded executor of TooManyCooks.
class ex_cpu {
private:
  struct alignas(64) ThreadState {
    std::atomic<size_t> yield_priority; // check to yield to a higher prio task
    std::atomic<int> sleep_wait;        // futex waker for this thread
    size_t group_size; // count of threads in this thread's group
    tmc::detail::qu_inbox<tmc::work_item, 4096>* inbox; // shared with group
    TMC_DISABLE_WARNING_PADDED_BEGIN
  };
  TMC_DISABLE_WARNING_PADDED_END
  using task_queue_t = tmc::queue::ConcurrentQueue<work_item>;
  // One inbox per thread group
  tmc::detail::tiny_vec<tmc::detail::qu_inbox<tmc::work_item, 4096>> inboxes;

  tmc::detail::InitParams* init_params;        // accessed only during init()
  tmc::detail::tiny_vec<std::jthread> threads; // size() == thread_count()
  tmc::ex_any type_erased_this;
  tmc::detail::tiny_vec<task_queue_t> work_queues; // size() == PRIORITY_COUNT
  // stop_sources that correspond to this pool's threads
  tmc::detail::tiny_vec<std::stop_source> thread_stoppers;
  size_t spins;

  std::atomic<bool> initialized;

  // TODO experiment with moving these the end and add padding before and after
  // to prevent false sharing
  std::atomic<size_t> working_threads_bitset;
  std::atomic<size_t> spinning_threads_bitset;

  // TODO maybe shrink this by 1? prio 0 tasks cannot yield
  std::atomic<size_t>* task_stopper_bitsets; // array of size PRIORITY_COUNT

  ThreadState* thread_states; // array of size thread_count()

  // ref_count prevents a race condition between post() which resumes a task
  // that completes and destroys the ex_cpu before the post() call completes -
  // after the enqueue, before the notify_n step. This can only happen when
  // post() is called by non-executor threads; if an executor thread is still
  // running, the join() call in the destructor will block until it completes.
  std::atomic<size_t> ref_count;

  std::vector<size_t> waker_matrix;

#ifdef TMC_USE_HWLOC
  tmc::detail::tiny_vec<size_t> pu_to_thread;
  void* topology; // actually a hwloc_topology_t
#endif

  // capitalized variables are constant while ex_cpu is initialized & running
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

  void notify_n(
    size_t Count, size_t Priority, size_t ThreadHint, bool FromExecThread,
    bool FromPost
  );

  void init_thread_locals(size_t Slot);
  task_queue_t::ExplicitProducer**
  init_queue_iteration_order(std::vector<std::vector<size_t>> const& Forward);
  void clear_thread_locals();

  // Returns a lambda closure that is executed on a worker thread
  auto make_worker(
    size_t Slot, size_t PriorityRangeBegin, size_t PriorityRangeEnd,
    ex_cpu::task_queue_t::ExplicitProducer** StealOrder,
    std::atomic<int>& InitThreadsBarrier,
    // will be nullptr if hwloc is not enabled
    tmc::detail::hwloc_unique_bitmap& CpuSet
  );

  // returns true if no tasks were found (caller should wait on cv)
  // returns false if thread stop requested (caller should exit)
  bool try_run_some(
    std::stop_token& ThreadStopToken, const size_t Slot,
    const size_t PriorityRangeBegin, const size_t PriorityRangeEnd,
    size_t& PrevPriority
  );

  void run_one(
    tmc::work_item& Item, const size_t Slot, const size_t Prio,
    size_t& PrevPriority, bool& WasSpinning
  );

  size_t set_spin(size_t Slot);
  size_t clr_spin(size_t Slot);
  size_t set_work(size_t Slot);
  size_t clr_work(size_t Slot);

  std::coroutine_handle<>
  task_enter_context(std::coroutine_handle<> Outer, size_t Priority);

  size_t* wake_nearby_thread_order(size_t ThreadIdx);

  tmc::detail::InitParams* set_init_params();

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
  /// The maximum number of threads is equal to the number of bits on your
  /// platform (32 or 64 bit). The default is 0, which will cause `init()` to
  /// automatically create 1 thread per physical core (with hwloc), or create
  /// std::thread::hardware_concurrency() threads (without hwloc).
  ex_cpu& set_thread_count(size_t ThreadCount);
#ifdef TMC_USE_HWLOC
  /// Builder func to set the number of threads per core before calling
  /// `init()`. Requires TMC_USE_HWLOC. The default is 1.0f, which will cause
  /// `init()` to automatically create threads equal to the number of physical
  /// cores. If you want full SMT, set it to 2.0. Smaller increments (1.5, 1.75)
  /// are also valid to increase thread occupancy without full saturation.
  ex_cpu& set_thread_occupancy(
    float ThreadOccupancy,
    tmc::topology::CpuKind::value CpuKinds = tmc::topology::CpuKind::PERFORMANCE
  );

  ex_cpu& add_partition(
    tmc::topology::TopologyFilter Filter, size_t PriorityRangeBegin = 0,
    size_t PriorityRangeEnd = TMC_MAX_PRIORITY_COUNT
  );

  ex_cpu& set_thread_pinning_level(tmc::topology::ThreadPinningLevel Level);

  ex_cpu&
  set_thread_packing_strategy(tmc::topology::ThreadPackingStrategy Strategy);
#endif

#ifndef TMC_PRIORITY_COUNT
  /// Builder func to set the number of priority levels before calling `init()`.
  /// The value must be in the range [1, 16].
  /// The default is 1.
  ex_cpu& set_priority_count(size_t PriorityCount);
#endif
  /// Gets the number of worker threads. Only useful after `init()` has been
  /// called.
  size_t thread_count();

  /// Gets the number of priority levels. Only useful after `init()` has been
  /// called.
  size_t priority_count();

  /// Builder func to set a hook that will be invoked at the startup of each
  /// thread owned by this executor, and passed the ordinal index
  /// (0..thread_count()-1) of the thread.
  ex_cpu& set_thread_init_hook(std::function<void(size_t)> Hook);

  /// Builder func to set a hook that will be invoked before destruction of each
  /// thread owned by this executor, and passed the ordinal index
  /// (0..thread_count()-1) of the thread.
  ex_cpu& set_thread_teardown_hook(std::function<void(size_t)> Hook);

  /// Builder func to set the number of times that a thread worker will spin
  /// wait looking for new work when all queues appear to be empty before
  /// suspending the thread.  Each spin wait is an asm("pause") followed by
  /// re-checking all queues.
  /// The default is 4.
  ex_cpu& set_spins(size_t Spins);

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
    if (!fromExecThread) {
      ++ref_count;
    }
    if (ThreadHint < thread_count()) [[unlikely]] {
      size_t enqueuedCount = thread_states[ThreadHint].inbox->try_push_bulk(
        static_cast<It&&>(Items), Count, Priority
      );
      if (enqueuedCount != 0) {
        Count -= enqueuedCount;
        if (!fromExecThread || ThreadHint != tmc::current_thread_index()) {
          notify_n(1, Priority, ThreadHint, fromExecThread, true);
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
      notify_n(Count, Priority, NO_HINT, fromExecThread, true);
    }
    if (!fromExecThread) {
      --ref_count;
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
    ex.post_bulk(static_cast<It&&>(Items), Count, Priority, ThreadHint);
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
