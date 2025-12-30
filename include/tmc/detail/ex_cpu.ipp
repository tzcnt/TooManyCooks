// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

// Implementation definition file for tmc::ex_cpu. This will be included
// anywhere TMC_IMPL is defined. If you prefer to manually separate compilation
// units, you can instead include this file directly in a CPP file.

#include "tmc/current.hpp"
#include "tmc/detail/compat.hpp"
#include "tmc/detail/container_cpu_quota.hpp"
#include "tmc/detail/hwloc_unique_bitmap.hpp"
#include "tmc/detail/matrix.hpp"
#include "tmc/detail/qu_lockfree.hpp"
#include "tmc/detail/thread_layout.hpp"
#include "tmc/detail/thread_locals.hpp"
#include "tmc/ex_any.hpp"
#include "tmc/ex_cpu.hpp"
#include "tmc/sync.hpp"
#include "tmc/topology.hpp"
#include "tmc/work_item.hpp"

#include <bit>
#include <climits>
#include <coroutine>

#ifdef TMC_USE_HWLOC
#include <hwloc.h>
static_assert(sizeof(void*) == sizeof(hwloc_topology_t));
static_assert(sizeof(void*) == sizeof(hwloc_bitmap_t));
#endif

#ifdef TMC_DEBUG_THREAD_CREATION
#include <cstdio>
#endif

namespace tmc {

size_t ex_cpu::set_spin(size_t Slot) {
  return spinning_threads_bitset.fetch_or(
    TMC_ONE_BIT << Slot, std::memory_order_relaxed
  );
}
size_t ex_cpu::clr_spin(size_t Slot) {
  return spinning_threads_bitset.fetch_and(
    ~(TMC_ONE_BIT << Slot), std::memory_order_relaxed
  );
}
size_t ex_cpu::set_work(size_t Slot) {
  return working_threads_bitset.fetch_or(
    TMC_ONE_BIT << Slot, std::memory_order_relaxed
  );
}
size_t ex_cpu::clr_work(size_t Slot) {
  return working_threads_bitset.fetch_and(
    ~(TMC_ONE_BIT << Slot), std::memory_order_relaxed
  );
}

bool ex_cpu::is_initialized() {
  return initialized.load(std::memory_order_relaxed);
}

void ex_cpu::notify_n(
  size_t Count, size_t Priority, size_t ThreadHint, bool FromExecThread,
  bool AllowedPriority, bool FromPost
) {
  size_t spinningThreads = 0;
  size_t workingThreads = 0;
  size_t allowedThreads = threads_by_priority_bitset[Priority];
  if (ThreadHint < thread_count()) {
    size_t* neighbors = waker_matrix[Priority].get_row(ThreadHint);
    for (size_t i = 0; i < waker_matrix[Priority].cols; ++i) {
      size_t slot = neighbors[i];
      size_t bit = TMC_ONE_BIT << slot;
      thread_states[slot].sleep_wait.fetch_add(1, std::memory_order_seq_cst);
      spinningThreads =
        spinning_threads_bitset.load(std::memory_order_relaxed) &
        allowedThreads;
      workingThreads =
        working_threads_bitset.load(std::memory_order_relaxed) & allowedThreads;
      // If there are no spinning threads in this group, don't respect the
      // global spinner limit. If there is at least 1 spinning thread in the
      // group already, respect the global spinner limit.
      if ((spinningThreads & bit) != 0) {
        ptrdiff_t spinningThreadCount = std::popcount(spinningThreads);
        ptrdiff_t workingThreadCount = std::popcount(workingThreads);
        if (spinningThreadCount * 2 > workingThreadCount) {
          // There is already at least 1 spinning thread in this group
          return;
        }
      }
      if ((workingThreads & bit) == 0) {
        // TODO it would be nice to set thread as spinning before waking it -
        // so that multiple concurrent wakers don't syscall. However this can
        // lead to lost wakeups currently.
        // spinning_threads_bitset.fetch_or(hintBit,
        // std::memory_order_release);
        thread_states[slot].sleep_wait.notify_one();
        return;
      }
    }
  } else {
    // As a performance optimization, we only try to wake when we know
    // there is at least 1 sleeping thread. In combination with the inverse
    // barrier/double-check in the main worker loop, prevents lost wakeups.
    tmc::detail::memory_barrier();
    spinningThreads =
      spinning_threads_bitset.load(std::memory_order_relaxed) & allowedThreads;
    workingThreads =
      working_threads_bitset.load(std::memory_order_relaxed) & allowedThreads;
  }
  size_t spinningThreadCount =
    static_cast<size_t>(std::popcount(spinningThreads));
  size_t workingThreadCount =
    static_cast<size_t>(std::popcount(workingThreads));

  // Can't sum spinningThreadCount and workingThreadCount since the bitsets are
  // not synchronized, so a single thread may appear in both. OR them together
  // instead.
  size_t spinningOrWorkingThreads = workingThreads | spinningThreads;
  size_t sleepingThreads = ~spinningOrWorkingThreads & allowedThreads;
  size_t sleepingThreadCount =
    static_cast<size_t>(std::popcount(sleepingThreads));
#ifdef TMC_PRIORITY_COUNT
  if constexpr (PRIORITY_COUNT > 1)
#else
  if (PRIORITY_COUNT > 1)
#endif
  {
    if (FromPost) {
      // if available threads can take all tasks, no need to interrupt
      if (sleepingThreadCount < Count && workingThreadCount != 0) {
        size_t interruptCount = 0;
        size_t interruptMax = Count - sleepingThreadCount;
        if (workingThreadCount < interruptMax) {
          interruptMax = workingThreadCount;
        }
        for (size_t prio = PRIORITY_COUNT - 1; prio > Priority; --prio) {
          size_t set =
            task_stopper_bitsets[prio].load(std::memory_order_acquire);
          while (set != 0) {
            size_t slot = static_cast<size_t>(std::countr_zero(set));
            set = set & ~(TMC_ONE_BIT << slot);
            auto currentPrio = thread_states[slot].yield_priority.load(
              std::memory_order_relaxed
            );

            // 2 threads may request a task to yield at the same time. The
            // thread with the higher priority (lower priority index) should
            // prevail.
            while (currentPrio > Priority) {
              if (thread_states[slot].yield_priority.compare_exchange_strong(
                    currentPrio, Priority, std::memory_order_acq_rel
                  )) {
                if (++interruptCount == interruptMax) {
                  goto INTERRUPT_DONE;
                }
                break;
              }
            }
          }
        }
        // Currently, Count is not read after this point so this is not
        // necessary INTERRUPT_DONE:
        //   Count -= interruptCount;
      }
    }
  }
INTERRUPT_DONE:

  if (FromPost && spinningThreadCount != 0) {
    return;
  }
  if (sleepingThreadCount > 0) {
    // Limit the number of spinning threads to half the number of
    // working threads. This prevents too many spinners in a lightly
    // loaded system.
    if (spinningThreadCount != 0 &&
        spinningThreadCount * 2 > workingThreadCount) {
      return;
    }

    size_t base;
    // Optimize for most likely case where we are waking a thread near an
    // already running thread. Skip index 0 which is already running.
    size_t startIdx = 1;
    if (FromExecThread) [[likely]] {
      if (AllowedPriority) [[likely]] {
        // Index 0 is this thread, which is already awake, so start at index 1
        base = current_thread_index();
      } else {
        if (spinningOrWorkingThreads == 0) {
          // All executor threads in the target priority group are sleeping;
          // wake a thread that is bound to a CPU near the currently executing
          // executor thread.
          base = current_thread_index();
          startIdx = 0;
        } else {
          // Choose a working thread and try to wake a thread near it
          // (start at index 1)
          base =
            static_cast<size_t>(std::countr_zero(spinningOrWorkingThreads));
        }
      }
    } else {
#ifdef TMC_USE_HWLOC
      if (spinningOrWorkingThreads != 0) [[likely]] {
        // Choose a working thread and try to wake a thread near it
        // (start at index 1)
        base = static_cast<size_t>(std::countr_zero(spinningOrWorkingThreads));
      } else {
        // All executor threads are sleeping; wake a thread that is bound to a
        // CPU near the currently executing non-executor thread.
        tmc::detail::hwloc_unique_bitmap set = hwloc_bitmap_alloc();
        auto topo = topology;
        if (0 == hwloc_get_last_cpu_location(topo, set, HWLOC_CPUBIND_THREAD)) {
          auto i = hwloc_bitmap_first(set);
          auto pu =
            hwloc_get_pu_obj_by_os_index(topo, static_cast<unsigned int>(i));
          // This matrix is 1 column wide
          base = external_waker_list[Priority].get_row(pu->logical_index)[0];
        } else {
          base = 0;
        }
        startIdx = 0;
      }
#else
      // Treat thread bitmap as a stack - OS can balance them as needed
      base = static_cast<size_t>(std::countr_zero(sleepingThreads));
      startIdx = 0;
#endif
    }
    // Wake exactly 1 thread
    size_t* threadsWakeList = waker_matrix[Priority].get_row(base);
    for (size_t i = startIdx; i < waker_matrix[Priority].cols; ++i) {
      size_t slot = threadsWakeList[i];
      size_t bit = TMC_ONE_BIT << slot;
      if ((sleepingThreads & bit) != 0) {
        thread_states[slot].sleep_wait.fetch_add(1, std::memory_order_acq_rel);
        thread_states[slot].sleep_wait.notify_one();
        return;
      }
    }
  }
}

ex_cpu::task_queue_t::ExplicitProducer*** ex_cpu::init_queue_iteration_order(
  std::vector<std::vector<size_t>> const& Forward
) {
  task_queue_t::ExplicitProducer*** producers =
    new task_queue_t::ExplicitProducer**[PRIORITY_COUNT];
  for (size_t prio = 0; prio < PRIORITY_COUNT; ++prio) {
    if (Forward[prio].empty()) {
      producers[prio] = nullptr;
    } else {
      // An additional is entry inserted at index 1 to cache the
      // most-recently-stolen-from producer.
      auto sz = Forward[prio].size() + 1;
      producers[prio] = new task_queue_t::ExplicitProducer*[sz];
    }
  }

  for (size_t prio = 0; prio < PRIORITY_COUNT; ++prio) {
    if (Forward[prio].empty()) {
      continue;
    }
    auto& thisMatrix = Forward[prio];
    // pointer to this thread's producer
    producers[prio][0] = &work_queues[prio].staticProducers[thisMatrix[0]];
    // pointer to previously consumed-from producer (initially none)
    producers[prio][1] = nullptr;

    for (size_t i = 1; i < Forward[prio].size(); ++i) {
      producers[prio][i + 1] =
        &work_queues[prio].staticProducers[thisMatrix[i]];
    }
  }
  return producers;
}

void ex_cpu::init_thread_locals(size_t Slot) {
  tmc::detail::this_thread::executor = &type_erased_this;
  tmc::detail::this_thread::this_task = {
    .prio = 0, .yield_priority = &thread_states[Slot].yield_priority
  };
  tmc::detail::this_thread::thread_index = Slot;
}

void ex_cpu::clear_thread_locals() {
  tmc::detail::this_thread::executor = nullptr;
  tmc::detail::this_thread::this_task = {};
}

void ex_cpu::run_one(
  tmc::work_item& Item, const size_t Slot, const size_t Prio,
  size_t& PrevPriority, bool& WasSpinning
) {
  if (WasSpinning) {
    WasSpinning = false;
    set_work(Slot);
    clr_spin(Slot);
    // Wake 1 nearest neighbor. Don't priority-preempt any running tasks
    notify_n(1, Prio, NO_HINT, true, true, false);
  }
#ifdef TMC_PRIORITY_COUNT
  if constexpr (PRIORITY_COUNT > 1)
#else
  if (PRIORITY_COUNT > 1)
#endif
  {
    thread_states[Slot].yield_priority.store(Prio, std::memory_order_release);
    if (Prio != PrevPriority) {
      if (PrevPriority != NO_TASK_RUNNING) {
        task_stopper_bitsets[PrevPriority].fetch_and(
          ~(TMC_ONE_BIT << Slot), std::memory_order_acq_rel
        );
      }
      task_stopper_bitsets[Prio].fetch_or(
        TMC_ONE_BIT << Slot, std::memory_order_acq_rel
      );
      tmc::detail::this_thread::this_task.prio = Prio;
      PrevPriority = Prio;
    }
  }

  Item();
  assert(
    Prio == tmc::current_priority() && "Tasks should not modify the priority "
                                       "directly. Use tmc::change_priority() "
                                       "or .with_priority() instead."
  );
}

// returns true if no tasks were found (caller should wait on cv)
// returns false if thread stop requested (caller should exit)
bool ex_cpu::try_run_some(
  std::stop_token& ThreadStopToken, const size_t Slot,
  const size_t PriorityRangeBegin, const size_t PriorityRangeEnd,
  size_t& PrevPriority
) {
  // Precondition: this thread is spinning / not working
  bool wasSpinning = true;

  while (true) {
  TOP:
    if (ThreadStopToken.stop_requested()) [[unlikely]] {
      return false;
    }
    work_item item;

    // For priority 0, check private queue, then inbox, then try to steal
    // Lower priorities can just check private queue and steal - no inbox
    // Although this could be combined into the following loop (with an if
    // statement to remove the inbox check), it gives better codegen for the
    // fast path to keep it separate.
    if (work_queues[PriorityRangeBegin].try_dequeue_ex_cpu_private(
          item, PriorityRangeBegin
        )) [[likely]] {
      run_one(item, Slot, PriorityRangeBegin, PrevPriority, wasSpinning);
      goto TOP;
    }

    // Inbox may retrieve items with out of order priority.
    // This also may allow threads to run work items that are outside of their
    // normally assigned priorities.
    size_t inbox_prio;
    if (thread_states[Slot].inbox->try_pull(item, inbox_prio)) {
      run_one(item, Slot, inbox_prio, PrevPriority, wasSpinning);
      goto TOP;
    }

    if (work_queues[PriorityRangeBegin].try_dequeue_ex_cpu_steal(
          item, PriorityRangeBegin
        )) {
      run_one(item, Slot, PriorityRangeBegin, PrevPriority, wasSpinning);
      goto TOP;
    }

    // Now check lower priority queues
    for (size_t prio = PriorityRangeBegin + 1; prio < PriorityRangeEnd;
         ++prio) {
      if (work_queues[prio].try_dequeue_ex_cpu_private(item, prio)) {
        run_one(item, Slot, prio, PrevPriority, wasSpinning);
        goto TOP;
      }
      if (work_queues[prio].try_dequeue_ex_cpu_steal(item, prio)) {
        run_one(item, Slot, prio, PrevPriority, wasSpinning);
        goto TOP;
      }
    }
    return true;
  }
}

void ex_cpu::clamp_priority(size_t& Priority) {
#ifdef TMC_PRIORITY_COUNT
  if constexpr (PRIORITY_COUNT == 1) {
    Priority = 0;
    return;
  }
#endif
  if (Priority > PRIORITY_COUNT - 1) {
    Priority = PRIORITY_COUNT - 1;
  }
}

void ex_cpu::post(work_item&& Item, size_t Priority, size_t ThreadHint) {
  clamp_priority(Priority);
  bool fromExecThread = tmc::detail::this_thread::executor == &type_erased_this;
  bool allowedPriority = false;
  if (ThreadHint < thread_count()) {
    allowedPriority =
      (0 != (0b1 & (threads_by_priority_bitset[Priority] >> ThreadHint)));
  } else if (fromExecThread) {
    allowedPriority =
      (0 != (0b1 &
             (threads_by_priority_bitset[Priority] >> current_thread_index())));
  }

  if (!fromExecThread) {
    ++ref_count;
  }

  if (ThreadHint < thread_count() && allowedPriority &&
      thread_states[ThreadHint].inbox->try_push(
        static_cast<work_item&&>(Item), Priority
      )) {
    if (!fromExecThread || ThreadHint != tmc::current_thread_index()) {
      notify_n(1, Priority, ThreadHint, fromExecThread, allowedPriority, true);
    }
  } else [[likely]] {
    if (fromExecThread && allowedPriority) [[likely]] {
      work_queues[Priority].enqueue_ex_cpu(
        static_cast<work_item&&>(Item), Priority
      );
    } else {
      work_queues[Priority].enqueue(static_cast<work_item&&>(Item));
    }
    notify_n(1, Priority, NO_HINT, fromExecThread, allowedPriority, true);
  }
  if (!fromExecThread) {
    --ref_count;
  }
}

tmc::ex_any* ex_cpu::type_erased() { return &type_erased_this; }

// Default constructor does not call init() - you need to do it afterward
ex_cpu::ex_cpu()
    : init_params{nullptr}, type_erased_this(this), thread_stoppers{},
      task_stopper_bitsets{nullptr}, thread_states{nullptr}, ref_count{0}
#ifndef TMC_PRIORITY_COUNT
      ,
      PRIORITY_COUNT{1}
#endif
{
  initialized.store(false, std::memory_order_seq_cst);
}

auto ex_cpu::make_worker(
  tmc::topology::thread_info Info, size_t PriorityRangeBegin,
  size_t PriorityRangeEnd, ex_cpu::task_queue_t::ExplicitProducer*** StealOrder,
  std::atomic<int>& InitThreadsBarrier,
  // will be nullptr if hwloc is not enabled
  [[maybe_unused]] tmc::detail::hwloc_unique_bitmap& CpuSet
) {
  std::function<void(tmc::topology::thread_info)> ThreadTeardownHook = nullptr;
  if (init_params != nullptr && init_params->thread_teardown_hook != nullptr) {
    ThreadTeardownHook = init_params->thread_teardown_hook;
  }

  return [this, StealOrder, Info, PriorityRangeBegin, PriorityRangeEnd,
          &InitThreadsBarrier, ThreadTeardownHook
#ifdef TMC_USE_HWLOC
          ,
          myCpuSet = CpuSet.clone()
#endif
  ](std::stop_token ThreadStopToken) mutable {
    // Ensure this thread sees all non-atomic read-only values
    tmc::detail::memory_barrier();

    size_t Slot = Info.index;

#ifdef TMC_USE_HWLOC
    if (myCpuSet != nullptr) {
      tmc::detail::pin_thread(topology, myCpuSet, Info.group.cpu_kind);
    }
    myCpuSet.free();
#endif

    init_thread_locals(Slot);
    tmc::detail::this_thread::producers = StealOrder;

    if (init_params != nullptr && init_params->thread_init_hook != nullptr) {
      init_params->thread_init_hook(Info);
    }

    InitThreadsBarrier.fetch_sub(1);
    InitThreadsBarrier.notify_all();

    // Initialization complete, commence runloop
    size_t previousPrio = NO_TASK_RUNNING;
  TOP:
    while (try_run_some(
      ThreadStopToken, Slot, PriorityRangeBegin, PriorityRangeEnd, previousPrio
    )) {
      size_t spinningThreads = set_spin(Slot);
      size_t workingThreads = clr_work(Slot);

      // Limit the number of spinning threads to half the number of
      // working threads. This prevents too many spinners in a lightly
      // loaded system.
      size_t spinningThreadCount =
        static_cast<size_t>(std::popcount(spinningThreads)) + 1;
      size_t workingThreadCount =
        static_cast<size_t>(std::popcount(workingThreads)) - 1;
      if (2 * spinningThreadCount <= workingThreadCount) {
        for (size_t i = 0; i < spins; ++i) {
          TMC_CPU_PAUSE();
          if (!thread_states[Slot].inbox->empty()) {
            goto TOP;
          }
          for (size_t prio = 0; prio < PRIORITY_COUNT; ++prio) {
            if (!work_queues[prio].empty()) {
              goto TOP;
            }
          }
        }
      }

#ifdef TMC_PRIORITY_COUNT
      if constexpr (PRIORITY_COUNT > 1)
#else
      if (PRIORITY_COUNT > 1)
#endif
      {
        if (previousPrio != NO_TASK_RUNNING) {
          task_stopper_bitsets[previousPrio].fetch_and(
            ~(TMC_ONE_BIT << Slot), std::memory_order_acq_rel
          );
        }
      }
      previousPrio = NO_TASK_RUNNING;

      // Transition from spinning to sleeping.
      int waitValue =
        thread_states[Slot].sleep_wait.load(std::memory_order_relaxed);
      clr_spin(Slot);
      tmc::detail::memory_barrier(); // pairs with barrier in notify_n

      // Double check that the queue is empty after the memory
      // barrier. In combination with the inverse double-check in
      // notify_n, this prevents any lost wakeups.
      if (!thread_states[Slot].inbox->empty()) {
        set_spin(Slot);
        goto TOP;
      }
      for (size_t prio = 0; prio < PRIORITY_COUNT; ++prio) {
        if (!work_queues[prio].empty()) {
          set_spin(Slot);
          goto TOP;
        }
      }

      // No work found. Go to sleep.
      if (ThreadStopToken.stop_requested()) [[unlikely]] {
        break;
      }
      thread_states[Slot].sleep_wait.wait(waitValue);
      set_spin(Slot);
    }

    // Thread stop has been requested (executor is shutting down)
    working_threads_bitset.fetch_and(~(TMC_ONE_BIT << Slot));
    if (ThreadTeardownHook != nullptr) {
      ThreadTeardownHook(Info);
    }

    clear_thread_locals();
    auto ppp = static_cast<task_queue_t::ExplicitProducer***>(
      tmc::detail::this_thread::producers
    );
    for (size_t i = 0; i < waker_matrix.size(); ++i) {
      delete[] ppp[i];
    }
    delete[] ppp;
    tmc::detail::this_thread::producers = nullptr;
  };
}

void ex_cpu::init() {
  if (initialized) {
    return;
  }
  initialized.store(true, std::memory_order_relaxed);

#ifndef TMC_PRIORITY_COUNT
  if (init_params != nullptr && init_params->priority_count != 0) {
    PRIORITY_COUNT = init_params->priority_count;
  } else {
    PRIORITY_COUNT = 1;
  }
  NO_TASK_RUNNING = PRIORITY_COUNT;
#endif
  task_stopper_bitsets = new std::atomic<size_t>[PRIORITY_COUNT];

  if (init_params == nullptr) {
    init_params = new tmc::detail::InitParams;
  }
  spins = init_params->spins;

  std::vector<tmc::detail::hwloc_unique_bitmap> partitionCpusets;
#ifndef TMC_USE_HWLOC
  // Treat all cores as part of the same group
  std::vector<tmc::topology::detail::CacheGroup> groupedCores;
  {
    size_t nthreads;
    if (init_params->thread_count != 0) {
      nthreads = init_params->thread_count;
    } else {
      // If running in a container with CPU limits, set thread count to quota,
      // if a specific number of threads was not requested
      auto containerQuota = tmc::detail::query_container_cpu_quota();
      if (containerQuota.is_container_limited()) {
        size_t containerLimit = static_cast<size_t>(containerQuota.cpu_count);
        if (containerLimit == 0) {
          containerLimit = 1;
        }
        nthreads = containerLimit;
      } else {
        nthreads = std::thread::hardware_concurrency();
      }
      // limited to 32/64 threads for now, due to use of size_t bitset
      if (nthreads > TMC_PLATFORM_BITS) {
        nthreads = TMC_PLATFORM_BITS;
      }
    }
    groupedCores.push_back(
      tmc::topology::detail::CacheGroup{nullptr, 0, 0, {}, {}, nthreads, 0}
    );
  }
#else
  hwloc_topology_t topo;
  auto internalTopo = tmc::topology::detail::query_internal(topo);
  topology = topo;
  auto& groupedCores = internalTopo.groups;

  // Maintain an unmodified copy of the groups for PU indexing later
  auto puIndexingGroups = groupedCores;
  auto flatGroups = tmc::topology::detail::flatten_groups(internalTopo.groups);

  // Default to a partition that excludes LP E-cores. This is only necessary
  // for multi-threaded executors.
  if (init_params->partitions.empty()) {
    init_params->partitions.emplace_back();
    init_params->priority_ranges.push_back({0, PRIORITY_COUNT});
  }

  {
    [[maybe_unused]] size_t prioritiesCoveredBitset = 0;
    for (size_t i = 0; i < init_params->priority_ranges.size(); ++i) {
      [[maybe_unused]] size_t begin = init_params->priority_ranges[i].begin;
      [[maybe_unused]] size_t end = init_params->priority_ranges[i].end;
      if (end > PRIORITY_COUNT) {
        init_params->priority_ranges[i].end = PRIORITY_COUNT;
        end = PRIORITY_COUNT;
      }
      assert(
        begin < end && "Executor partition PriorityRangeBegin must be less "
                       "than PriorityRangeEnd!"
      );

      prioritiesCoveredBitset |= (TMC_ONE_BIT << end) - (TMC_ONE_BIT << begin);
    }

    // Together, all of the partitions in the executor should cover the number
    // of priorities configured by set_priority_count(). Otherwise, a task
    // submitted with that priority would have no thread to execute it.
    [[maybe_unused]] size_t expectedPrioritiesBitset =
      (TMC_ONE_BIT << PRIORITY_COUNT) - 1;
    assert(
      prioritiesCoveredBitset == expectedPrioritiesBitset &&
      "Executor partitions do not match allowed priorities!"
      " At least one partition must include each allowed priority"
      " in the range [ 0..priority_count() )."
    );
  }

  partitionCpusets.resize(init_params->partitions.size());
  tmc::topology::cpu_kind::value unused;
  for (size_t i = 0; i < init_params->partitions.size(); ++i) {
    partitionCpusets[i] = tmc::detail::make_partition_cpuset(
      topo, internalTopo, init_params->partitions[i], unused
    );
#ifdef TMC_DEBUG_THREAD_CREATION
    std::printf("ex_cpu partition %zu cpuset bitmap: ", i);
    partitionCpusets[i].print();
#endif
  }

  // Combine the partitions into a single filter for grouping
  tmc::topology::topology_filter filter = init_params->partitions[0];
  for (size_t i = 1; i < init_params->partitions.size(); ++i) {
    filter = filter | init_params->partitions[i];
  }
  // adjust_thread_groups modifies groupedCores in place
  tmc::detail::adjust_thread_groups(
    init_params->thread_count, init_params->thread_occupancy, flatGroups,
    filter, init_params->pack
  );
#endif
  // After adjust_thread_groups, some groups might be empty. We only care
  // about the non-empty groups from this point going forward. Use a different
  // name for this variable for clarification.
  size_t totalThreadCount = 0;
  std::vector<tmc::topology::detail::CacheGroup*> nonEmptyGroups;
  tmc::detail::for_all_groups(
    groupedCores,
    [&nonEmptyGroups,
     &totalThreadCount](tmc::topology::detail::CacheGroup& group) {
      if (group.group_size != 0) {
        nonEmptyGroups.push_back(&group);
        totalThreadCount += group.group_size;
      }
    }
  );

  assert(
    totalThreadCount != 0 &&
    "Partition configuration resulted in zero usable cores. Check that the "
    "specified partition IDs are valid and within the allowed cpuset."
  );
  // limited to 32/64 threads for now, due to use of size_t bitset
  assert(totalThreadCount <= TMC_PLATFORM_BITS);
  threads.resize(totalThreadCount);

  inboxes.resize(nonEmptyGroups.size());
  inboxes.fill_default();

  thread_states = new ThreadState[thread_count()];
  for (size_t i = 0; i < thread_count(); ++i) {
    thread_states[i].yield_priority = NO_TASK_RUNNING;
    thread_states[i].sleep_wait = 0;
  }

  thread_stoppers.resize(thread_count());
  // All threads start in the "spinning / not working" state
  working_threads_bitset.store(0);
  spinning_threads_bitset.store(
    (TMC_ONE_BIT << (thread_count() - 1)) |
    ((TMC_ONE_BIT << (thread_count() - 1)) - 1)
  );

  struct ThreadConstructData {
    size_t priorityRangeBegin;
    size_t priorityRangeEnd;
    tmc::detail::hwloc_unique_bitmap threadCpuset;
    std::vector<size_t> slotsByPrio;
    tmc::topology::thread_info info;
  };
  std::vector<ThreadConstructData> threadData;
  threadData.resize(thread_count());

  threads_by_priority_bitset.resize(PRIORITY_COUNT);

  // The set of thread indexes that are allowed to participate in each priority
  // level
  std::vector<std::vector<size_t>> tidsByPrio;
  tidsByPrio.resize(PRIORITY_COUNT);

  // Initially contains PRIORITY_COUNT separate clones of groupedCores.
  std::vector<std::vector<tmc::topology::detail::CacheGroup>> groupsByPrio;
  groupsByPrio.resize(PRIORITY_COUNT, groupedCores);

  // Flattened view of non-empty groups.
  // Pointers to the separate clones in groupsByPrio, excluding empty groups.
  std::vector<std::vector<tmc::topology::detail::CacheGroup*>> flatGroupsByPrio;
  flatGroupsByPrio.resize(PRIORITY_COUNT);
  for (size_t prio = 0; prio < groupsByPrio.size(); ++prio) {
    tmc::detail::for_all_groups(
      groupsByPrio[prio],
      [p = &flatGroupsByPrio[prio]](tmc::topology::detail::CacheGroup& group) {
        if (group.group_size != 0) {
          p->push_back(&group);
        }
      }
    );
  }

  size_t slot = 0;
  for (size_t groupIdx = 0; groupIdx < nonEmptyGroups.size(); ++groupIdx) {
    auto& coreGroup = *nonEmptyGroups[groupIdx];
    size_t groupSize = coreGroup.group_size;
#ifdef TMC_USE_HWLOC
    tmc::detail::hwloc_unique_bitmap threadCpuset = nullptr;
    if (!coreGroup.cores.empty()) {
      if (init_params->pin == tmc::topology::thread_pinning_level::GROUP) {
        // Construct the group cpuset out of its allowed cores, which may be
        // more restricted than the cache obj->cpuset.
        threadCpuset = hwloc_bitmap_alloc();
        for (size_t i = 0; i < coreGroup.cores.size(); ++i) {
          hwloc_bitmap_or(
            threadCpuset, threadCpuset, coreGroup.cores[i].cpuset
          );
        }
      } else if (init_params->pin ==
                 tmc::topology::thread_pinning_level::NUMA) {
        if (coreGroup.cores[0].numa != nullptr) {
          threadCpuset = hwloc_bitmap_dup(coreGroup.cores[0].numa->cpuset);
        }
      }
    }

#endif
    for (size_t subIdx = 0; subIdx < groupSize; ++subIdx) {
      thread_states[slot].inbox = &inboxes[groupIdx];
#ifdef TMC_USE_HWLOC
      if (!coreGroup.cores.empty() &&
          init_params->pin == tmc::topology::thread_pinning_level::CORE) {
        // User can only set thread occupancy per group, not per core... so
        // just modulo the thread index against the number of cores
        auto coreIdx = subIdx % coreGroup.cores.size();
        threadCpuset = hwloc_bitmap_dup(coreGroup.cores[coreIdx].cpuset);
      }
      if (threadCpuset == nullptr) {
        threadCpuset =
          hwloc_bitmap_dup(hwloc_topology_get_allowed_cpuset(topo));
      }
#ifdef TMC_DEBUG_THREAD_CREATION
      std::printf(
        "ex_cpu group %zu thread %zu cpuset bitmap: ", groupIdx, subIdx
      );
      threadCpuset.print();
#endif
      size_t priorityRangeBegin = TMC_ALL_ONES;
      size_t priorityRangeEnd = 0;
      for (size_t i = 0; i < partitionCpusets.size(); ++i) {
        if (hwloc_bitmap_intersects(threadCpuset, partitionCpusets[i])) {
          if (init_params->priority_ranges[i].begin < priorityRangeBegin) {
            priorityRangeBegin = init_params->priority_ranges[i].begin;
          }
          if (init_params->priority_ranges[i].end > priorityRangeEnd) {
            priorityRangeEnd = init_params->priority_ranges[i].end;
          }
        }
      }
#else
      size_t priorityRangeBegin = 0;
      size_t priorityRangeEnd = PRIORITY_COUNT;
#endif

      threadData[slot].slotsByPrio.resize(PRIORITY_COUNT);
      for (size_t prio = 0; prio < PRIORITY_COUNT; ++prio) {
        if (prio >= priorityRangeBegin && prio < priorityRangeEnd) {
          threadData[slot].slotsByPrio[prio] = tidsByPrio[prio].size();
          tidsByPrio[prio].push_back(slot);
          threads_by_priority_bitset[prio] |= TMC_ONE_BIT << slot;
        } else {
          // this thread doesn't participate in this priority level's queue
          threadData[slot].slotsByPrio[prio] = TMC_ALL_ONES;
          flatGroupsByPrio[prio][groupIdx]->group_size--;
        }
      }

#ifdef TMC_USE_HWLOC
      if (init_params->pin == tmc::topology::thread_pinning_level::CORE) {
        threadData[slot].threadCpuset = std::move(threadCpuset);
      } else {
        // This cpuset is reused for multiple threads.
        threadData[slot].threadCpuset = threadCpuset.clone();
      }
      threadData[slot].info.index_within_group = subIdx;
      threadData[slot].info.group =
        tmc::detail::public_group_from_private(coreGroup);
#endif
      threadData[slot].priorityRangeBegin = priorityRangeBegin;
      threadData[slot].priorityRangeEnd = priorityRangeEnd;
      threadData[slot].info.index = slot;
      ++slot;
    }
  }

  work_queues.resize(PRIORITY_COUNT);
  for (size_t prio = 0; prio < PRIORITY_COUNT; ++prio) {
    size_t threadCount = tidsByPrio[prio].size();
    work_queues.emplace_at(prio, threadCount + 1);
    work_queues[prio].staticProducers =
      new task_queue_t::ExplicitProducer[threadCount];
    for (size_t i = 0; i < threadCount; ++i) {
      work_queues[prio].staticProducers[i].init(&work_queues[prio]);
    }
    work_queues[prio].dequeueProducerCount = threadCount + 1;
  }

  std::vector<tmc::detail::Matrix> stealMatrixes;
  stealMatrixes.resize(PRIORITY_COUNT);
  waker_matrix.resize(PRIORITY_COUNT);
#ifdef TMC_USE_HWLOC
  external_waker_list.resize(PRIORITY_COUNT);
#endif

  for (size_t prio = 0; prio < PRIORITY_COUNT; ++prio) {
    // If there are no priority partitions, all of the steal/waker matrixes can
    // refer to the same underlying data.
    if (partitionCpusets.size() < 2 && prio > 0) {

      stealMatrixes[prio].set_weak_ref(stealMatrixes[0]);
      waker_matrix[prio].set_weak_ref(waker_matrix[0]);
#ifdef TMC_USE_HWLOC
      external_waker_list[prio].set_weak_ref(external_waker_list[0]);
#endif

#ifdef TMC_DEBUG_THREAD_CREATION
      if (prio == 1) {
        std::printf("All priorities share the same matrixes.\n");
      }
#endif
      continue;
    }
    // Steal matrix is sliced up and shared with each thread.
    std::vector<size_t> rawMatrix;
    if (init_params->strategy == work_stealing_strategy::LATTICE_MATRIX) {
      rawMatrix = detail::get_lattice_matrix(groupsByPrio[prio]);
    } else {
      rawMatrix = detail::get_hierarchical_matrix(groupsByPrio[prio]);
    }
    stealMatrixes[prio].init(std::move(rawMatrix), tidsByPrio[prio].size());
#ifdef TMC_DEBUG_THREAD_CREATION
    std::printf("ex_cpu priority %zu stealMatrix (local thread IDs):\n", prio);
    stealMatrixes[prio].print(nullptr);
#endif

    /*** CONSTRUCT WAKER MATRIXES ***/

    // Waker matrix is kept as a member so it can be accessed by any thread.
    waker_matrix[prio].init(
      TMC_ALL_ONES, thread_count(), tidsByPrio[prio].size()
    );

    // Step 1: For threads included in the priority group, copy rows from their
    // waker matrixes, but convert them from local to global thread indexes.
    auto includedWakers = stealMatrixes[prio].to_wakers();
    for (size_t localTid = 0; localTid < tidsByPrio[prio].size(); ++localTid) {
      size_t globalTid = tidsByPrio[prio][localTid];
      size_t* dstRow = waker_matrix[prio].get_row(globalTid);
      size_t* srcRow = includedWakers.get_row(localTid);
      for (size_t wakeIdx = 0; wakeIdx < tidsByPrio[prio].size(); ++wakeIdx) {
        auto localWakeTid = srcRow[wakeIdx];
        dstRow[wakeIdx] = tidsByPrio[prio][localWakeTid];
      }
    }

    // Step 2: For threads excluded from the priority group, fill in their waker
    // rows by copying from the included threads. This means that excluded
    // threads can only wake included threads when submitting work.
    size_t srcLocalTid = 0;
    for (size_t dstGlobalTid = 0; dstGlobalTid < thread_count();
         ++dstGlobalTid) {
      if (waker_matrix[prio].get_row(dstGlobalTid)[0] ==
          TMC_ALL_ONES) { // indicates this group was excluded
        size_t srcGlobalTid = tidsByPrio[prio][srcLocalTid];
        waker_matrix[prio].copy_row(
          dstGlobalTid, srcGlobalTid, waker_matrix[prio]
        );

        ++srcLocalTid;
        if (srcLocalTid == tidsByPrio[prio].size()) {
          srcLocalTid = 0;
        }
      }
    }

#ifdef TMC_DEBUG_THREAD_CREATION
    std::printf(
      "ex_cpu priority %zu waker_matrix (global thread IDs):\n", prio
    );
    waker_matrix[prio].print(nullptr);
#endif

#ifdef TMC_USE_HWLOC
    // Step 3: Construct a separate waker list for external threads. These don't
    // have executor thread IDs, so we instead map the PU they are executing on
    // (with hwloc) to an executor thread that is executing in the vicinity.

    // This calculation needs all cores (including excluded ones) and all groups
    // (including empty ones). So get the original groups (which include all the
    // cores) and then set their group_size.
    auto flatPuIndexingGroups =
      tmc::topology::detail::flatten_groups(puIndexingGroups);
    auto puIndexingByPrio =
      tmc::topology::detail::flatten_groups(groupsByPrio[prio]);
    for (size_t i = 0; i < flatPuIndexingGroups.size(); ++i) {
      flatPuIndexingGroups[i]->group_size = puIndexingByPrio[i]->group_size;
    }

    auto puIndexes = tmc::detail::get_all_pu_indexes(flatPuIndexingGroups);
    external_waker_list[prio].init(std::move(puIndexes), puIndexes.size(), 1);
#endif
  }

  // Start the worker threads
  std::atomic<int> initThreadsBarrier(static_cast<int>(thread_count()));
  tmc::detail::memory_barrier();

  slot = 0;
  for (; slot < threadData.size(); ++slot) {
    auto& d = threadData[slot];
    std::vector<std::vector<size_t>> myMatrixes;
    myMatrixes.resize(PRIORITY_COUNT);
    for (size_t prio = 0; prio < PRIORITY_COUNT; ++prio) {
      if (d.slotsByPrio[prio] == TMC_ALL_ONES) {
        continue;
      }
      myMatrixes[prio] = stealMatrixes[prio].get_slice(d.slotsByPrio[prio]);
    }
    auto stealOrder = init_queue_iteration_order(myMatrixes);
    threads.emplace_at(
      slot, make_worker(
              d.info, d.priorityRangeBegin, d.priorityRangeEnd, stealOrder,
              initThreadsBarrier, d.threadCpuset
            )
    );
    thread_stoppers.emplace_at(slot, threads[slot].get_stop_source());
  }

  // Wait for all workers to finish init before cleaning up
  auto barrierVal = initThreadsBarrier.load();
  while (barrierVal != 0) {
    initThreadsBarrier.wait(barrierVal);
    barrierVal = initThreadsBarrier.load();
  }

  if (init_params != nullptr) {
    delete init_params;
    init_params = nullptr;
  }
}

tmc::detail::InitParams* ex_cpu::set_init_params() {
  assert(!is_initialized());
  if (init_params == nullptr) {
    init_params = new tmc::detail::InitParams;
  }
  return init_params;
}

#ifdef TMC_USE_HWLOC
ex_cpu& ex_cpu::set_thread_occupancy(
  float ThreadOccupancy, tmc::topology::cpu_kind::value CpuKinds
) {
  set_init_params()->set_thread_occupancy(ThreadOccupancy, CpuKinds);
  return *this;
}

ex_cpu& ex_cpu::fill_thread_occupancy() {
  auto topo = tmc::topology::query();
  for (size_t kind = 0; kind < topo.cpu_kind_counts.size(); ++kind) {
    for (size_t i = 0; i < topo.group_count(); ++i) {
      auto& group = topo.groups[i];
      if (group.cpu_kind ==
          static_cast<tmc::topology::cpu_kind::value>(TMC_ONE_BIT << kind)) {
        set_thread_occupancy(
          static_cast<float>(group.smt_level), group.cpu_kind
        );
        break;
      }
    }
  }
  return *this;
}

ex_cpu& ex_cpu::add_partition(
  tmc::topology::topology_filter Filter, size_t PriorityRangeBegin,
  size_t PriorityRangeEnd
) {
  auto params = set_init_params();
  params->add_partition(Filter);
  params->priority_ranges.push_back({PriorityRangeBegin, PriorityRangeEnd});
  return *this;
}

ex_cpu&
ex_cpu::set_thread_pinning_level(tmc::topology::thread_pinning_level Level) {
  set_init_params()->set_thread_pinning_level(Level);
  return *this;
}

ex_cpu& ex_cpu::set_thread_packing_strategy(
  tmc::topology::thread_packing_strategy Strategy
) {
  set_init_params()->set_thread_packing_strategy(Strategy);
  return *this;
}
#endif

#ifndef TMC_PRIORITY_COUNT
ex_cpu& ex_cpu::set_priority_count(size_t PriorityCount) {
  set_init_params()->set_priority_count(PriorityCount);
  return *this;
}
size_t ex_cpu::priority_count() { return PRIORITY_COUNT; }
#endif

ex_cpu& ex_cpu::set_thread_count(size_t ThreadCount) {
  set_init_params()->set_thread_count(ThreadCount);
  return *this;
}

ex_cpu& ex_cpu::set_thread_init_hook(std::function<void(size_t)> Hook) {
  set_init_params()->set_thread_init_hook(Hook);
  return *this;
}

ex_cpu& ex_cpu::set_thread_teardown_hook(std::function<void(size_t)> Hook) {
  set_init_params()->set_thread_teardown_hook(Hook);
  return *this;
}

#ifdef TMC_USE_HWLOC
ex_cpu& ex_cpu::set_thread_init_hook(
  std::function<void(tmc::topology::thread_info)> Hook
) {
  set_init_params()->set_thread_init_hook(Hook);
  return *this;
}

ex_cpu& ex_cpu::set_thread_teardown_hook(
  std::function<void(tmc::topology::thread_info)> Hook
) {
  set_init_params()->set_thread_teardown_hook(Hook);
  return *this;
}
#endif

ex_cpu& ex_cpu::set_spins(size_t Spins) {
  set_init_params()->set_spins(Spins);
  return *this;
}

ex_cpu&
ex_cpu::set_work_stealing_strategy(tmc::work_stealing_strategy Strategy) {
  set_init_params()->set_work_stealing_strategy(Strategy);
  return *this;
}

size_t ex_cpu::thread_count() { return threads.size(); }

void ex_cpu::teardown() {
  bool expected = true;
  if (!initialized.compare_exchange_strong(expected, false)) {
    return;
  }

  for (size_t i = 0; i < threads.size(); ++i) {
    thread_stoppers[i].request_stop();
    thread_states[i].sleep_wait.fetch_add(1, std::memory_order_seq_cst);
    thread_states[i].sleep_wait.notify_one();
  }
  for (size_t i = 0; i < threads.size(); ++i) {
    threads[i].join();
  }
  while (ref_count.load() > 0) {
    TMC_CPU_PAUSE();
  }
  threads.clear();
  thread_stoppers.clear();
  inboxes.clear();

  threads_by_priority_bitset.clear();
  waker_matrix.clear();
#ifdef TMC_USE_HWLOC
  external_waker_list.clear();
#endif

  for (size_t i = 0; i < work_queues.size(); ++i) {
    delete[] work_queues[i].staticProducers;
  }
  work_queues.clear();
  if (task_stopper_bitsets != nullptr) {
    delete[] task_stopper_bitsets;
  }
  if (thread_states != nullptr) {
    delete[] thread_states;
  }
}

ex_cpu::~ex_cpu() { teardown(); }

std::coroutine_handle<>
ex_cpu::task_enter_context(std::coroutine_handle<> Outer, size_t Priority) {
  if (tmc::detail::this_thread::exec_prio_is(&type_erased_this, Priority)) {
    return Outer;
  } else {
    post(static_cast<std::coroutine_handle<>&&>(Outer), Priority);
    return std::noop_coroutine();
  }
}

namespace detail {

void executor_traits<tmc::ex_cpu>::post(
  tmc::ex_cpu& ex, tmc::work_item&& Item, size_t Priority, size_t ThreadHint
) {
  ex.post(static_cast<tmc::work_item&&>(Item), Priority, ThreadHint);
}

tmc::ex_any* executor_traits<tmc::ex_cpu>::type_erased(tmc::ex_cpu& ex) {
  return ex.type_erased();
}

std::coroutine_handle<> executor_traits<tmc::ex_cpu>::task_enter_context(
  tmc::ex_cpu& ex, std::coroutine_handle<> Outer, size_t Priority
) {
  return ex.task_enter_context(Outer, Priority);
}

tmc::task<void> client_main_awaiter(
  tmc::task<int> ClientMainTask, std::atomic<int>* ExitCode_out
) {
  int exitCode = co_await static_cast<tmc::task<int>&&>(
    ClientMainTask.resume_on(tmc::cpu_executor())
  );
  ExitCode_out->store(exitCode);
  ExitCode_out->notify_all();
}
} // namespace detail
int async_main(tmc::task<int>&& ClientMainTask) {
  // if the user already called init(), this will do nothing
  tmc::cpu_executor().init();
  std::atomic<int> exitCode(INT_MIN);
  tmc::post(
    tmc::cpu_executor(),
    tmc::detail::client_main_awaiter(
      static_cast<tmc::task<int>&&>(ClientMainTask), &exitCode
    ),
    0, 0
  );
  exitCode.wait(INT_MIN);
  return exitCode.load();
}
} // namespace tmc
