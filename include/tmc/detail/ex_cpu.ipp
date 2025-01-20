// Copyright (c) 2023-2024 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

// Implementation definition file for tmc::ex_cpu. This will be included
// anywhere TMC_IMPL is defined. If you prefer to manually separate compilation
// units, you can instead include this file directly in a CPP file.

#include "tmc/detail/qu_lockfree.hpp"
#include "tmc/detail/thread_layout.hpp"
#include "tmc/ex_cpu.hpp"

namespace tmc {
void ex_cpu::notify_n(size_t Count, size_t Priority) {
// TODO set notified threads prev_prod (index 1) to this?
#ifdef _MSC_VER
  size_t workingThreadCount = static_cast<size_t>(
    __popcnt64(working_threads_bitset.load(std::memory_order_acquire))
  );
#else
  size_t workingThreadCount = static_cast<size_t>(
    __builtin_popcountll(working_threads_bitset.load(std::memory_order_acquire))
  );
#endif
  size_t sleepingThreadCount = thread_count() - workingThreadCount;
#ifdef TMC_PRIORITY_COUNT
  if constexpr (PRIORITY_COUNT > 1)
#else
  if (PRIORITY_COUNT > 1)
#endif
  {
    // if available threads can take all tasks, no need to interrupt
    if (sleepingThreadCount < Count && workingThreadCount != 0) {
      size_t interruptCount = 0;
      size_t interruptMax =
        std::min(workingThreadCount, Count - sleepingThreadCount);
      for (size_t prio = PRIORITY_COUNT - 1; prio > Priority; --prio) {
        uint64_t set =
          task_stopper_bitsets[prio].load(std::memory_order_acquire);
        while (set != 0) {
#ifdef _MSC_VER
          size_t slot = static_cast<size_t>(_tzcnt_u64(set));
#else
          size_t slot = static_cast<size_t>(__builtin_ctzll(set));
#endif
          set = set & ~(1ULL << slot);
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
  // As a performance optimization, only modify ready_task_cv when we know there
  // is at least 1 sleeping thread. This requires some extra care to prevent a
  // race with threads going to sleep.

  // TODO on Linux this tends to wake threads in a round-robin fashion.
  //   Prefer to wake more recently used threads instead (see folly::LifoSem)
  //   or wake neighbor and peer threads first
  if (sleepingThreadCount > 0) {
    ready_task_cv.fetch_add(1, std::memory_order_acq_rel);
    if (Count > 1) {
      ready_task_cv.notify_all();
    } else {
      ready_task_cv.notify_one();
    }
  }
  // if (count >= availableThreads) {
  // ready_task_cv.notify_all();
  //} else {
  //  for (size_t i = 0; i < count; ++i) {
  //    ready_task_cv.notify_one();
  //  }
  //}
}
#ifndef TMC_USE_MUTEXQ
void ex_cpu::init_queue_iteration_order(
  tmc::detail::ThreadSetupData const& TData, size_t GroupIdx, size_t SubIdx,
  size_t Slot
) {
  std::vector<size_t> iterationOrder;
  iterationOrder.reserve(TData.total_size);
  // Calculate entire iteration order in advance and cache it.
  // The resulting order will be:
  // This thread
  // The previously consumed-from thread (dynamically updated)
  // Other threads in this thread's group
  // 1 thread from each other group (with same slot_off as this)
  // Remaining threads

  // This thread + other threads in this group
  {
    auto& group = TData.groups[GroupIdx];
    for (size_t off = 0; off < group.size; ++off) {
      size_t sidx = (SubIdx + off) % group.size;
      iterationOrder.push_back(sidx + group.start);
    }
  }

  auto groupOrder =
    tmc::detail::get_group_iteration_order(TData.groups.size(), GroupIdx);
  assert(groupOrder.size() == TData.groups.size());

  // 1 peer thread from each other group (with same sub_idx as this)
  // groups may have different sizes, so use modulo
  for (size_t groupOff = 1; groupOff < groupOrder.size(); ++groupOff) {
    size_t gidx = groupOrder[groupOff];
    auto& group = TData.groups[gidx];
    size_t sidx = SubIdx % group.size;
    iterationOrder.push_back(sidx + group.start);
  }

  // Remaining threads from other groups (1 group at a time)
  for (size_t groupOff = 1; groupOff < groupOrder.size(); ++groupOff) {
    size_t gidx = groupOrder[groupOff];
    auto& group = TData.groups[gidx];
    for (size_t off = 1; off < group.size; ++off) {
      size_t sidx = (SubIdx + off) % group.size;
      iterationOrder.push_back(sidx + group.start);
    }
  }
  assert(iterationOrder.size() == TData.total_size);

  size_t dequeueCount = TData.total_size + 1;
  task_queue_t::ExplicitProducer** producers =
    new task_queue_t::ExplicitProducer*[PRIORITY_COUNT * dequeueCount];
  for (size_t prio = 0; prio < PRIORITY_COUNT; ++prio) {
    assert(Slot == iterationOrder[0]);
    size_t pidx = prio * dequeueCount;
    // pointer to this thread's producer
    producers[pidx] = &work_queues[prio].staticProducers[Slot];
    ++pidx;
    // pointer to previously consumed-from producer (initially none)
    producers[pidx] = nullptr;
    ++pidx;

    for (size_t i = 1; i < TData.total_size; ++i) {
      task_queue_t::ExplicitProducer* prod =
        &work_queues[prio].staticProducers[iterationOrder[i]];
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
  while (true) {
  TOP:
    if (ThreadStopToken.stop_requested()) [[unlikely]] {
      return false;
    }
    size_t prio = 0;
    for (; prio <= MinPriority; ++prio) {
      work_item item;
      if (!work_queues[prio].try_dequeue_ex_cpu(item, prio)) {
        continue;
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
              ~(1ULL << Slot), std::memory_order_acq_rel
            );
          }
          task_stopper_bitsets[prio].fetch_or(
            1ULL << Slot, std::memory_order_acq_rel
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

void ex_cpu::post(work_item&& Item, size_t Priority) {
  work_queues[Priority].enqueue_ex_cpu(std::move(Item), Priority);
  notify_n(Priority, 1);
}

tmc::detail::type_erased_executor* ex_cpu::type_erased() {
  return &type_erased_this;
}

// Default constructor does not call init() - you need to do it afterward
ex_cpu::ex_cpu()
    : init_params{nullptr}, type_erased_this(this), thread_stoppers{},
      ready_task_cv{}, task_stopper_bitsets{nullptr}, thread_states{nullptr}
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
  task_stopper_bitsets = new std::atomic<uint64_t>[PRIORITY_COUNT];
  work_queues.resize(PRIORITY_COUNT);
  for (size_t i = 0; i < PRIORITY_COUNT; ++i) {
#ifndef TMC_USE_MUTEXQ
    work_queues.emplace_at(i, 32000UL);
#else
    work_queues.emplace_at(i);
#endif
  }
#ifndef TMC_USE_HWLOC
  if (init_params != nullptr && init_params->thread_count != 0) {
    threads.resize(init_params->thread_count);
  } else {
    // limited to 64 threads for now, due to use of uint64_t bitset
    size_t hwconc = std::thread::hardware_concurrency();
    if (hwconc > 64) {
      hwconc = 64;
    }
    threads.resize(hwconc);
  }
#else
  hwloc_topology_t topology;
  hwloc_topology_init(&topology);
  hwloc_topology_load(topology);
  auto groupedCores = tmc::detail::group_cores_by_l3c(topology);
  bool lasso = true;
  size_t totalThreadCount = 0;
  size_t coreCount = 0;
  for (size_t i = 0; i < groupedCores.size(); ++i) {
    coreCount += groupedCores[i].group_size;
  }
  if (init_params == nullptr || (init_params->thread_count == 0 &&
                                 init_params->thread_occupancy >= -0.0001f &&
                                 init_params->thread_occupancy <= 0.0001f)) {
    totalThreadCount = coreCount;
  } else {
    if (init_params->thread_count != 0) {
      float occupancy = static_cast<float>(init_params->thread_count) /
                        static_cast<float>(coreCount);
      totalThreadCount = init_params->thread_count;
      if (occupancy <= 0.5f) {
        // turn off thread-lasso capability and make everything one group
        groupedCores.resize(1);
        groupedCores[0].group_size = init_params->thread_count;
        lasso = false;
      } else if (coreCount > init_params->thread_count) {
        // Evenly reduce the size of groups until we hit the desired thread
        // count
        size_t i = groupedCores.size() - 1;
        while (coreCount > init_params->thread_count) {
          --groupedCores[i].group_size;
          --coreCount;
          if (i == 0) {
            i = groupedCores.size() - 1;
          } else {
            --i;
          }
        }
      } else if (coreCount < init_params->thread_count) {
        // Evenly increase the size of groups until we hit the desired thread
        // count
        size_t i = 0;
        while (coreCount < init_params->thread_count) {
          ++groupedCores[i].group_size;
          ++coreCount;
          ++i;
          if (i == groupedCores.size()) {
            i = 0;
          }
        }
      }
    } else { // init_params->thread_occupancy != 0
      for (size_t i = 0; i < groupedCores.size(); ++i) {
        size_t groupSize = static_cast<size_t>(
          static_cast<float>(groupedCores[i].group_size) *
          init_params->thread_occupancy
        );
        groupedCores[i].group_size = groupSize;
        totalThreadCount += groupSize;
      }
    }
  }
  threads.resize(totalThreadCount);
#endif
  assert(thread_count() != 0);
  // limited to 64 threads for now, due to use of uint64_t bitset
  assert(thread_count() <= 64);
  thread_states = new ThreadState[thread_count()];
  for (size_t i = 0; i < thread_count(); ++i) {
    thread_states[i].yield_priority = NO_TASK_RUNNING;
  }

  thread_stoppers.resize(thread_count());
  // All threads start in the "working" state
  working_threads_bitset.store(
    (1ULL << (thread_count() - 1)) | ((1ULL << (thread_count() - 1)) - 1)
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
  std::atomic<int> initThreadsBarrier(static_cast<int>(thread_count()));
  std::atomic_thread_fence(std::memory_order_seq_cst);
  size_t slot = 0;
  size_t groupStart = 0;
#ifdef TMC_USE_HWLOC
  // copy elements of groupedCores into thread lambda capture
  // that will go out of scope at the end of this function
  tmc::detail::ThreadSetupData tdata;
  tdata.total_size = thread_count();
  tdata.groups.resize(groupedCores.size());
  for (size_t i = 0; i < groupedCores.size(); ++i) {
    size_t groupSize = groupedCores[i].group_size;
    tdata.groups[i].size = groupSize;
    tdata.groups[i].start = groupStart;
    groupStart += groupSize;
  }
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
          this, tdata, groupIdx, subIdx, slot,
          barrier = &initThreadsBarrier](std::stop_token thread_stop_token) {
          init_thread_locals(slot);
#ifdef TMC_USE_HWLOC
          if (lasso) {
            tmc::detail::bind_thread(topology, sharedCores);
          }
          hwloc_bitmap_free(sharedCores);
#endif
#ifndef TMC_USE_MUTEXQ
          init_queue_iteration_order(tdata, groupIdx, subIdx, slot);
#endif
          barrier->fetch_sub(1);
          barrier->notify_all();
          size_t previousPrio = NO_TASK_RUNNING;
        TOP:
          auto cvValue = ready_task_cv.load(std::memory_order_acquire);
          while (try_run_some(
            thread_stop_token, slot, PRIORITY_COUNT - 1, previousPrio
          )) {
            auto newCvValue = ready_task_cv.load(std::memory_order_acquire);
            if (newCvValue != cvValue) {
              // more tasks have been posted, try again
              cvValue = newCvValue;
              continue;
            }
            // Because of dequeueOvercommit, when multiple threads try to
            // dequeue at once, they may all see the queue as empty
            // incorrectly. Empty() is more accurate
            for (size_t prio = 0; prio < PRIORITY_COUNT; ++prio) {
              if (!work_queues[prio].empty()) {
                goto TOP;
              }
            }

            // no waiting or in progress work found. wait until a task is
            // ready
            working_threads_bitset.fetch_and(~(1ULL << slot));

#ifdef TMC_PRIORITY_COUNT
            if constexpr (PRIORITY_COUNT > 1)
#else
          if (PRIORITY_COUNT > 1)
#endif
            {
              if (previousPrio != NO_TASK_RUNNING) {
                task_stopper_bitsets[previousPrio].fetch_and(
                  ~(1ULL << slot), std::memory_order_acq_rel
                );
              }
            }
            previousPrio = NO_TASK_RUNNING;

            // To mitigate a race condition with the calculation of
            // available threads in notify_n, we need to check the queue again
            // before going to sleep
            for (size_t prio = 0; prio < PRIORITY_COUNT; ++prio) {
              if (!work_queues[prio].empty()) {
                working_threads_bitset.fetch_or(1ULL << slot);
                goto TOP;
              }
            }
            ready_task_cv.wait(cvValue);
            working_threads_bitset.fetch_or(1ULL << slot);
            cvValue = ready_task_cv.load(std::memory_order_acquire);
          }

          // Thread stop has been requested (executor is shutting down)
          working_threads_bitset.fetch_and(~(1ULL << slot));
          clear_thread_locals();
#ifndef TMC_USE_MUTEXQ
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
  if (init_params == nullptr) {
    init_params = new InitParams;
  }
  init_params->thread_count = ThreadCount;
  return *this;
}

ex_cpu& ex_cpu::set_thread_init_hook(void (*Hook)(size_t)) {
  assert(!is_initialized);
  if (init_params == nullptr) {
    init_params = new InitParams;
  }
  init_params->thread_init_hook = Hook;
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
  }
  ready_task_cv.fetch_add(1, std::memory_order_release);
  ready_task_cv.notify_all();
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
    post(std::move(Outer), Priority);
    return std::noop_coroutine();
  }
}

namespace detail {
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
