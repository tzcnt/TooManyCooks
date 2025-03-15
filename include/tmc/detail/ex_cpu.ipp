// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

// Implementation definition file for tmc::ex_cpu. This will be included
// anywhere TMC_IMPL is defined. If you prefer to manually separate compilation
// units, you can instead include this file directly in a CPP file.

#include "tmc/detail/compat.hpp"
#include "tmc/detail/qu_lockfree.hpp"
#include "tmc/detail/thread_layout.hpp"
#include "tmc/detail/thread_locals.hpp"
#include "tmc/ex_cpu.hpp"

#include <bit>

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

void ex_cpu::notify_n(
  size_t Count, size_t Priority, size_t ThreadHint, bool FromExecThread,
  bool FromPost
) {
  // As a performance optimization, we only try to wake when we know
  // there is at least 1 sleeping thread. In combination with the inverse
  // barrier/double-check in the main worker loop, prevents lost wakeups.
  // tmc::detail::memory_barrier();

  tmc::detail::memory_barrier();
  size_t spinningThreads =
    spinning_threads_bitset.load(std::memory_order_acquire);
  size_t workingThreads =
    working_threads_bitset.load(std::memory_order_acquire);

  if (ThreadHint != TMC_ALL_ONES) {
    size_t spinningOrWorking = spinningThreads | workingThreads;
    thread_states[ThreadHint].sleep_wait.fetch_add(
      1, std::memory_order_acq_rel
    );
    size_t hintBit = (TMC_ONE_BIT << ThreadHint);
    if ((spinningOrWorking & hintBit) == 0) {
      // TODO it would be nice to set thread as spinning before waking it -
      // so that multiple concurrent wakers don't syscall. However this can lead
      // to lost wakeups currently.
      // spinning_threads_bitset.fetch_or(hintBit, std::memory_order_release);
      thread_states[ThreadHint].sleep_wait.notify_one();
      return;
    }
  }
  ptrdiff_t spinningThreadCount = std::popcount(spinningThreads);
  ptrdiff_t workingThreadCount = std::popcount(workingThreads);
  ptrdiff_t sleepingThreadCount =
    thread_count() - (workingThreadCount + spinningThreadCount);
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
          if (thread_states[slot].yield_priority.load(std::memory_order_relaxed
              ) <= Priority) {
            continue;
          }
          auto oldPrio = thread_states[slot].yield_priority.exchange(
            Priority, std::memory_order_acq_rel
          );
          if (oldPrio < Priority) {
            // If the prior priority was higher than this one, put it back. This
            // is a race condition that is expected to occur very infrequently
            // if 2 tasks try to request the same thread to yield at the same
            // time.
            size_t restorePrio;
            do {
              restorePrio = oldPrio;
              oldPrio = thread_states[slot].yield_priority.exchange(
                restorePrio, std::memory_order_acq_rel
              );
            } while (oldPrio < restorePrio);
          }
          if (++interruptCount == interruptMax) {
            goto INTERRUPT_DONE;
          }
        }
      }
    }
  }

INTERRUPT_DONE:
  if (FromPost && spinningThreadCount != 0) {
    return;
  }
  if (sleepingThreadCount > 0) {
    size_t sleepingThreads = ~(workingThreads | spinningThreads);
    if (spinningThreadCount != 0 &&
        spinningThreadCount * 2 > workingThreadCount) {
      return;
    }
    // Limit the number of spinning threads to half the number of
    // working threads. This prevents too many spinners in a lightly
    // loaded system.
    if (FromExecThread) {
      size_t* neighbors = tmc::detail::this_thread::neighbors;
      // Skip index 0 - can't wake self
      // Wake exactly 1 thread
      for (size_t i = 1;; ++i) {
        size_t slot = neighbors[i];
        size_t bit = TMC_ONE_BIT << slot;
        if ((sleepingThreads & bit) != 0) {
          thread_states[slot].sleep_wait.fetch_add(
            1, std::memory_order_acq_rel
          );
          thread_states[slot].sleep_wait.notify_one();
          return;
        }
      }
    } else {
      size_t slot = std::countr_zero(sleepingThreads);
      thread_states[slot].sleep_wait.fetch_add(1, std::memory_order_acq_rel);
      thread_states[slot].sleep_wait.notify_one();
      return;
    }
  }
}
#ifndef TMC_USE_MUTEXQ
void ex_cpu::init_queue_iteration_order(
  std::vector<size_t> const& Forward, std::vector<size_t> const& Inverse
) {
  const size_t size = Forward.size();
  const size_t slot = Forward[0];

  // Inverse has the order in which we should wake threads
  size_t* neighbors = new size_t[size];
  for (size_t i = 0; i < size; ++i) {
    neighbors[i] = Inverse[i];
  }
  tmc::detail::this_thread::neighbors = neighbors;

  // Forward has the order in which we should look to steal work
  // Producers is the list of producers, at the neighbors indexes
  // With an additional entry inserted at index 1
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
#endif

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

// returns true if no tasks were found (caller should wait on cv)
// returns false if thread stop requested (caller should exit)
bool ex_cpu::try_run_some(
  std::stop_token& ThreadStopToken, const size_t Slot, const size_t MinPriority,
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
    if (thread_states[Slot].inbox.try_pull(item)) {
      if (wasSpinning) {
        wasSpinning = false;
        set_work(Slot);
        clr_spin(Slot);
        // TODO set priority, or handle multiple priority inboxes

        // TODO determine the impact of waking
        // Wake 1 nearest neighbor. Don't priority-preempt any running tasks
        // notify_n(1, PRIORITY_COUNT, TMC_ALL_ONES, true, false);
      }
      item();
      goto TOP;
    }
    size_t prio = 0;
    for (; prio <= MinPriority; ++prio) {
      if (!work_queues[prio].try_dequeue_ex_cpu(item, prio)) {
        continue;
      }
      if (wasSpinning) {
        wasSpinning = false;
        set_work(Slot);
        clr_spin(Slot);
        // Wake 1 nearest neighbor. Don't priority-preempt any running tasks
        notify_n(1, PRIORITY_COUNT, TMC_ALL_ONES, true, false);
      }
#ifdef TMC_PRIORITY_COUNT
      if constexpr (PRIORITY_COUNT > 1)
#else
      if (PRIORITY_COUNT > 1)
#endif
      {
        if (prio != PrevPriority) {
          // TODO RACE if a higher prio asked us to yield, but then
          // got taken by another thread, and we resumed back on our previous
          // prio, yield_priority will not be reset
          tmc::detail::this_thread::this_task.yield_priority->store(
            prio, std::memory_order_release
          );
          if (PrevPriority != NO_TASK_RUNNING) {
            task_stopper_bitsets[PrevPriority].fetch_and(
              ~(TMC_ONE_BIT << Slot), std::memory_order_acq_rel
            );
          }
          task_stopper_bitsets[prio].fetch_or(
            TMC_ONE_BIT << Slot, std::memory_order_acq_rel
          );
          tmc::detail::this_thread::this_task.prio = prio;
          PrevPriority = prio;
        }
      }
      item();
      goto TOP;
    }
    return true;
  }
}

void ex_cpu::post(work_item&& Item, size_t Priority, size_t ThreadHint) {
  assert(Priority < PRIORITY_COUNT);
  bool fromExecThread = tmc::detail::this_thread::executor == &type_erased_this;
  if (ThreadHint != TMC_ALL_ONES) {
    // Try to push this to the inbox of the hinted thread, or its neighbors
    size_t* hintNeighbors = inverse_matrix.data() + ThreadHint * thread_count();
    for (size_t i = 0; i < thread_count(); ++i) {
      size_t n = hintNeighbors[i];
      if (thread_states[n].inbox.try_push(std::move(Item))) {
        // Don't notify if all work was successfully submitted to the currently
        // active thread's private queue.
        if (n != tmc::detail::this_thread::thread_index) {
          notify_n(1, Priority, n, fromExecThread, true);
        }
        return;
      }
    }
  }

  if (fromExecThread) {
    work_queues[Priority].enqueue_ex_cpu(std::move(Item), Priority);
  } else {
    work_queues[Priority].enqueue(std::move(Item));
  }
  notify_n(1, Priority, TMC_ALL_ONES, fromExecThread, true);
}

tmc::detail::type_erased_executor* ex_cpu::type_erased() {
  return &type_erased_this;
}

// Default constructor does not call init() - you need to do it afterward
ex_cpu::ex_cpu()
    : init_params{nullptr}, type_erased_this(this), thread_stoppers{},
      // ready_task_cv{},
      task_stopper_bitsets{nullptr}, thread_states{nullptr}
#ifndef TMC_PRIORITY_COUNT
      ,
      PRIORITY_COUNT{1}
#endif
{
}

void ex_cpu::init() {
  if (is_initialized) {
    return;
  }
  is_initialized = true;
#ifndef TMC_PRIORITY_COUNT
  if (init_params != nullptr && init_params->priority_count != 0) {
    PRIORITY_COUNT = init_params->priority_count;
  }
  NO_TASK_RUNNING = PRIORITY_COUNT;
#endif
  task_stopper_bitsets = new std::atomic<size_t>[PRIORITY_COUNT];

#ifndef TMC_USE_HWLOC
  if (init_params != nullptr && init_params->thread_count != 0) {
    threads.resize(init_params->thread_count);
  } else {
    // limited to 32/64 threads for now, due to use of size_t bitset
    size_t hwconc = std::thread::hardware_concurrency();
    if (hwconc > TMC_PLATFORM_BITS) {
      hwconc = TMC_PLATFORM_BITS;
    }
    threads.resize(hwconc);
  }
#else
  hwloc_topology_t topology;
  hwloc_topology_init(&topology);
  hwloc_topology_load(topology);
  auto groupedCores = tmc::detail::group_cores_by_l3c(topology);
  bool lasso;
  tmc::detail::adjust_thread_groups(
    init_params == nullptr ? 0 : init_params->thread_count,
    init_params == nullptr ? 0.0f : init_params->thread_occupancy, groupedCores,
    lasso
  );
  {
    size_t totalThreadCount = 0;
    for (size_t i = 0; i < groupedCores.size(); ++i) {
      totalThreadCount += groupedCores[i].group_size;
    }
    threads.resize(totalThreadCount);
  }
#endif

  assert(thread_count() != 0);
  // limited to 32/64 threads for now, due to use of size_t bitset
  assert(thread_count() <= TMC_PLATFORM_BITS);

  work_queues.resize(PRIORITY_COUNT);
  for (size_t i = 0; i < PRIORITY_COUNT; ++i) {
#ifndef TMC_USE_MUTEXQ
    work_queues.emplace_at(i, thread_count() + 1);
#else
    work_queues.emplace_at(i);
#endif
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

#ifndef TMC_USE_MUTEXQ
  for (size_t prio = 0; prio < PRIORITY_COUNT; ++prio) {
    work_queues[prio].staticProducers =
      new task_queue_t::ExplicitProducer[thread_count()];
    for (size_t i = 0; i < thread_count(); ++i) {
      work_queues[prio].staticProducers[i].init(&work_queues[prio]);
    }
    work_queues[prio].dequeueProducerCount = thread_count() + 1;
  }
#endif
  std::function<void(size_t)> thread_teardown_hook = nullptr;
  if (init_params != nullptr && init_params->thread_teardown_hook != nullptr) {
    thread_teardown_hook = init_params->thread_teardown_hook;
  }
  std::atomic<int> initThreadsBarrier(static_cast<int>(thread_count()));
  std::atomic_thread_fence(std::memory_order_seq_cst);
  size_t slot = 0;
#ifdef TMC_USE_HWLOC
  auto fh = detail::get_hierarchical_matrix(groupedCores);
  auto ih = detail::invert_matrix(fh, thread_count());

  // auto fl = detail::get_lattice_matrix(groupedCores);
  // auto il = detail::invert_matrix(fl, thread_count());
  forward_matrix = std::move(fh);
  inverse_matrix = std::move(ih);
  // #ifndef NDEBUG
  //   detail::print_square_matrix(
  //     forward_matrix, thread_count(), "Forward Work-Stealing Matrix"
  //   );
  //   detail::print_square_matrix(
  //     inverse_matrix, thread_count(), "Inverse Work-Stealing Matrix"
  //   );
  // #endif
  for (size_t groupIdx = 0; groupIdx < groupedCores.size(); ++groupIdx) {
    auto& coreGroup = groupedCores[groupIdx];
    size_t groupSize = coreGroup.group_size;
    for (size_t subIdx = 0; subIdx < groupSize; ++subIdx) {
      auto sharedCores = hwloc_bitmap_dup(coreGroup.l3cache->cpuset);
#else
  // without HWLOC, treat everything as a single group
  tmc::detail::ThreadSetupData tdata;
  tdata.total_size = thread_count();
  tdata.groups.push_back({0, thread_count()});
  size_t groupIdx = 0;
  while (slot < thread_count()) {
    size_t subIdx = slot;
#endif
      // TODO pull this out into a separate struct
      threads.emplace_at(
        slot,
        [
#ifdef TMC_USE_HWLOC
          topology, sharedCores, lasso,
#endif
          this,
          forward = detail::slice_matrix(forward_matrix, thread_count(), slot),
          inverse = detail::slice_matrix(inverse_matrix, thread_count(), slot),
          groupIdx, subIdx, slot, thread_teardown_hook,
          barrier = &initThreadsBarrier](std::stop_token thread_stop_token) {
          init_thread_locals(slot);
#ifdef TMC_USE_HWLOC
          if (lasso) {
            tmc::detail::bind_thread(topology, sharedCores);
          }
          hwloc_bitmap_free(sharedCores);
#endif
#ifndef TMC_USE_MUTEXQ
          init_queue_iteration_order(forward, inverse);
#endif
          barrier->fetch_sub(1);
          barrier->notify_all();
          size_t previousPrio = NO_TASK_RUNNING;
        TOP:
          // auto cvValue = ready_task_cv.load(std::memory_order_acquire);
          while (try_run_some(
            thread_stop_token, slot, PRIORITY_COUNT - 1, previousPrio
          )) {
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
                if (!thread_states[slot].inbox.empty()) {
                  goto TOP;
                }
                for (size_t prio = 0; prio < PRIORITY_COUNT; ++prio) {
                  if (!work_queues[prio].empty()) {
                    goto TOP;
                  }
                }
              }
            }

            int waitValue =
              thread_states[slot].sleep_wait.load(std::memory_order_relaxed);

            tmc::detail::memory_barrier(); // pairs with barrier in notify_n

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

            // Double check that the queue is empty after the memory barrier.
            // In combination with the inverse double-check in notify_n,
            // this prevents any lost wakeups.
            if (!thread_states[slot].inbox.empty()) {
              goto TOP;
            }
            for (size_t i = 0; i < thread_count(); ++i) {
              if (i == slot) {
                continue;
              }
              // allow stealing from other threads' inboxes only as a last
              // resort before going to sleep
              tmc::work_item item;
              if (thread_states[i].inbox.try_pull(item)) {
                set_work(slot);
                clr_spin(slot);
                item();
                goto TOP;
              }
            }
            for (size_t prio = 0; prio < PRIORITY_COUNT; ++prio) {
              if (!work_queues[prio].empty()) {
                goto TOP;
              }
            }

            clr_spin(slot);
            // no waiting or in progress work found. wait until a task is
            // ready
            thread_states[slot].sleep_wait.wait(waitValue);
            // ready_task_cv.wait(cvValue);
            set_spin(slot);

            // cvValue = ready_task_cv.load(std::memory_order_acquire);
          }

          // Thread stop has been requested (executor is shutting down)
          working_threads_bitset.fetch_and(~(TMC_ONE_BIT << slot));
          if (thread_teardown_hook != nullptr) {
            thread_teardown_hook(slot);
          }
          clear_thread_locals();
#ifndef TMC_USE_MUTEXQ
          delete[] tmc::detail::this_thread::neighbors;
          tmc::detail::this_thread::neighbors = nullptr;

          delete[] static_cast<task_queue_t::ExplicitProducer**>(
            tmc::detail::this_thread::producers
          );
          tmc::detail::this_thread::producers = nullptr;
#endif
        }
      );
      thread_stoppers.emplace_at(slot, threads[slot].get_stop_source());
#ifdef TMC_USE_HWLOC
      ++slot;
    }
  }
  auto barrierVal = initThreadsBarrier.load();
  while (barrierVal != 0) {
    initThreadsBarrier.wait(barrierVal);
    barrierVal = initThreadsBarrier.load();
  }
  hwloc_topology_destroy(topology);
#else
    ++slot;
  }
  auto barrierVal = initThreadsBarrier.load();
  while (barrierVal != 0) {
    initThreadsBarrier.wait(barrierVal);
    barrierVal = initThreadsBarrier.load();
  }
#endif
  if (init_params != nullptr) {
    delete init_params;
    init_params = nullptr;
  }
}

#ifndef TMC_PRIORITY_COUNT
ex_cpu& ex_cpu::set_priority_count(size_t PriorityCount) {
  assert(!is_initialized);
  if (init_params == nullptr) {
    init_params = new InitParams;
  }
  init_params->priority_count = PriorityCount;
  return *this;
}
size_t ex_cpu::priority_count() {
  assert(is_initialized);
  return PRIORITY_COUNT;
}
#endif
#ifdef TMC_USE_HWLOC
ex_cpu& ex_cpu::set_thread_occupancy(float ThreadOccupancy) {
  assert(!is_initialized);
  if (init_params == nullptr) {
    init_params = new InitParams;
  }
  init_params->thread_occupancy = ThreadOccupancy;
  return *this;
}
#endif

ex_cpu& ex_cpu::set_thread_count(size_t ThreadCount) {
  assert(!is_initialized);
  // limited to 32/64 threads for now, due to use of size_t bitset
  assert(ThreadCount <= TMC_PLATFORM_BITS);
  if (init_params == nullptr) {
    init_params = new InitParams;
  }
  init_params->thread_count = ThreadCount;
  return *this;
}

ex_cpu& ex_cpu::set_thread_init_hook(std::function<void(size_t)> Hook) {
  assert(!is_initialized);
  if (init_params == nullptr) {
    init_params = new InitParams;
  }
  init_params->thread_init_hook = std::move(Hook);
  return *this;
}

ex_cpu& ex_cpu::set_thread_teardown_hook(std::function<void(size_t)> Hook) {
  assert(!is_initialized);
  if (init_params == nullptr) {
    init_params = new InitParams;
  }
  init_params->thread_teardown_hook = std::move(Hook);
  return *this;
}

size_t ex_cpu::thread_count() {
  assert(is_initialized);
  return threads.size();
}

void ex_cpu::teardown() {
  if (!is_initialized) {
    return;
  }
  is_initialized = false;

  for (size_t i = 0; i < threads.size(); ++i) {
    thread_stoppers[i].request_stop();
    thread_states[i].sleep_wait.fetch_add(1, std::memory_order_release);
    thread_states[i].sleep_wait.notify_one();
  }
  // ready_task_cv.fetch_add(1, std::memory_order_release);
  // ready_task_cv.notify_all();
  for (size_t i = 0; i < threads.size(); ++i) {
    threads[i].join();
  }
  threads.clear();
  thread_stoppers.clear();

#ifndef TMC_USE_MUTEXQ
  for (size_t i = 0; i < work_queues.size(); ++i) {
    delete[] work_queues[i].staticProducers;
  }
#endif
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
  if (tmc::detail::this_thread::exec_is(&type_erased_this)) {
    return Outer;
  } else {
    post(std::move(Outer), Priority, TMC_ALL_ONES);
    return std::noop_coroutine();
  }
}

namespace detail {

void executor_traits<tmc::ex_cpu>::post(
  tmc::ex_cpu& ex, tmc::work_item&& Item, size_t Priority, size_t ThreadHint
) {
  ex.post(std::move(Item), Priority, ThreadHint);
}

tmc::detail::type_erased_executor*
executor_traits<tmc::ex_cpu>::type_erased(tmc::ex_cpu& ex) {
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
  int exitCode =
    co_await std::move(ClientMainTask.resume_on(tmc::cpu_executor()));
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
    tmc::detail::client_main_awaiter(std::move(ClientMainTask), &exitCode), 0
  );
  exitCode.wait(std::numeric_limits<int>::min());
  return exitCode.load();
}
} // namespace tmc
