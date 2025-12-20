// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#include "tmc/current.hpp"
#include "tmc/detail/compat.hpp"
#include "tmc/detail/hwloc_unique_bitmap.hpp"
#include "tmc/detail/qu_mpsc.hpp"
#include "tmc/detail/thread_layout.hpp"
#include "tmc/detail/thread_locals.hpp"
#include "tmc/ex_any.hpp"
#include "tmc/ex_cpu_st.hpp"
#include "tmc/topology.hpp"
#include "tmc/work_item.hpp"

#include <coroutine>

#ifdef TMC_USE_HWLOC
#include <hwloc.h>
static_assert(sizeof(void*) == sizeof(hwloc_topology_t));
static_assert(sizeof(void*) == sizeof(hwloc_bitmap_t));
#endif

namespace tmc {

void ex_cpu_st::set_state(WorkerState NewState) {
  thread_state.store(NewState, std::memory_order_release);
}

ex_cpu_st::WorkerState ex_cpu_st::get_state() {
  return thread_state.load(std::memory_order_acquire);
}

bool ex_cpu_st::is_initialized() {
  return initialized.load(std::memory_order_relaxed);
}

void ex_cpu_st::notify_n(size_t Priority, bool FromExecThread) {
  // In combination with the inverse barrier/double-check in the main worker
  // loop, prevents lost wakeups.
  if (!FromExecThread) {
    tmc::detail::memory_barrier();
  }
  WorkerState currentState = get_state();

  // For single-threaded executor: only wake if thread is sleeping
  if (currentState == WorkerState::SLEEPING) {
    thread_state_data.sleep_wait.fetch_add(1, std::memory_order_acq_rel);
    thread_state_data.sleep_wait.notify_one();
  } else if (currentState == WorkerState::WORKING) {
    // Possibly interrupt a working thread, if new task priority is higher
#ifdef TMC_PRIORITY_COUNT
    if constexpr (PRIORITY_COUNT > 1)
#else
    if (PRIORITY_COUNT > 1)
#endif
    {
      auto currentPrio =
        thread_state_data.yield_priority.load(std::memory_order_relaxed);
      // 2 threads may request a task to yield at the same time. The
      // thread with the higher priority (lower priority index) should
      // prevail.
      while (currentPrio > Priority) {
        if (thread_state_data.yield_priority.compare_exchange_strong(
              currentPrio, Priority, std::memory_order_acq_rel
            )) {
          return;
        }
      }
    }
  }
  // If thread is already spinning or working, no need to wake it
}

void ex_cpu_st::init_thread_locals(size_t Slot) {
  tmc::detail::this_thread::executor = &type_erased_this;
  tmc::detail::this_thread::this_task = {
    .prio = 0, .yield_priority = &thread_state_data.yield_priority
  };
  tmc::detail::this_thread::thread_index = Slot;
}

void ex_cpu_st::clear_thread_locals() {
  tmc::detail::this_thread::executor = nullptr;
  tmc::detail::this_thread::this_task = {};
}

void ex_cpu_st::run_one(
  tmc::work_item& Item, const size_t Prio, size_t& PrevPriority,
  bool& WasSpinning
) {
  if (WasSpinning) {
    WasSpinning = false;
    set_state(WorkerState::WORKING);
  }
#ifdef TMC_PRIORITY_COUNT
  if constexpr (PRIORITY_COUNT > 1)
#else
  if (PRIORITY_COUNT > 1)
#endif
  {
    thread_state_data.yield_priority.store(Prio, std::memory_order_release);
    if (Prio != PrevPriority) {
      tmc::detail::this_thread::this_task.prio = Prio;
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
  std::stop_token& ThreadStopToken, size_t& PrevPriority
) {
  // Precondition: this thread is spinning / not working
  bool wasSpinning = true;
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
        run_one(item, prio, PrevPriority, wasSpinning);
        goto TOP;
      }
      if (work_queues[prio].try_pull(item)) {
        run_one(item, prio, PrevPriority, wasSpinning);
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

void ex_cpu_st::post(work_item&& Item, size_t Priority, size_t ThreadHint) {
  clamp_priority(Priority);
  bool fromExecThread = tmc::detail::this_thread::executor == &type_erased_this;
  if (fromExecThread && ThreadHint != 0) [[likely]] {
    private_work[Priority].push_back(static_cast<work_item&&>(Item));
    notify_n(Priority, fromExecThread);
  } else {
    auto handle = work_queues[Priority].get_hazard_ptr();
    auto& haz = handle.value;
    work_queues[Priority].post(&haz, static_cast<work_item&&>(Item));
    notify_n(Priority, fromExecThread);
    // Hold the handle until after notify_n() to prevent race
    // with destructor on another thread
    handle.release();
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
}

auto ex_cpu_st::make_worker(
  size_t Slot, std::atomic<int>& InitThreadsBarrier,
  // actually a hwloc_topology_t
  // will be nullptr if hwloc is not enabled
  [[maybe_unused]] void* Topology,
  // will be nullptr if hwloc is not enabled
  [[maybe_unused]] tmc::detail::hwloc_unique_bitmap& CpuSet
) {
  std::function<void(size_t)> ThreadTeardownHook = nullptr;
  if (init_params != nullptr && init_params->thread_teardown_hook != nullptr) {
    ThreadTeardownHook = init_params->thread_teardown_hook;
  }

  return [this, Slot, &InitThreadsBarrier, ThreadTeardownHook
#ifdef TMC_USE_HWLOC
          ,
          topo = Topology, myCpuSet = CpuSet.obj
#endif
  ](std::stop_token ThreadStopToken) {
    // Ensure this thread sees all non-atomic read-only values
    tmc::detail::memory_barrier();

#ifdef TMC_USE_HWLOC
    if (myCpuSet != nullptr) {
      tmc::detail::pin_thread(static_cast<hwloc_topology_t>(topo), myCpuSet);
    }
#endif

    init_thread_locals(Slot);

    if (init_params != nullptr && init_params->thread_init_hook != nullptr) {
      init_params->thread_init_hook(Slot);
    }

    InitThreadsBarrier.fetch_sub(1);
    InitThreadsBarrier.notify_all();

    // Initialization complete, commence runloop
    size_t previousPrio = NO_TASK_RUNNING;
  TOP:
    while (try_run_some(ThreadStopToken, previousPrio)) {
      // Transition from working to spinning
      set_state(WorkerState::SPINNING);
      for (size_t i = 0; i < spins; ++i) {
        TMC_CPU_PAUSE();
        for (size_t prio = 0; prio < PRIORITY_COUNT; ++prio) {
          if (!work_queues[prio].empty()) {
            goto TOP;
          }
        }
      }

      previousPrio = NO_TASK_RUNNING;

      // Transition from spinning to sleeping.
      int waitValue =
        thread_state_data.sleep_wait.load(std::memory_order_relaxed);
      set_state(WorkerState::SLEEPING);
      tmc::detail::memory_barrier(); // pairs with barrier in notify_n

      // Double check that the queue is empty after the memory
      // barrier. In combination with the inverse double-check in
      // notify_n, this prevents any lost wakeups.
      for (size_t prio = 0; prio < PRIORITY_COUNT; ++prio) {
        if (!work_queues[prio].empty()) {
          set_state(WorkerState::SPINNING);
          goto TOP;
        }
      }

      // No work found. Go to sleep.
      if (ThreadStopToken.stop_requested()) [[unlikely]] {
        break;
      }
      thread_state_data.sleep_wait.wait(waitValue);

      // Upon waking, transition from sleeping to spinning
      set_state(WorkerState::SPINNING);
    }
    if (ThreadTeardownHook != nullptr) {
      ThreadTeardownHook(Slot);
    }
    clear_thread_locals();
  };
}

void ex_cpu_st::init() {
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
  if (init_params != nullptr) {
    spins = init_params->spins;
  }

  tmc::detail::hwloc_unique_bitmap threadCpuset;
#ifdef TMC_USE_HWLOC
  hwloc_topology_t topo;
  auto internal_topo = tmc::topology::detail::query_internal(topo);

  // Create partition cpuset based on user configuration
  if (init_params != nullptr && !init_params->partitions.empty()) {
    threadCpuset = tmc::detail::make_partition_cpuset(
      topo, internal_topo, init_params->partitions[0]
    );
    std::printf("overall partition cpuset:\n");
    print_cpu_set(threadCpuset);
  }
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
  thread_state_data.sleep_wait = 0;
  thread_state.store(WorkerState::SPINNING);

  // Start worker thread
  std::atomic<int> initThreadsBarrier(1);
  tmc::detail::memory_barrier();
  worker_thread =
    std::jthread(make_worker(0, initThreadsBarrier, topo, threadCpuset));
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
ex_cpu_st& ex_cpu_st::add_partition(tmc::topology::TopologyFilter Filter) {
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
  thread_state_data.sleep_wait.fetch_add(1, std::memory_order_seq_cst);
  thread_state_data.sleep_wait.notify_one();

  if (worker_thread.joinable()) {
    worker_thread.join();
  }

  work_queues.clear();
  private_work.clear();
}

ex_cpu_st::~ex_cpu_st() { teardown(); }

std::coroutine_handle<>
ex_cpu_st::task_enter_context(std::coroutine_handle<> Outer, size_t Priority) {
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

std::coroutine_handle<> executor_traits<tmc::ex_cpu_st>::task_enter_context(
  tmc::ex_cpu_st& ex, std::coroutine_handle<> Outer, size_t Priority
) {
  return ex.task_enter_context(Outer, Priority);
}

} // namespace detail
} // namespace tmc
