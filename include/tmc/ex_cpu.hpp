// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/impl.hpp" // IWYU pragma: keep

#include "tmc/aw_resume_on.hpp"
#include "tmc/current.hpp"
#include "tmc/detail/atomic_bitmap.hpp"
#include "tmc/detail/compat.hpp"
#include "tmc/detail/hwloc_unique_bitmap.hpp"
#include "tmc/detail/init_params.hpp"
#include "tmc/detail/matrix.hpp"
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
  struct alignas(TMC_CACHE_LINE_SIZE) ThreadState {
    std::atomic<size_t> yield_priority; // check to yield to a higher prio task
    std::atomic<tmc::detail::atomic_waker_t>
      sleep_wait; // futex waker for this thread
    tmc::detail::qu_inbox<tmc::work_item, 4096>* inbox; // shared with group
    size_t group_size;
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

  std::vector<tmc::detail::bitmap> threads_by_priority_bitset;

  // TODO maybe shrink this by 1? prio 0 tasks cannot yield
  tmc::detail::atomic_bitmap*
    task_stopper_bitsets; // array of size PRIORITY_COUNT

  ThreadState* thread_states; // array of size thread_count()

  std::vector<tmc::detail::Matrix> waker_matrix;

  // Manually pad out atomics that are part of this struct. We can't use alignas
  // here due to the possibility to construct an ex_cpu inside of a task. Tasks
  // don't support overaligned allocation by default without a compiler flag,
  // which would be a footgun.
  char pad0[TMC_CACHE_LINE_SIZE - sizeof(size_t)];
  tmc::detail::atomic_bitmap working_threads_bitset;
  tmc::detail::atomic_bitmap spinning_threads_bitset;
  char pad1[TMC_CACHE_LINE_SIZE - 2 * sizeof(size_t)];
  // ref_count prevents a race condition between post() which resumes a task
  // that completes and destroys the ex_cpu before the post() call completes -
  // after the enqueue, before the notify_n step. This can only happen when
  // post() is called by non-executor threads; if an executor thread is still
  // running, the join() call in the destructor will block until it completes.
  std::atomic<size_t> ref_count;
  char pad2[TMC_CACHE_LINE_SIZE - sizeof(size_t)];

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

  TMC_DECL_HWLOC bool is_initialized();

  TMC_DECL_HWLOC void clamp_priority(size_t& Priority);

  TMC_DECL_HWLOC void
  notify_n(size_t Count, size_t Priority, bool AllowedPriority, bool FromPost);

  TMC_DECL_HWLOC void notify_hint(size_t Priority, size_t ThreadHint);

  TMC_DECL_HWLOC void init_thread_locals(size_t Slot);
  TMC_DECL_HWLOC task_queue_t::ExplicitProducer**
  init_queue_iteration_order(std::vector<std::vector<size_t>> const& Forward);
  TMC_DECL_HWLOC void clear_thread_locals();

  // Returns a lambda closure that is executed on a worker thread
  TMC_DECL_HWLOC auto make_worker(
    tmc::topology::thread_info Info, size_t PriorityRangeBegin,
    size_t PriorityRangeEnd,
    ex_cpu::task_queue_t::ExplicitProducer** StealOrder,
    std::atomic<tmc::detail::atomic_wait_t>& InitThreadsBarrier,
    // will be nullptr if hwloc is not enabled
    tmc::detail::hwloc_unique_bitmap& CpuSet,
    // will be nullptr if hwloc is not enabled
    void* HwlocTopo
  );

  // returns true if no tasks were found (caller should wait on cv)
  // returns false if thread stop requested (caller should exit)
  TMC_FORCE_INLINE inline bool try_run_some(
    std::stop_token& ThreadStopToken, const size_t Slot,
    const size_t PriorityRangeBegin, const size_t PriorityRangeEnd,
    size_t& PrevPriority, bool& Spinning
  );

  TMC_DECL_HWLOC void run_one(
    tmc::work_item& Item, const size_t Slot, const size_t Prio,
    size_t& PrevPriority, bool& Spinning
  );

  TMC_DECL_HWLOC std::coroutine_handle<>
  dispatch(std::coroutine_handle<> Outer, size_t Priority);

  TMC_DECL_HWLOC tmc::detail::InitParams* set_init_params();

  friend class aw_ex_scope_enter<ex_cpu>;
  friend tmc::detail::executor_traits<ex_cpu>;

  // not movable or copyable due to type_erased_this pointer being accessible by
  // child threads
  ex_cpu& operator=(const ex_cpu& Other) = delete;
  ex_cpu(const ex_cpu& Other) = delete;
  ex_cpu& operator=(ex_cpu&& Other) = delete;
  ex_cpu(ex_cpu&& Other) = delete;

public:
#ifdef TMC_USE_HWLOC
  /// Requires `TMC_USE_HWLOC`.
  /// Builder func to set the number of threads per core before calling
  /// `init()`. The default is 1.0f, which will cause
  /// `init()` to automatically create threads equal to the number of physical
  /// cores. If you want full SMT, set it to 2.0. Smaller increments (1.5, 1.75)
  /// are also valid to increase thread occupancy without full saturation.
  /// If the input is less than 1.0f, the minimum number of threads this can
  /// reduce a group to is 1.
  ///
  /// This only applies to CPU kinds specified in the 2nd parameter (defaults to
  /// P-cores). It can be called multiple times to set different occupancies for
  /// different CPU kinds.
  TMC_DECL_HWLOC ex_cpu& set_thread_occupancy(
    float ThreadOccupancy, tmc::topology::cpu_kind::value CpuKinds =
                             tmc::topology::cpu_kind::PERFORMANCE
  );

  /// Requires `TMC_USE_HWLOC`.
  /// Builder func to fill the SMT level of each core. On systems with multiple
  /// CPU kinds, the occupancy will be set separately for each CPU kind, based
  /// on its SMT level. (e.g. on Intel Hybrid, only P-cores have SMT, but on
  /// Apple M, neither P-cores nor E-cores have SMT)
  TMC_DECL_HWLOC ex_cpu& fill_thread_occupancy();

  /// Requires `TMC_USE_HWLOC`.
  /// Builder func to limit threads to a subset of the available CPUs.
  /// This affects both the thread count and thread affinities.
  ///
  /// If called multiple times, this can be used to create multiple subsets in
  /// the same executor, which can take tasks of different priorities. This can
  /// be used to steer work to different partitions based on
  /// priority, e.g. between P-cores and E-cores on hybrid CPUs.
  /// See the `hybrid_executor.cpp` example.
  TMC_DECL_HWLOC ex_cpu& add_partition(
    tmc::topology::topology_filter Filter, size_t PriorityRangeBegin = 0,
    size_t PriorityRangeEnd = TMC_MAX_PRIORITY_COUNT
  );

  /// Requires `TMC_USE_HWLOC`.
  /// Builder func to specify whether threads should be pinned/bound to
  /// specific cores, groups, or NUMA nodes. The default is GROUP.
  TMC_DECL_HWLOC ex_cpu&
  set_thread_pinning_level(tmc::topology::thread_pinning_level Level);

  /// Requires `TMC_USE_HWLOC`.
  /// Builder func to configure how threads should be allocated when the thread
  /// occupancy is less than the full system. This will only have any effect
  /// if `set_thread_count()` is called with a number less than the count of
  /// physical cores in the system.
  TMC_DECL_HWLOC ex_cpu&
  set_thread_packing_strategy(tmc::topology::thread_packing_strategy Strategy);

  /// Builder func to set a hook that will be invoked at the startup of each
  /// thread owned by this executor, and passed information about this thread.
  /// This overload requires `TMC_USE_HWLOC`.
  TMC_DECL_HWLOC ex_cpu&
  set_thread_init_hook(std::function<void(tmc::topology::thread_info)> Hook);

  /// Builder func to set a hook that will be invoked before destruction of each
  /// thread owned by this executor, and passed information about this thread.
  /// This overload requires `TMC_USE_HWLOC`.
  TMC_DECL_HWLOC ex_cpu& set_thread_teardown_hook(
    std::function<void(tmc::topology::thread_info)> Hook
  );
#endif
  /// Builder func to set the number of threads before calling `init()`.
  /// The maximum allowed value is equal to the number of bits on your
  /// platform (32 or 64 bit), unless TMC_MORE_THREADS is defined, in which case
  /// the number of threads is unlimited.
  /// If this is not called, the default behavior is:
  /// - If Linux cgroups CPU quota is detected, that will be used to
  /// set the number of threads (rounded down, to a minimum of 1).
  /// - Otherwise, if `TMC_USE_HWLOC` is enabled, 1 thread per physical core
  /// will be created.
  /// - Otherwise, `std::thread::hardware_concurrency()` threads will be
  /// created.
  TMC_DECL_HWLOC ex_cpu& set_thread_count(size_t ThreadCount);

  /// Gets the number of worker threads. Only useful after `init()` has been
  /// called.
  TMC_DECL_HWLOC size_t thread_count();

#ifndef TMC_PRIORITY_COUNT
  /// Builder func to set the number of priority levels before calling `init()`.
  /// The value must be in the range [1, 16].
  /// The default is 1.
  TMC_DECL_HWLOC ex_cpu& set_priority_count(size_t PriorityCount);
#endif

  /// Gets the number of priority levels. Only useful after `init()` has been
  /// called.
  TMC_DECL_HWLOC size_t priority_count();

  /// Builder func to set a hook that will be invoked at the startup of each
  /// thread owned by this executor, and passed the ordinal index
  /// [0..thread_count()-1] of the thread.
  TMC_DECL_HWLOC ex_cpu& set_thread_init_hook(std::function<void(size_t)> Hook);

  /// Builder func to set a hook that will be invoked before destruction of each
  /// thread owned by this executor, and passed the ordinal index
  /// [0..thread_count()-1] of the thread.
  TMC_DECL_HWLOC ex_cpu&
  set_thread_teardown_hook(std::function<void(size_t)> Hook);

  /// Builder func to set the number of times that a thread worker will spin
  /// looking for new work when all queues appear to be empty before suspending
  /// the thread.  Each spin is an asm("pause") followed by re-checking all
  /// queues. The default is 4.
  TMC_DECL_HWLOC ex_cpu& set_spins(size_t Spins);

  /// Builder func to configure the work-stealing strategy used internally by
  /// this executor. The default is `HIERARCHY_MATRIX`.
  TMC_DECL_HWLOC ex_cpu&
  set_work_stealing_strategy(tmc::work_stealing_strategy Strategy);

  /// Initializes the executor. If you want to customize the behavior, call the
  /// `set_X()` functions before calling `init()`. By default, uses hwloc to
  /// automatically generate threads, and creates 1 (or TMC_PRIORITY_COUNT)
  /// priority levels.
  ///
  /// If the executor is already initialized, calling `init()` will do nothing.
  TMC_DECL_HWLOC void init();

  /// Stops the executor, joins the worker threads, and destroys resources. Does
  /// not wait for any queued work to complete. `teardown()` must not be
  /// called from one of this executor's threads.
  ///
  /// Restores the executor to an uninitialized state. After
  /// calling `teardown()`, you may call `set_X()` to reconfigure the executor
  /// and call `init()` again.
  ///
  /// If the executor is not initialized, calling `teardown()` will do nothing.
  TMC_DECL_HWLOC void teardown();

  /// After constructing, you must call `init()` before use.
  TMC_DECL_HWLOC ex_cpu();

  /// Invokes `teardown()`. Must not be called from one of this executor's
  /// threads.
  TMC_DECL_HWLOC ~ex_cpu();

  /// Submits a single work_item to the executor. If Priority is out of range,
  /// it will be clamped to an in-range value.
  ///
  /// Rather than calling this directly, it is recommended to use the
  /// `tmc::post()` free function template.
  TMC_DECL_HWLOC void
  post(work_item&& Item, size_t Priority = 0, size_t ThreadHint = NO_HINT);

  /// Returns a pointer to the type erased `ex_any` version of this executor.
  /// This object shares a lifetime with this executor, and can be used for
  /// pointer-based equality comparison against
  /// the thread-local `tmc::current_executor()`.
  TMC_DECL_HWLOC tmc::ex_any* type_erased();

  /// Submits `count` items to the executor. `It` is expected to be an iterator
  /// type that implements `operator*()` and `It& operator++()`. If Priority is
  /// out of range, it will be clamped to an in-range value.
  ///
  /// Rather than calling this directly, it is recommended to use the
  /// `tmc::post_bulk()` free function template.
  template <typename It>
  void post_bulk(
    It&& Items, size_t Count, size_t Priority = 0, size_t ThreadHint = NO_HINT
  ) {
    clamp_priority(Priority);
    bool fromExecThread =
      tmc::detail::this_thread::executor() == &type_erased_this;
    bool allowedPriority =
      fromExecThread &&
      threads_by_priority_bitset[Priority].test_bit(current_thread_index());

    if (!fromExecThread) {
      ++ref_count;
    }
    if (ThreadHint < thread_count() &&
        // Check allowed priority of the target thread, not the current thread
        threads_by_priority_bitset[Priority].test_bit(ThreadHint))
      [[unlikely]] {
      size_t enqueuedCount = thread_states[ThreadHint].inbox->try_push_bulk(
        static_cast<It&&>(Items), Count, Priority
      );
      if (enqueuedCount != 0) {
        Count -= enqueuedCount;
        if (!fromExecThread || ThreadHint != tmc::current_thread_index()) {
          notify_hint(Priority, ThreadHint);
        }
      }
    }
    if (Count > 0) [[likely]] {
      if (allowedPriority) [[likely]] {
        work_queues[Priority].enqueue_bulk_ex_cpu(
          static_cast<It&&>(Items), Count
        );
      } else {
        work_queues[Priority].enqueue_bulk(static_cast<It&&>(Items), Count);
      }
      notify_n(Count, Priority, allowedPriority, true);
    }
    if (!fromExecThread) {
      --ref_count;
    }
  }
};

namespace detail {
template <> struct executor_traits<tmc::ex_cpu> {
  static TMC_DECL_HWLOC void post(
    tmc::ex_cpu& ex, tmc::work_item&& Item, size_t Priority, size_t ThreadHint
  );

  template <typename It>
  static inline void post_bulk(
    tmc::ex_cpu& ex, It&& Items, size_t Count, size_t Priority,
    size_t ThreadHint
  ) {
    ex.post_bulk(static_cast<It&&>(Items), Count, Priority, ThreadHint);
  }

  static TMC_DECL_HWLOC tmc::ex_any* type_erased(tmc::ex_cpu& ex);

  static TMC_DECL_HWLOC std::coroutine_handle<>
  dispatch(tmc::ex_cpu& ex, std::coroutine_handle<> Outer, size_t Priority);
};

#ifdef TMC_WINDOWS_DLL
TMC_DECL extern ex_cpu g_ex_cpu;
#ifdef TMC_IMPL
TMC_DECL ex_cpu g_ex_cpu;
#endif
#else
inline ex_cpu g_ex_cpu;
#endif
} // namespace detail

/// Returns a reference to the global instance of `tmc::ex_cpu`.
constexpr ex_cpu& cpu_executor() { return tmc::detail::g_ex_cpu; }
namespace detail {
TMC_DECL_HWLOC tmc::task<void> client_main_awaiter(
  tmc::task<int> ClientMainTask,
  std::atomic<tmc::detail::atomic_wait_t>* ExitCode_out
);
}

/// A convenience function that initializes `tmc::cpu_executor()`, submits the
/// ClientMainTask parameter to `tmc::cpu_executor()`, and then waits for it to
/// complete. The int value returned by the submitted task will be returned from
/// this function, so that you can use it as an exit code.
TMC_DECL_HWLOC int async_main(tmc::task<int>&& ClientMainTask);
} // namespace tmc

#if !defined(TMC_STANDALONE_COMPILATION_HWLOC) || defined(TMC_IMPL)
#include "tmc/detail/ex_cpu.ipp"
#endif
