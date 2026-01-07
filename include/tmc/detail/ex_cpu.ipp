// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

// Implementation definition file for tmc::ex_cpu. This will be included
// anywhere TMC_IMPL is defined. If you prefer to manually separate compilation
// units, you can instead include this file directly in a CPP file.

#include "tmc/current.hpp"
#include "tmc/detail/bit_manip.hpp"
#include "tmc/detail/compat.hpp"
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

#include <climits>
#include <coroutine>

#ifdef TMC_USE_HWLOC
#include <hwloc.h>
static_assert(sizeof(void*) == sizeof(hwloc_topology_t));
static_assert(sizeof(void*) == sizeof(hwloc_bitmap_t));
#else
// With hwloc these limits are configured in thread_layout.ipp
// Without, they are configured here
#include "tmc/detail/container_cpu_quota.hpp"
#endif

#ifdef TMC_DEBUG_THREAD_CREATION
#include <cstdio>
#endif

namespace tmc {

bool ex_cpu::is_initialized() {
  return initialized.load(std::memory_order_relaxed);
}

void ex_cpu::notify_hint(size_t Priority, size_t ThreadHint) {
  size_t* neighbors = waker_matrix[Priority].get_row(ThreadHint);
  // Only wake threads in the target thread's group.
  size_t groupSize = thread_states[ThreadHint].group_size;
  if (waker_matrix[Priority].cols < groupSize) {
    // If there's weird priority partitions, the group may have more threads
    // than the priority partition. Currently this can only happen if the thread
    // pinning level is CORE.
    groupSize = waker_matrix[Priority].cols;
  }

#ifndef TMC_MORE_THREADS
  size_t workingThreads =
    working_threads_bitset.word.load(std::memory_order_relaxed);
#endif
  for (size_t i = 0; i < groupSize; ++i) {
    size_t slot = neighbors[i];
// Always try to wake a thread in the group, regardless of spinner limit.
#ifdef TMC_MORE_THREADS
    if (!working_threads_bitset.test_bit(slot, std::memory_order_relaxed))
#else
    if ((workingThreads & (TMC_ONE_BIT << slot)) == 0)
#endif
    {
      // TODO investigate setting thread as spinning before waking it,
      // so that multiple concurrent wakers don't syscall. However, with the
      // current design, this can cause lost wakeups.
      // spinning_threads_bitset.set_bit(slot,
      // std::memory_order_release);
      thread_states[slot].sleep_wait.fetch_add(1, std::memory_order_seq_cst);
      thread_states[slot].sleep_wait.notify_one();
      return;
    }
  }
  // All threads in the group appear to be working. However, to prevent lost
  // wakeups, we must ensure that at least one wait var is incremented.
  // The alternative would be to always fetch_add before checking
  // working_threads_bitset, and then only conditionally notify_one afterward.
  thread_states[ThreadHint].sleep_wait.fetch_add(1, std::memory_order_seq_cst);
  thread_states[ThreadHint].sleep_wait.notify_one();
}

void ex_cpu::notify_n(
  size_t Count, size_t Priority, bool AllowedPriority, bool FromPost
) {
#ifdef TMC_MORE_THREADS
  const tmc::detail::bitmap& allowedThreads =
    threads_by_priority_bitset[Priority];
  size_t wordCount = allowedThreads.get_word_count();
  assert(wordCount == spinning_threads_bitset.get_word_count());
  assert(wordCount == working_threads_bitset.get_word_count());

  size_t spinningThreadCount = 0;
  size_t workingThreadCount = 0;
  size_t spinningOrWorkingAllowedCount = 0;
  for (size_t i = 0; i < wordCount; ++i) {
    size_t mask = allowedThreads.load_word(i);
    size_t spinning =
      spinning_threads_bitset.words[i].load(std::memory_order_relaxed) & mask;
    size_t working =
      working_threads_bitset.words[i].load(std::memory_order_relaxed) & mask;
    if (i + 1 == wordCount) {
      size_t finalMask = spinning_threads_bitset.valid_mask_for_word(i);
      spinning &= finalMask;
      working &= finalMask;
    }
    size_t spinningOrWorking = spinning | working;
    spinningThreadCount += tmc::detail::popcnt(spinning);
    workingThreadCount += tmc::detail::popcnt(working);
    spinningOrWorkingAllowedCount += tmc::detail::popcnt(spinningOrWorking);
  }
#else
  const size_t allowedThreads = threads_by_priority_bitset[Priority].word;

  size_t spinningThreads =
    spinning_threads_bitset.word.load(std::memory_order_relaxed) &
    allowedThreads;
  size_t workingThreads =
    working_threads_bitset.word.load(std::memory_order_relaxed) &
    allowedThreads;
  size_t spinningOrWorkingThreads = workingThreads | spinningThreads;
  size_t sleepingThreads = (~spinningOrWorkingThreads) & allowedThreads;

  size_t spinningThreadCount = tmc::detail::popcnt(spinningThreads);
  size_t workingThreadCount = tmc::detail::popcnt(workingThreads);
  size_t spinningOrWorkingAllowedCount =
    tmc::detail::popcnt(spinningOrWorkingThreads);
#endif
  size_t sleepingThreadCount =
    waker_matrix[Priority].cols - spinningOrWorkingAllowedCount;
  if TMC_PRIORITY_CONSTEXPR (PRIORITY_COUNT > 1) {
    if (FromPost) {
      // if available threads can take all tasks, no need to interrupt
      if (sleepingThreadCount < Count && workingThreadCount != 0) {
        size_t interruptCount = 0;
        size_t interruptMax = Count - sleepingThreadCount;
        if (workingThreadCount < interruptMax) {
          interruptMax = workingThreadCount;
        }
        for (size_t prio = PRIORITY_COUNT - 1; prio > Priority; --prio) {
          for (size_t wordIdx = 0;
               wordIdx < task_stopper_bitsets[prio].get_word_count();
               ++wordIdx) {
#ifdef TMC_MORE_THREADS
            size_t set = task_stopper_bitsets[prio].load_word(
              wordIdx, std::memory_order_acquire
            );
#else
            size_t set =
              task_stopper_bitsets[prio].word.load(std::memory_order_acquire);
#endif
            while (set != 0) {
              size_t bit_offset = tmc::detail::tzcnt(set);
              size_t slot = wordIdx * TMC_PLATFORM_BITS + bit_offset;
              set = set & ~(TMC_ONE_BIT << bit_offset);
              auto currentPrio = thread_states[slot].yield_priority.load(
                std::memory_order_relaxed
              );

              // 2 threads may request a task to yield at the same time.
              // The thread with the higher priority (lower priority index)
              // should prevail.
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
        }
        // Currently, Count is not read after this point so this is not
        // necessary INTERRUPT_DONE:
        //   Count -= interruptCount;
      }
    }
  }
INTERRUPT_DONE:

  if (AllowedPriority) [[likely]] {
    // We're allowed to return early IFF this thread is part of the working
    // priority group. That's because these checks are inherently racy and we
    // don't want to get soft locked by a lost wakeup.

    // All threads are spinning or working.
#ifdef TMC_MORE_THREADS
    if (sleepingThreadCount == 0)
#else
    if (sleepingThreads == 0)
#endif
      [[likely]] {
      return;
    }

    // Don't wake threads rapidly when posting - prefer to wake them slowly in
    // try_run_some(). This spreads out the wakeup overhead between threads.
    if (FromPost && spinningThreadCount != 0) {
      return;
    }

    // Limit the number of spinning threads to half the number of
    // working threads. This prevents too many spinners in a lightly
    // loaded system.
    if (spinningThreadCount * 2 > workingThreadCount) {
      return;
    }

    // Wake exactly 1 thread. Prefer a thread that's running near this thread,
    // to maximize work-stealing efficiency.
    size_t* threadsWakeList =
      waker_matrix[Priority].get_row(current_thread_index());
    for (size_t i = 1; i < waker_matrix[Priority].cols; ++i) {
      size_t slot = threadsWakeList[i];
      assert(slot < thread_count());
#ifdef TMC_MORE_THREADS
      if (!working_threads_bitset.test_bit(slot, std::memory_order_relaxed) &&
          !spinning_threads_bitset.test_bit(slot, std::memory_order_relaxed))
#else
      if ((spinningOrWorkingThreads & (TMC_ONE_BIT << slot)) == 0)
#endif
      {
        thread_states[slot].sleep_wait.fetch_add(1, std::memory_order_acq_rel);
        thread_states[slot].sleep_wait.notify_one();
        return;
      }
    }
  } else {
    size_t slot;
#ifdef TMC_MORE_THREADS
    if (sleepingThreadCount != 0 && spinningThreadCount == 0) {
      // Find first sleeping thread.
      // This might not find an allowed thread as the base, but the
      // waker_matrix redirects to an allowed thread anyway.
      size_t base = 0;
      for (size_t wordIdx = 0;
           wordIdx < spinning_threads_bitset.get_word_count(); ++wordIdx) {
        size_t sleeping_word = spinning_threads_bitset.load_inverted_or(
          working_threads_bitset, wordIdx, std::memory_order_relaxed
        );
        if (sleeping_word != 0) {
          base =
            wordIdx * TMC_PLATFORM_BITS + tmc::detail::tzcnt(sleeping_word);
          break;
        }
      }
      assert(base < thread_count());
      slot = waker_matrix[Priority].get_row(base)[0];
    }
#else
    if (sleepingThreads != 0 && spinningThreadCount == 0) {
      slot = tmc::detail::tzcnt(sleepingThreads);
    }
#endif
    else {
      // If we get here, no sleeping thread was found, and this thread isn't
      // part of the allowed priority group. Wake 1 thread to prevent lost
      // wakeups.
      slot = waker_matrix[Priority].get_row(0)[0];
    }
    thread_states[slot].sleep_wait.fetch_add(1, std::memory_order_acq_rel);
    thread_states[slot].sleep_wait.notify_one();
  }
}

ex_cpu::task_queue_t::ExplicitProducer** ex_cpu::init_queue_iteration_order(
  std::vector<std::vector<size_t>> const& Forward
) {
  // Calculate total size based on the global producerArrayOffset layout.
  // Each priority level contributes (dequeueProducerCount + 1) slots.
  // The array must be full-sized so that producerArrayOffset indexing works
  // correctly, even if this thread doesn't handle all priorities.
  // Layout per priority: [self, cached, others...]
  size_t totalSize = 0;
  for (size_t prio = 0; prio < PRIORITY_COUNT; ++prio) {
    totalSize += work_queues[prio].dequeueProducerCount + 1;
  }

  if (totalSize == 0) {
    return nullptr;
  }

  task_queue_t::ExplicitProducer** producers =
    new task_queue_t::ExplicitProducer*[totalSize];

  for (size_t prio = 0; prio < PRIORITY_COUNT; ++prio) {
    size_t offset = work_queues[prio].producerArrayOffset;
    size_t slotCount = work_queues[prio].dequeueProducerCount + 1;

    if (Forward[prio].empty()) {
      // This thread doesn't handle this priority - null out all slots
      for (size_t i = 0; i < slotCount; ++i) {
        producers[offset + i] = nullptr;
      }
      continue;
    }

    auto& thisMatrix = Forward[prio];
    // pointer to this thread's producer
    producers[offset] = &work_queues[prio].staticProducers[thisMatrix[0]];
    // pointer to previously consumed-from producer (initially none)
    producers[offset + 1] = nullptr;

    for (size_t i = 1; i < Forward[prio].size(); ++i) {
      producers[offset + 1 + i] =
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
  size_t& PrevPriority, bool& Spinning
) {
  if (Spinning) [[unlikely]] {
    Spinning = false;
    working_threads_bitset.set_bit(Slot);
    spinning_threads_bitset.clr_bit(Slot);
    // Wake 1 nearest neighbor. Don't priority-preempt any running tasks
    notify_n(1, Prio, true, false);
  }
  if TMC_PRIORITY_CONSTEXPR (PRIORITY_COUNT > 1) {
    thread_states[Slot].yield_priority.store(Prio, std::memory_order_release);
    if (Prio != PrevPriority) {
      if (PrevPriority != NO_TASK_RUNNING) {
        task_stopper_bitsets[PrevPriority].clr_bit(
          Slot, std::memory_order_acq_rel
        );
      }
      task_stopper_bitsets[Prio].set_bit(Slot, std::memory_order_acq_rel);
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
TMC_FORCE_INLINE inline bool ex_cpu::try_run_some(
  std::stop_token& ThreadStopToken, const size_t Slot,
  const size_t PriorityRangeBegin, const size_t PriorityRangeEnd,
  size_t& PrevPriority, bool& Spinning
) {
  size_t idxBase = work_queues[PriorityRangeBegin].producerArrayOffset;
TOP:
  if (ThreadStopToken.stop_requested()) [[unlikely]] {
    return false;
  }
  work_item item;

  size_t idx = idxBase;
  // For priority 0, check private queue, then inbox, then try to steal
  // Lower priorities can just check private queue and steal - no inbox
  // Although this could be combined into the following loop (with an if
  // statement to remove the inbox check), it gives better codegen for the
  // fast path to keep it separate.
  if (work_queues[PriorityRangeBegin].try_dequeue_ex_cpu_private(item, idx))
    [[likely]] {
    run_one(item, Slot, PriorityRangeBegin, PrevPriority, Spinning);
    goto TOP;
  }

  // Inbox may retrieve items with out of order priority.
  // This also may allow threads to run work items that are outside of their
  // normally assigned priorities.
  size_t inbox_prio;
  if (thread_states[Slot].inbox->try_pull(item, inbox_prio)) {
    run_one(item, Slot, inbox_prio, PrevPriority, Spinning);
    goto TOP;
  }

  if (work_queues[PriorityRangeBegin].try_dequeue_ex_cpu_steal(item, idx))
    [[likely]] {
    run_one(item, Slot, PriorityRangeBegin, PrevPriority, Spinning);
    goto TOP;
  }

  // Now check lower priority queues
  for (size_t prio = PriorityRangeBegin + 1; prio < PriorityRangeEnd; ++prio) {
    if (work_queues[prio].try_dequeue_ex_cpu_private(item, idx)) {
      run_one(item, Slot, prio, PrevPriority, Spinning);
      goto TOP;
    }
    if (work_queues[prio].try_dequeue_ex_cpu_steal(item, idx)) {
      run_one(item, Slot, prio, PrevPriority, Spinning);
      goto TOP;
    }
  }
  return true;
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
  bool allowedPriority =
    fromExecThread &&
    threads_by_priority_bitset[Priority].test_bit(current_thread_index());

  if (!fromExecThread) {
    ++ref_count;
  }

  if (ThreadHint < thread_count()) [[unlikely]] {
    // Check allowed priority of the target thread, not the current thread
    if (threads_by_priority_bitset[Priority].test_bit(ThreadHint) &&
        thread_states[ThreadHint].inbox->try_push(
          static_cast<work_item&&>(Item), Priority
        )) {
      if (!fromExecThread || ThreadHint != tmc::current_thread_index()) {
        notify_hint(Priority, ThreadHint);
      }
      goto END; // This is necessary for optimal codegen
    }
  }
  if (fromExecThread && allowedPriority) [[likely]] {
    work_queues[Priority].enqueue_ex_cpu(static_cast<work_item&&>(Item));
  } else {
    work_queues[Priority].enqueue(static_cast<work_item&&>(Item));
  }
  notify_n(1, Priority, allowedPriority, true);

END:
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
  size_t PriorityRangeEnd, ex_cpu::task_queue_t::ExplicitProducer** StealOrder,
  std::atomic<tmc::detail::atomic_wait_t>& InitThreadsBarrier,
  // will be nullptr if hwloc is not enabled
  [[maybe_unused]] tmc::detail::hwloc_unique_bitmap& CpuSet,
  // will be nullptr if hwloc is not enabled
  [[maybe_unused]] void* HwlocTopo
) {
  std::function<void(tmc::topology::thread_info)> ThreadTeardownHook = nullptr;
  if (init_params != nullptr && init_params->thread_teardown_hook != nullptr) {
    ThreadTeardownHook = init_params->thread_teardown_hook;
  }

  return [this, StealOrder, Info, PriorityRangeBegin, PriorityRangeEnd,
          &InitThreadsBarrier, ThreadTeardownHook
#ifdef TMC_USE_HWLOC
          ,
          myCpuSet = CpuSet.clone(), HwlocTopo
#endif
  ](std::stop_token ThreadStopToken) mutable {
    size_t Slot = Info.index;

#ifdef TMC_USE_HWLOC
    if (myCpuSet != nullptr) {
      tmc::detail::pin_thread(
        static_cast<hwloc_topology_t>(HwlocTopo), myCpuSet, Info.group.cpu_kind
      );
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
    while (true) {
      bool spinning = true;
      if (!try_run_some(
            ThreadStopToken, Slot, PriorityRangeBegin, PriorityRangeEnd,
            previousPrio, spinning
          )) [[unlikely]] {
        break;
      }

      // Become spinning
      size_t spinningThreadCount;
      size_t workingThreadCount;
      if (spinning) {
        spinningThreadCount = spinning_threads_bitset.popcnt();
        workingThreadCount = working_threads_bitset.popcnt();
      } else {
        spinningThreadCount = spinning_threads_bitset.set_bit_popcnt(Slot);
        workingThreadCount = working_threads_bitset.clr_bit_popcnt(Slot);
      }

      // Limit the number of spinning threads to half the number of
      // working threads. This prevents too many spinners in a lightly
      // loaded system.
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

      if TMC_PRIORITY_CONSTEXPR (PRIORITY_COUNT > 1) {
        if (previousPrio != NO_TASK_RUNNING) {
          task_stopper_bitsets[previousPrio].clr_bit(
            Slot, std::memory_order_acq_rel
          );
        }
      }
      previousPrio = NO_TASK_RUNNING;

      // Transition from spinning to sleeping.
      auto waitValue =
        thread_states[Slot].sleep_wait.load(std::memory_order_relaxed);
      spinning_threads_bitset.clr_bit(Slot);

      // StoreLoad barrier to prevent lost wakeups
      tmc::detail::memory_barrier();

      // Double-check for work
      if (!thread_states[Slot].inbox->empty()) {
        spinning_threads_bitset.set_bit(Slot);
        goto TOP;
      }
      for (size_t prio = 0; prio < PRIORITY_COUNT; ++prio) {
        if (!work_queues[prio].empty()) {
          spinning_threads_bitset.set_bit(Slot);
          goto TOP;
        }
      }

      // No work found. Go to sleep.
      if (ThreadStopToken.stop_requested()) [[unlikely]] {
        break;
      }
      thread_states[Slot].sleep_wait.wait(waitValue);
      spinning_threads_bitset.set_bit(Slot);
    }

    // Thread stop has been requested (executor is shutting down)
    working_threads_bitset.clr_bit(Slot);
    if (ThreadTeardownHook != nullptr) {
      ThreadTeardownHook(Info);
    }

    clear_thread_locals();
    delete[] static_cast<task_queue_t::ExplicitProducer**>(
      tmc::detail::this_thread::producers
    );
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
  task_stopper_bitsets = new tmc::detail::atomic_bitmap[PRIORITY_COUNT];

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
#ifndef TMC_MORE_THREADS
      // limited to 32/64 threads for now, due to use of size_t bitset
      if (nthreads > TMC_PLATFORM_BITS) {
        nthreads = TMC_PLATFORM_BITS;
      }
#endif
    }
    groupedCores.push_back(
      tmc::topology::detail::CacheGroup{nullptr, 0, 0, {}, {}, nthreads, 0}
    );
  }
  void* topo = nullptr;
#else
  hwloc_topology_t topo;
  auto internalTopo = tmc::topology::detail::query_internal(topo);
  auto& groupedCores = internalTopo.groups;
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
#ifndef TMC_MORE_THREADS
  // limited to 32/64 threads due to use of size_t bitset
  assert(totalThreadCount <= TMC_PLATFORM_BITS);
#endif
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
  working_threads_bitset.init(thread_count());
  spinning_threads_bitset.init(thread_count());
  spinning_threads_bitset.set_first_n_bits(
    thread_count(), std::memory_order_relaxed
  );
  for (size_t prio = 0; prio < PRIORITY_COUNT; ++prio) {
    task_stopper_bitsets[prio].init(thread_count());
  }

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
  for (size_t prio = 0; prio < PRIORITY_COUNT; ++prio) {
    threads_by_priority_bitset[prio].init(thread_count());
  }

  // The set of thread indexes that are allowed to participate in each
  // priority level
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
      thread_states[slot].group_size = groupSize;
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
          threads_by_priority_bitset[prio].set_bit(slot);
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
  size_t cumulativeOffset = 0;
  for (size_t prio = 0; prio < PRIORITY_COUNT; ++prio) {
    size_t threadCount = tidsByPrio[prio].size();
    work_queues.emplace_at(prio, threadCount + 1);
    work_queues[prio].staticProducers =
      new task_queue_t::ExplicitProducer[threadCount];
    for (size_t i = 0; i < threadCount; ++i) {
      work_queues[prio].staticProducers[i].init(&work_queues[prio]);
    }
    work_queues[prio].dequeueProducerCount = threadCount;
    work_queues[prio].producerArrayOffset = cumulativeOffset;
    // add 1 extra space for cached producer
    cumulativeOffset += threadCount + 1;
  }

  std::vector<tmc::detail::Matrix> stealMatrixes;
  stealMatrixes.resize(PRIORITY_COUNT);
  waker_matrix.resize(PRIORITY_COUNT);

  for (size_t prio = 0; prio < PRIORITY_COUNT; ++prio) {
    // If there are no priority partitions, all of the steal/waker matrixes
    // can refer to the same underlying data.
    if (partitionCpusets.size() < 2 && prio > 0) {

      stealMatrixes[prio].set_weak_ref(stealMatrixes[0]);
      waker_matrix[prio].set_weak_ref(waker_matrix[0]);

#ifdef TMC_DEBUG_THREAD_CREATION
      if (prio == 1) {
        std::printf("All priorities share the same matrixes.\n");
      }
#endif
      continue;
    }

    // Steal matrix is sliced up and shared with each thread.
    if (init_params->strategy == work_stealing_strategy::LATTICE_MATRIX) {
      stealMatrixes[prio].init(
        detail::get_lattice_matrix(groupsByPrio[prio]), tidsByPrio[prio].size()
      );
    } else {
      stealMatrixes[prio].init(
        detail::get_hierarchical_matrix(groupsByPrio[prio]),
        tidsByPrio[prio].size()
      );
    }

    // Waker matrix is the inverse of the steal matrix
    tmc::detail::Matrix includedWakers = stealMatrixes[prio].to_wakers();

#ifdef TMC_DEBUG_THREAD_CREATION
    std::printf("ex_cpu priority %zu stealMatrix (local thread IDs):\n", prio);
    stealMatrixes[prio].print(nullptr);
#endif

    /*** CONSTRUCT WAKER MATRIXES ***/

    // Waker matrix is kept as a member so it can be accessed by any thread.
    waker_matrix[prio].init(
      TMC_ALL_ONES, thread_count(), tidsByPrio[prio].size()
    );

    // Step 1: For threads included in the priority group, copy rows from
    // their waker matrixes, but convert them from local to global thread
    // indexes.
    for (size_t localTid = 0; localTid < tidsByPrio[prio].size(); ++localTid) {
      size_t globalTid = tidsByPrio[prio][localTid];
      size_t* dstRow = waker_matrix[prio].get_row(globalTid);
      size_t* srcRow = includedWakers.get_row(localTid);
      for (size_t wakeIdx = 0; wakeIdx < tidsByPrio[prio].size(); ++wakeIdx) {
        auto localWakeTid = srcRow[wakeIdx];
        dstRow[wakeIdx] = tidsByPrio[prio][localWakeTid];
      }
    }

    // Step 2: For threads excluded from the priority group, fill in their
    // waker rows by copying from the included threads. This means that
    // excluded threads can only wake included threads when submitting work.
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

    assert(
      waker_matrix[prio].cols == threads_by_priority_bitset[prio].popcnt()
    );
#ifdef TMC_DEBUG_THREAD_CREATION
    std::printf(
      "ex_cpu priority %zu waker_matrix (global thread IDs):\n", prio
    );
    waker_matrix[prio].print(nullptr);
#endif
  }

  // Start the worker threads
  std::atomic<tmc::detail::atomic_wait_t> initThreadsBarrier(
    static_cast<int>(thread_count())
  );
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
              initThreadsBarrier, d.threadCpuset, topo
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

  for (size_t i = 0; i < work_queues.size(); ++i) {
    delete[] work_queues[i].staticProducers;
  }
  work_queues.clear();
  working_threads_bitset.clear();
  spinning_threads_bitset.clear();
  if (task_stopper_bitsets != nullptr) {
    delete[] task_stopper_bitsets;
    task_stopper_bitsets = nullptr;
  }
  if (thread_states != nullptr) {
    delete[] thread_states;
    thread_states = nullptr;
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
  tmc::task<int> ClientMainTask,
  std::atomic<tmc::detail::atomic_wait_t>* ExitCode_out
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
  std::atomic<tmc::detail::atomic_wait_t> exitCode(INT_MIN);
  tmc::post(
    tmc::cpu_executor(),
    tmc::detail::client_main_awaiter(
      static_cast<tmc::task<int>&&>(ClientMainTask), &exitCode
    ),
    0, 0
  );
  exitCode.wait(INT_MIN);
  return static_cast<int>(exitCode.load());
}
} // namespace tmc
