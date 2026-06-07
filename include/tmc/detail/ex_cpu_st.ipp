// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/current.hpp"
#include "tmc/detail/compat.hpp"
#include "tmc/detail/hwloc_unique_bitmap.hpp"
#include "tmc/detail/impl.hpp" // IWYU pragma: keep
#include "tmc/detail/qu_mpsc_blocking.hpp"
#include "tmc/detail/thread_locals.hpp"
#include "tmc/ex_any.hpp"
#include "tmc/ex_cpu_st.hpp"
#include "tmc/topology.hpp"
#include "tmc/work_item.hpp"

#include <coroutine>

#ifdef __linux__
#include <cstdint>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#ifdef TMC_USE_HWLOC
#include "tmc/detail/thread_layout.hpp"

#include <hwloc.h>
static_assert(sizeof(void*) == sizeof(hwloc_topology_t));
static_assert(sizeof(void*) == sizeof(hwloc_bitmap_t));
#endif

#ifdef TMC_DEBUG_THREAD_CREATION
#include <cstdio>
#endif

namespace tmc {
bool ex_cpu_st::is_initialized() {
  return initialized.load(std::memory_order_relaxed);
}

void ex_cpu_st::init_thread_locals(size_t Slot) {
  tmc::detail::this_thread::executor() = &type_erased_this;
  tmc::detail::this_thread::this_task() = {
    .prio = 0, .yield_priority = &thread_state_data.yield_priority
  };
  tmc::detail::this_thread::thread_index() = Slot;
}

void ex_cpu_st::clear_thread_locals() {
  tmc::detail::this_thread::executor() = nullptr;
  tmc::detail::this_thread::this_task() = {};
}

void ex_cpu_st::run_one(
  tmc::work_item& Item, const size_t Prio, size_t& PrevPriority
) {
  if TMC_PRIORITY_CONSTEXPR (PRIORITY_COUNT > 1) {
    thread_state_data.yield_priority.store(Prio, std::memory_order_release);
    if (Prio != PrevPriority) {
      tmc::detail::this_thread::this_task().prio = Prio;
      PrevPriority = Prio;
    }
  }

  Item();
  assert(
    Prio == tmc::current_priority() &&
    "Tasks should not modify the priority directly. Use tmc::change_priority() "
    "or .with_priority() instead."
  );
}

// returns true if no tasks were found (caller should wait on cv)
// returns false if thread stop requested (caller should exit)
bool ex_cpu_st::try_run_some(
  std::stop_token& ThreadStopToken, size_t& PrevPriority, bool* DidWait
) {
  while (true) {
  TOP:
    if (ThreadStopToken.stop_requested()) [[unlikely]] {
      return false;
    }
    work_item item;
    for (size_t prio = 0; prio < PRIORITY_COUNT; ++prio) {
      if (!private_work[prio].empty()) {
        item = std::move(private_work[prio].back());
        private_work[prio].pop_back();
        run_one(item, prio, PrevPriority);
        goto TOP;
      }
      if (work_queues[prio].try_pull(item)) {
        if (DidWait[prio]) {
          DidWait[prio] = false;
          wait_count.fetch_add(1, std::memory_order_release);
        }
        run_one(item, prio, PrevPriority);
        goto TOP;
      }
    }
    return true;
  }
}

void ex_cpu_st::clamp_priority(size_t& Priority) {
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

void ex_cpu_st::request_yield(size_t Priority) {
  if TMC_PRIORITY_CONSTEXPR (PRIORITY_COUNT > 1) {
    auto currentPrio =
      thread_state_data.yield_priority.load(std::memory_order_relaxed);
    // 2 threads may request a task to yield at the same time. The thread with
    // the higher priority (lower priority index) should prevail.
    while (currentPrio > Priority) {
      if (thread_state_data.yield_priority.compare_exchange_strong(
            currentPrio, Priority, std::memory_order_acq_rel
          )) {
        return;
      }
    }
  }
}

void ex_cpu_st::post(work_item&& Item, size_t Priority, size_t ThreadHint) {
  clamp_priority(Priority);
  bool fromExecThread =
    tmc::detail::this_thread::executor() == &type_erased_this;
  // A zero ThreadHint indicates that reschedule() was called. In that case
  // we should use the external queue to force FIFO ordering.
  if (fromExecThread && ThreadHint != 0) [[likely]] {
    private_work[Priority].push_back(static_cast<work_item&&>(Item));
    request_yield(Priority);
  } else {
    bool didWake = work_queues[Priority].post(static_cast<work_item&&>(Item));
    if (!didWake) {
      request_yield(Priority);
    }
  }
}

tmc::ex_any* ex_cpu_st::type_erased() { return &type_erased_this; }

// Default constructor does not call init() - you need to do it afterward
ex_cpu_st::ex_cpu_st()
    : init_params{nullptr}, type_erased_this(this), spins{4}
#ifndef TMC_PRIORITY_COUNT
      ,
      PRIORITY_COUNT{1}
#endif
{
  initialized.store(false, std::memory_order_seq_cst);
  wait_count.store(0, std::memory_order_relaxed);
#ifndef __linux__
  wake_wait.store(0, std::memory_order_relaxed);
#endif
}

auto ex_cpu_st::make_worker(
  std::atomic<tmc::detail::atomic_wait_t>& InitThreadsBarrier,
  // actually a hwloc_topology_t
  // will be nullptr if hwloc is not enabled
  [[maybe_unused]] void* Topology,
  // will be nullptr if hwloc is not enabled
  [[maybe_unused]] tmc::detail::hwloc_unique_bitmap& CpuSet,
  [[maybe_unused]] tmc::topology::cpu_kind::value Kind
) {
  std::function<void(tmc::topology::thread_info)> ThreadTeardownHook = nullptr;
  if (init_params != nullptr && init_params->thread_teardown_hook != nullptr) {
    ThreadTeardownHook = init_params->thread_teardown_hook;
  }
  std::function<bool(tmc::topology::thread_info)> ThreadPostRunHook = nullptr;
  if (init_params != nullptr && init_params->thread_post_run_hook != nullptr) {
    ThreadPostRunHook = init_params->thread_post_run_hook;
  }

  return [this, &InitThreadsBarrier, ThreadTeardownHook, ThreadPostRunHook
#ifdef TMC_USE_HWLOC
          ,
          topo = Topology, myCpuSet = CpuSet.clone(), Kind
#endif
  ](std::stop_token ThreadStopToken) mutable {
    const size_t Slot = 0;

#ifdef TMC_USE_HWLOC
    if (myCpuSet != nullptr) {
      tmc::detail::pin_thread(
        static_cast<hwloc_topology_t>(topo), myCpuSet, Kind
      );
    }
    myCpuSet.free();
#endif

    init_thread_locals(Slot);

    if (init_params != nullptr && init_params->thread_init_hook != nullptr) {
      tmc::topology::thread_info info{};
      info.index = Slot;
      init_params->thread_init_hook(info);
    }

    InitThreadsBarrier.fetch_sub(1);
    InitThreadsBarrier.notify_all();

    // Initialization complete, commence runloop
    tmc::topology::thread_info threadInfo{};
    threadInfo.index = Slot;
    size_t previousPrio = NO_TASK_RUNNING;

    // If consumer signaled that it was going to wait on a particular priority,
    // it should set this to true.
    bool didWait[TMC_MAX_PRIORITY_COUNT]{};

#ifdef __linux__
    struct futex_waitv waiters[TMC_MAX_PRIORITY_COUNT + 1];
    for (size_t i = 0; i < PRIORITY_COUNT; ++i) {
      waiters[i].val = task_queue_t::WAIT_VALUE;
      waiters[i].flags = FUTEX_32 | FUTEX_PRIVATE_FLAG;
      waiters[i].__reserved = 0;
      // Setup all queue waiters, except for the wait address
    }
    // The last waiter is statically reserved for the teardown signal
    waiters[PRIORITY_COUNT] = {
      .val = 0,
      .uaddr = reinterpret_cast<uintptr_t>(&this->stop_wait),
      .flags = FUTEX_32 | FUTEX_PRIVATE_FLAG,
      .__reserved = 0,
    };
#endif

  TOP:
    while (try_run_some(ThreadStopToken, previousPrio, didWait)) {
      if (ThreadPostRunHook != nullptr && ThreadPostRunHook(threadInfo)) {
        goto TOP;
      }

      for (size_t i = 1; i < spins; ++i) {
        TMC_CPU_PAUSE();
        for (size_t prio = 0; prio < PRIORITY_COUNT; ++prio) {
          if (!work_queues[prio].empty()) {
            goto TOP;
          }
        }
        if (ThreadStopToken.stop_requested()) [[unlikely]] {
          break;
        }
      }

      previousPrio = NO_TASK_RUNNING;

#ifndef __linux__
      auto waitValue = wake_wait.load(std::memory_order_seq_cst);
#endif

      work_item item;
      for (size_t prio = 0; prio < PRIORITY_COUNT; ++prio) {
        auto queueWait = work_queues[prio].pull(item);
        if (queueWait == nullptr) {
          if (didWait[prio]) {
            didWait[prio] = false;
            wait_count.fetch_add(1, std::memory_order_release);
          }
          run_one(item, prio, previousPrio);
          goto TOP;
        }
        didWait[prio] = true;
#ifdef __linux__
        waiters[prio].uaddr = reinterpret_cast<uintptr_t>(queueWait);
#endif
      }

      // No work found. Go to sleep.
      if (ThreadStopToken.stop_requested()) [[unlikely]] {
        break;
      }
#ifdef __linux__
      long waitResult =
        syscall(SYS_futex_waitv, waiters, PRIORITY_COUNT + 1, 0, nullptr, 0);
      if (waitResult >= 0 && static_cast<size_t>(waitResult) < PRIORITY_COUNT) {
        didWait[waitResult] = false;
        // Don't increment failed_wait_count since this was a successful wait.
      }
#else
      // Non-Linux platforms don't provide a futex_waitv equivalent. Use a
      // shared wake word for all queues. Producers conservatively count every
      // wake attempt that observed WAITING, so keep didWait set here and
      // account it when the item is consumed.
      if (ThreadStopToken.stop_requested()) [[unlikely]] {
        break;
      }
      for (size_t prio = 0; prio < PRIORITY_COUNT; ++prio) {
        if (!work_queues[prio].empty()) {
          goto TOP;
        }
      }
      wake_wait.wait(waitValue);
#endif
    }
    if (ThreadTeardownHook != nullptr) {
      tmc::topology::thread_info info{};
      info.index = Slot;
      ThreadTeardownHook(info);
    }
    clear_thread_locals();
  };
}

void ex_cpu_st::init() {
  bool expected = false;
  if (!initialized.compare_exchange_strong(expected, true)) {
    return;
  }

#ifndef TMC_PRIORITY_COUNT
  if (init_params != nullptr && init_params->priority_count != 0) {
    PRIORITY_COUNT = init_params->priority_count;
  } else {
    PRIORITY_COUNT = 1;
  }
  NO_TASK_RUNNING = PRIORITY_COUNT;
#endif
  if (init_params != nullptr) {
    spins = init_params->spins;
  }

  tmc::detail::hwloc_unique_bitmap partitionCpuset;
  tmc::topology::cpu_kind::value cpuKind = tmc::topology::cpu_kind::ALL;
#ifdef TMC_USE_HWLOC
  hwloc_topology_t topo;
  auto internal_topo = tmc::topology::detail::query_internal(topo);

  // Create partition cpuset based on user configuration
  if (init_params != nullptr && !init_params->partitions.empty()) {
    partitionCpuset = tmc::detail::make_partition_cpuset(
      topo, internal_topo, init_params->partitions[0], cpuKind
    );
#ifdef TMC_DEBUG_THREAD_CREATION
    std::printf("ex_cpu_st partition cpuset bitmap: ");
    partitionCpuset.print();
#endif
  }
#else
  void* topo = nullptr;
#endif

  // Construct queues
  work_queues.resize(PRIORITY_COUNT);
  for (size_t i = 0; i < PRIORITY_COUNT; ++i) {
    work_queues.emplace_at(i);
  }

  private_work.resize(PRIORITY_COUNT);
  for (size_t i = 0; i < PRIORITY_COUNT; ++i) {
    private_work.emplace_at(i);
  }

  // Initialize single thread state
  thread_state_data.yield_priority = NO_TASK_RUNNING;
  stop_wait.store(0, std::memory_order_relaxed);
  wait_count.store(0, std::memory_order_relaxed);
#ifndef __linux__
  wake_wait.store(0, std::memory_order_relaxed);
  for (size_t i = 0; i < PRIORITY_COUNT; ++i) {
    work_queues[i].set_wake_wait(wake_wait);
  }
#endif

  // Start worker thread
  std::atomic<tmc::detail::atomic_wait_t> initThreadsBarrier(1);
  tmc::detail::memory_barrier();
  worker_thread = std::jthread(
    make_worker(initThreadsBarrier, topo, partitionCpuset, cpuKind)
  );
  thread_stopper = worker_thread.get_stop_source();

  // Wait for worker to finish init
  auto barrierVal = initThreadsBarrier.load();
  while (barrierVal != 0) {
    initThreadsBarrier.wait(barrierVal);
    barrierVal = initThreadsBarrier.load();
  }

  // Cleanup
  if (init_params != nullptr) {
    delete init_params;
    init_params = nullptr;
  }
}

tmc::detail::InitParams* ex_cpu_st::set_init_params() {
  assert(!is_initialized());
  if (init_params == nullptr) {
    init_params = new tmc::detail::InitParams;
  }
  return init_params;
}

#ifdef TMC_USE_HWLOC
ex_cpu_st& ex_cpu_st::add_partition(tmc::topology::topology_filter Filter) {
  set_init_params()->add_partition(Filter);
  return *this;
}
#endif

#ifndef TMC_PRIORITY_COUNT
ex_cpu_st& ex_cpu_st::set_priority_count(size_t PriorityCount) {
  set_init_params()->set_priority_count(PriorityCount);
  return *this;
}
size_t ex_cpu_st::priority_count() { return PRIORITY_COUNT; }
#endif

ex_cpu_st&
ex_cpu_st::set_thread_post_run_hook(std::function<bool(size_t)> Hook) {
  set_init_params()->set_thread_post_run_hook(Hook);
  return *this;
}

ex_cpu_st& ex_cpu_st::set_thread_init_hook(std::function<void(size_t)> Hook) {
  set_init_params()->set_thread_init_hook(Hook);
  return *this;
}

ex_cpu_st&
ex_cpu_st::set_thread_teardown_hook(std::function<void(size_t)> Hook) {
  set_init_params()->set_thread_teardown_hook(Hook);
  return *this;
}

ex_cpu_st& ex_cpu_st::set_spins(size_t Spins) {
  set_init_params()->set_spins(Spins);
  return *this;
}

size_t ex_cpu_st::thread_count() { return 1; }

void ex_cpu_st::teardown() {
  bool expected = true;
  if (!initialized.compare_exchange_strong(expected, false)) {
    return;
  }

  // Stop and join the single worker thread
  thread_stopper.request_stop();
  stop_wait.store(1, std::memory_order_release);
#ifdef __linux__
  syscall(
    SYS_futex, reinterpret_cast<tmc::detail::atomic_waker_t*>(&stop_wait),
    FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0
  );
#else
  wake_wait.fetch_add(1, std::memory_order_release);
  wake_wait.notify_one();
#endif

  if (worker_thread.joinable()) {
    worker_thread.join();
  }

  // For each failed wait, there must also be a failed wake - where the producer
  // released the data to the queue, but didn't wake us, since we already took
  // the data. In those scenarios, it's possible for the queue destruction to
  // race with the futex wake of the producer. Wait for all of the failed wakes
  // to finish before destroying.
  // On non-Linux, we can't distinguish failed from successful wakes, so we just
  // wait for all wakes.
  while (true) {
    size_t wakeCount = 0;
    for (size_t i = 0; i < PRIORITY_COUNT; ++i) {
      wakeCount +=
        work_queues[i].wake_ref_count.load(std::memory_order_acquire);
    }
    if (wakeCount == wait_count.load(std::memory_order_acquire)) {
      break;
    }
    TMC_CPU_PAUSE();
  }

  work_queues.clear();
  private_work.clear();
}

ex_cpu_st::~ex_cpu_st() { teardown(); }

std::coroutine_handle<>
ex_cpu_st::dispatch(std::coroutine_handle<> Outer, size_t Priority) {
  if (tmc::detail::this_thread::exec_prio_is(&type_erased_this, Priority)) {
    return Outer;
  } else {
    post(static_cast<std::coroutine_handle<>&&>(Outer), Priority);
    return std::noop_coroutine();
  }
}

namespace detail {

void executor_traits<tmc::ex_cpu_st>::post(
  tmc::ex_cpu_st& ex, tmc::work_item&& Item, size_t Priority, size_t ThreadHint
) {
  ex.post(static_cast<tmc::work_item&&>(Item), Priority, ThreadHint);
}

tmc::ex_any* executor_traits<tmc::ex_cpu_st>::type_erased(tmc::ex_cpu_st& ex) {
  return ex.type_erased();
}

std::coroutine_handle<> executor_traits<tmc::ex_cpu_st>::dispatch(
  tmc::ex_cpu_st& ex, std::coroutine_handle<> Outer, size_t Priority
) {
  return ex.dispatch(Outer, Priority);
}

} // namespace detail
} // namespace tmc
