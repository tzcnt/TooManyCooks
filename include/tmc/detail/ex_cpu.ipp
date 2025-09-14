// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

// Implementation definition file for tmc::ex_cpu. This will be included
// anywhere TMC_IMPL is defined. If you prefer to manually separate compilation
// units, you can instead include this file directly in a CPP file.

#include "tmc/current.hpp"
#include "tmc/detail/compat.hpp"
#include "tmc/detail/qu_lockfree.hpp"
#include "tmc/detail/thread_layout.hpp"
#include "tmc/detail/thread_locals.hpp"
#include "tmc/ex_any.hpp"
#include "tmc/ex_cpu.hpp"
#include "tmc/sync.hpp"
#include "tmc/work_item.hpp"

#include <bit>
#include <coroutine>

#ifdef TMC_USE_HWLOC
#include <hwloc.h>
static_assert(sizeof(void*) == sizeof(hwloc_topology_t));
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

size_t* ex_cpu::wake_nearby_thread_order(size_t ThreadIdx) {
  return waker_matrix.data() + ThreadIdx * thread_count();
}

void ex_cpu::notify_n(
  size_t Count, size_t Priority, size_t ThreadHint, bool FromExecThread,
  bool FromPost
) {
  size_t spinningThreads = 0;
  size_t workingThreads = 0;
  if (ThreadHint < thread_count()) {
    size_t* neighbors = wake_nearby_thread_order(ThreadHint);
    size_t groupSize = thread_states[ThreadHint].group_size;
    for (size_t i = 0; i < groupSize; ++i) {
      size_t slot = neighbors[i];
      size_t bit = TMC_ONE_BIT << slot;
      thread_states[slot].sleep_wait.fetch_add(1, std::memory_order_seq_cst);
      spinningThreads = spinning_threads_bitset.load(std::memory_order_relaxed);
      workingThreads = working_threads_bitset.load(std::memory_order_relaxed);
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
    spinningThreads = spinning_threads_bitset.load(std::memory_order_relaxed);
    workingThreads = working_threads_bitset.load(std::memory_order_relaxed);
  }
  size_t spinningThreadCount = std::popcount(spinningThreads);
  size_t workingThreadCount = std::popcount(workingThreads);
  size_t spinningOrWorkingThreads = workingThreads | spinningThreads;
  size_t sleepingThreadCount =
    thread_count() - std::popcount(spinningOrWorkingThreads);
#ifdef TMC_PRIORITY_COUNT
  if constexpr (PRIORITY_COUNT > 1)
#else
  if (PRIORITY_COUNT > 1)
#endif
  {
    // if available threads can take all tasks, no need to interrupt
    if (sleepingThreadCount < Count && workingThreadCount != 0) {
      size_t interruptCount = 0;
      size_t interruptMax = Count - sleepingThreadCount;
      if (workingThreadCount < interruptMax) {
        interruptMax = workingThreadCount;
      }
      for (size_t prio = PRIORITY_COUNT - 1; prio > Priority; --prio) {
        size_t set = task_stopper_bitsets[prio].load(std::memory_order_acquire);
        while (set != 0) {
          size_t slot = std::countr_zero(set);
          set = set & ~(TMC_ONE_BIT << slot);
          auto currentPrio =
            thread_states[slot].yield_priority.load(std::memory_order_relaxed);

          // 2 threads may request a task to yield at the same time. The thread
          // with the higher priority (lower priority index) should prevail.
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
      // Currently, Count is not read after this point so this is not necessary
      // INTERRUPT_DONE:
      //   Count -= interruptCount;
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

    size_t sleepingThreads = ~spinningOrWorkingThreads;
    size_t* threadsWakeList;
    size_t base = 0;
    if (FromExecThread) {
      // Index 0 is this thread, which is already awake, so start at index 1
      threadsWakeList = 1 + wake_nearby_thread_order(current_thread_index());
    } else {
#ifdef TMC_USE_HWLOC
      if (sleepingThreadCount == thread_count()) {
        // All executor threads are sleeping; wake a thread that is bound to a
        // CPU near the currently executing non-executor thread.
        hwloc_cpuset_t set = hwloc_bitmap_alloc();
        if (set != nullptr) {
          auto topo = static_cast<hwloc_topology_t>(topology);
          if (0 ==
              hwloc_get_last_cpu_location(topo, set, HWLOC_CPUBIND_THREAD)) {
            unsigned int i = hwloc_bitmap_first(set);
            auto pu = hwloc_get_pu_obj_by_os_index(topo, i);
            base = pu_to_thread[pu->logical_index];
          }
          hwloc_bitmap_free(set);
        }
        threadsWakeList = wake_nearby_thread_order(base);
      } else {
        // Choose a working thread and try to wake a thread near it
        base = std::countr_zero(spinningOrWorkingThreads);
        threadsWakeList = 1 + wake_nearby_thread_order(base);
      }
#else
      // Treat thread bitmap as a stack - OS can balance them as needed
      base = std::countr_zero(sleepingThreads);
      threadsWakeList = wake_nearby_thread_order(base);
#endif
    }
    // Wake exactly 1 thread
    for (size_t i = 0;; ++i) {
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

void ex_cpu::init_queue_iteration_order(std::vector<size_t> const& Forward) {
  const size_t size = Forward.size();
  const size_t slot = Forward[0];

  // Forward has the order in which we should look to steal work.
  // An additional is entry inserted at index 1 to cache the
  // most-recently-stolen-from producer.
  size_t dequeueCount = size + 1;
  task_queue_t::ExplicitProducer** producers =
    new task_queue_t::ExplicitProducer*[PRIORITY_COUNT * dequeueCount];
  for (size_t prio = 0; prio < PRIORITY_COUNT; ++prio) {
    size_t pidx = prio * dequeueCount;
    // pointer to this thread's producer
    producers[pidx] = &work_queues[prio].staticProducers[slot];
    ++pidx;
    // pointer to previously consumed-from producer (initially none)
    producers[pidx] = nullptr;
    ++pidx;

    for (size_t i = 1; i < size; ++i) {
      task_queue_t::ExplicitProducer* prod =
        &work_queues[prio].staticProducers[Forward[i]];
      producers[pidx] = prod;
      ++pidx;
    }
  }
  tmc::detail::this_thread::producers = producers;
}

void ex_cpu::init_thread_locals(size_t Slot) {
  tmc::detail::this_thread::executor = &type_erased_this;
  tmc::detail::this_thread::this_task = {
    .prio = 0, .yield_priority = &thread_states[Slot].yield_priority
  };
  tmc::detail::this_thread::thread_index = Slot;
  if (init_params != nullptr && init_params->thread_init_hook != nullptr) {
    init_params->thread_init_hook(Slot);
  }
}

void ex_cpu::clear_thread_locals() {
  tmc::detail::this_thread::executor = nullptr;
  tmc::detail::this_thread::this_task = {};
}

void ex_cpu::run_one(
  tmc::work_item& item, const size_t Slot, const size_t Prio,
  size_t& PrevPriority, bool& WasSpinning
) {
  if (WasSpinning) {
    WasSpinning = false;
    set_work(Slot);
    clr_spin(Slot);
    // Wake 1 nearest neighbor. Don't priority-preempt any running tasks
    notify_n(1, PRIORITY_COUNT, NO_HINT, true, false);
  }
#ifdef TMC_PRIORITY_COUNT
  if constexpr (PRIORITY_COUNT > 1)
#else
  if (PRIORITY_COUNT > 1)
#endif
  {
    if (Prio != PrevPriority) {
      tmc::detail::this_thread::this_task.yield_priority->store(
        Prio, std::memory_order_release
      );
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

  item();
  assert(
    Prio == tmc::current_priority() &&
    "Tasks should not modify the priority directly. Use tmc::change_priority() "
    "or .with_priority() instead."
  );
}

// returns true if no tasks were found (caller should wait on cv)
// returns false if thread stop requested (caller should exit)
bool ex_cpu::try_run_some(
  std::stop_token& ThreadStopToken, const size_t Slot, size_t& PrevPriority
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
    if (work_queues[0].try_dequeue_ex_cpu_private(item, 0)) [[likely]] {
      run_one(item, Slot, 0, PrevPriority, wasSpinning);
      goto TOP;
    }

    // Inbox may retrieve items with out of order priority
    size_t inbox_prio;
    if (thread_states[Slot].inbox->try_pull(item, inbox_prio)) {
      run_one(item, Slot, inbox_prio, PrevPriority, wasSpinning);
      goto TOP;
    }

    if (work_queues[0].try_dequeue_ex_cpu_steal(item, 0)) {
      run_one(item, Slot, 0, PrevPriority, wasSpinning);
      goto TOP;
    }

    // Now check lower priority queues
    for (size_t prio = 1; prio < PRIORITY_COUNT; ++prio) {
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
  if (ThreadHint < thread_count()) {
    if (thread_states[ThreadHint].inbox->try_push(
          static_cast<work_item&&>(Item), Priority
        )) {
      if (!fromExecThread || ThreadHint != tmc::current_thread_index()) {
        notify_n(1, Priority, ThreadHint, fromExecThread, true);
      }
      return;
    }
  }

  if (fromExecThread) {
    work_queues[Priority].enqueue_ex_cpu(
      static_cast<work_item&&>(Item), Priority
    );
  } else {
    work_queues[Priority].enqueue(static_cast<work_item&&>(Item));
  }
  notify_n(1, Priority, NO_HINT, fromExecThread, true);
}

tmc::ex_any* ex_cpu::type_erased() { return &type_erased_this; }

// Default constructor does not call init() - you need to do it afterward
ex_cpu::ex_cpu()
    : init_params{nullptr}, type_erased_this(this), thread_stoppers{},
      task_stopper_bitsets{nullptr}, thread_states{nullptr}
#ifndef TMC_PRIORITY_COUNT
      ,
      PRIORITY_COUNT{1}
#endif
{
  initialized.store(false, std::memory_order_seq_cst);
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

  std::vector<tmc::detail::L3CacheSet> groupedCores;
#ifndef TMC_USE_HWLOC
  {
    size_t nthreads;
    if (init_params != nullptr && init_params->thread_count != 0) {
      nthreads = init_params->thread_count;
    } else {
      // limited to 32/64 threads for now, due to use of size_t bitset
      nthreads = std::thread::hardware_concurrency();
      if (nthreads > TMC_PLATFORM_BITS) {
        nthreads = TMC_PLATFORM_BITS;
      }
    }
    // Treat all cores as part of the same group
    groupedCores.push_back(
      tmc::detail::L3CacheSet{nullptr, nthreads, std::vector<size_t>{}}
    );
  }
#else
  hwloc_topology_t topo;
  hwloc_topology_init(&topo);
  hwloc_topology_load(topo);
  topology = topo;
  groupedCores = tmc::detail::group_cores_by_l3c(topo);
  bool lasso;
  pu_to_thread = tmc::detail::adjust_thread_groups(
    init_params == nullptr ? 0 : init_params->thread_count,
    init_params == nullptr ? 0.0f : init_params->thread_occupancy, groupedCores,
    lasso
  );
#endif
  {
    size_t totalThreadCount = 0;
    for (size_t i = 0; i < groupedCores.size(); ++i) {
      totalThreadCount += groupedCores[i].group_size;
    }
    threads.resize(totalThreadCount);
  }
  inboxes.resize(groupedCores.size());
  inboxes.fill_default();
  // Steal matrix is sliced up and shared with each thread.
  // Waker matrix is kept as a member so it can be accessed by any thread.
  std::vector<size_t> steal_matrix = detail::get_lattice_matrix(groupedCores);
  waker_matrix = detail::invert_matrix(steal_matrix, thread_count());

  assert(thread_count() != 0);
  // limited to 32/64 threads for now, due to use of size_t bitset
  assert(thread_count() <= TMC_PLATFORM_BITS);

  work_queues.resize(PRIORITY_COUNT);
  for (size_t i = 0; i < PRIORITY_COUNT; ++i) {
    work_queues.emplace_at(i, thread_count() + 1);
  }

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

  for (size_t prio = 0; prio < PRIORITY_COUNT; ++prio) {
    work_queues[prio].staticProducers =
      new task_queue_t::ExplicitProducer[thread_count()];
    for (size_t i = 0; i < thread_count(); ++i) {
      work_queues[prio].staticProducers[i].init(&work_queues[prio]);
    }
    work_queues[prio].dequeueProducerCount = thread_count() + 1;
  }
  std::function<void(size_t)> thread_teardown_hook = nullptr;
  if (init_params != nullptr && init_params->thread_teardown_hook != nullptr) {
    thread_teardown_hook = init_params->thread_teardown_hook;
  }

  std::atomic<int> initThreadsBarrier(static_cast<int>(thread_count()));
  std::atomic_thread_fence(std::memory_order_seq_cst);

  size_t slot = 0;
  for (size_t groupIdx = 0; groupIdx < groupedCores.size(); ++groupIdx) {
    auto& coreGroup = groupedCores[groupIdx];
    size_t groupSize = coreGroup.group_size;
    for (size_t subIdx = 0; subIdx < groupSize; ++subIdx) {
      thread_states[slot].group_size = groupSize;
      thread_states[slot].inbox = &inboxes[groupIdx];
#ifdef TMC_USE_HWLOC
      auto sharedCores =
        hwloc_bitmap_dup(static_cast<hwloc_obj_t>(coreGroup.l3cache)->cpuset);
#endif

      // TODO pull this out into a separate struct
      threads.emplace_at(
        slot,
        [
#ifdef TMC_USE_HWLOC
          sharedCores, lasso,
#endif
          this,
          stealOrder = detail::slice_matrix(steal_matrix, thread_count(), slot),
          slot, thread_teardown_hook,
          barrier = &initThreadsBarrier](std::stop_token thread_stop_token) {
          // Ensure this thread sees all non-atomic read-only values
          tmc::detail::memory_barrier();
          init_thread_locals(slot);
#ifdef TMC_USE_HWLOC
          if (lasso) {
            tmc::detail::bind_thread(
              static_cast<hwloc_topology_t>(topology), sharedCores
            );
          }
          hwloc_bitmap_free(sharedCores);
#endif
          init_queue_iteration_order(stealOrder);
          barrier->fetch_sub(1);
          barrier->notify_all();
          size_t previousPrio = NO_TASK_RUNNING;
        TOP:
          while (try_run_some(thread_stop_token, slot, previousPrio)) {
            size_t spinningThreads = set_spin(slot);
            size_t workingThreads = clr_work(slot);

            // Limit the number of spinning threads to half the number of
            // working threads. This prevents too many spinners in a lightly
            // loaded system.
            size_t spinningThreadCount = std::popcount(spinningThreads) + 1;
            size_t workingThreadCount = std::popcount(workingThreads) - 1;
            if (2 * spinningThreadCount <= workingThreadCount) {
              for (size_t i = 0; i < 4; ++i) {
                TMC_CPU_PAUSE();
                if (!thread_states[slot].inbox->empty()) {
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
                  ~(TMC_ONE_BIT << slot), std::memory_order_acq_rel
                );
              }
            }
            previousPrio = NO_TASK_RUNNING;

            // Transition from spinning to sleeping.
            int waitValue =
              thread_states[slot].sleep_wait.load(std::memory_order_relaxed);
            clr_spin(slot);
            tmc::detail::memory_barrier(); // pairs with barrier in notify_n

            // Double check that the queue is empty after the memory
            // barrier. In combination with the inverse double-check in
            // notify_n, this prevents any lost wakeups.
            if (!thread_states[slot].inbox->empty()) {
              set_spin(slot);
              goto TOP;
            }
            for (size_t prio = 0; prio < PRIORITY_COUNT; ++prio) {
              if (!work_queues[prio].empty()) {
                set_spin(slot);
                goto TOP;
              }
            }

            // No work found. Go to sleep.
            if (thread_stop_token.stop_requested()) [[unlikely]] {
              break;
            }
            thread_states[slot].sleep_wait.wait(waitValue);
            set_spin(slot);
          }

          // Thread stop has been requested (executor is shutting down)
          working_threads_bitset.fetch_and(~(TMC_ONE_BIT << slot));
          if (thread_teardown_hook != nullptr) {
            thread_teardown_hook(slot);
          }
          clear_thread_locals();
          delete[] static_cast<task_queue_t::ExplicitProducer**>(
            tmc::detail::this_thread::producers
          );
          tmc::detail::this_thread::producers = nullptr;
        }
      );
      thread_stoppers.emplace_at(slot, threads[slot].get_stop_source());
      ++slot;
    }
  }
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

#ifndef TMC_PRIORITY_COUNT
ex_cpu& ex_cpu::set_priority_count(size_t PriorityCount) {
  assert(!is_initialized());
  assert(PriorityCount <= 16 && "The maximum number of priority levels is 16.");
  if (PriorityCount > 16) {
    PriorityCount = 16;
  }
  if (init_params == nullptr) {
    init_params = new InitParams;
  }
  init_params->priority_count = PriorityCount;
  return *this;
}
size_t ex_cpu::priority_count() { return PRIORITY_COUNT; }
#endif
#ifdef TMC_USE_HWLOC
ex_cpu& ex_cpu::set_thread_occupancy(float ThreadOccupancy) {
  assert(!is_initialized());
  if (init_params == nullptr) {
    init_params = new InitParams;
  }
  init_params->thread_occupancy = ThreadOccupancy;
  return *this;
}
#endif

ex_cpu& ex_cpu::set_thread_count(size_t ThreadCount) {
  assert(!is_initialized());
  // limited to 32/64 threads for now, due to use of size_t bitset
  assert(ThreadCount <= TMC_PLATFORM_BITS);
  if (init_params == nullptr) {
    init_params = new InitParams;
  }
  init_params->thread_count = ThreadCount;
  return *this;
}

ex_cpu& ex_cpu::set_thread_init_hook(std::function<void(size_t)> Hook) {
  assert(!is_initialized());
  if (init_params == nullptr) {
    init_params = new InitParams;
  }
  init_params->thread_init_hook =
    static_cast<std::function<void(size_t)>&&>(Hook);
  return *this;
}

ex_cpu& ex_cpu::set_thread_teardown_hook(std::function<void(size_t)> Hook) {
  assert(!is_initialized());
  if (init_params == nullptr) {
    init_params = new InitParams;
  }
  init_params->thread_teardown_hook =
    static_cast<std::function<void(size_t)>&&>(Hook);
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
  threads.clear();
  thread_stoppers.clear();
  inboxes.clear();

#ifdef TMC_USE_HWLOC
  pu_to_thread.clear();
  hwloc_topology_destroy(static_cast<hwloc_topology_t>(topology));
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
  std::atomic<int> exitCode(std::numeric_limits<int>::min());
  post(
    tmc::cpu_executor(),
    tmc::detail::client_main_awaiter(
      static_cast<tmc::task<int>&&>(ClientMainTask), &exitCode
    ),
    0
  );
  exitCode.wait(std::numeric_limits<int>::min());
  return exitCode.load();
}
} // namespace tmc
