// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

// Implementation definition file for tmc::ex_cpu_st. This will be included
// anywhere TMC_IMPL is defined. If you prefer to manually separate compilation
// units, you can instead include this file directly in a CPP file.

#include "tmc/current.hpp"
#include "tmc/detail/compat.hpp"
#include "tmc/detail/qu_mpsc.hpp"
#include "tmc/detail/thread_layout.hpp"
#include "tmc/detail/thread_locals.hpp"
#include "tmc/ex_any.hpp"
#include "tmc/ex_cpu_st.hpp"
#include "tmc/work_item.hpp"

#include <bit>
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

void ex_cpu_st::notify_n(size_t Priority) {
  // TODO if FromExecThread, we don't need a barrier or waking logic

  // In combination with the inverse barrier/double-check in the main worker
  // loop, prevents lost wakeups.
  tmc::detail::memory_barrier(); // pairs with barrier in make_worker
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
      for (size_t prio = PRIORITY_COUNT - 1; prio > Priority; --prio) {
        size_t set = task_stopper_bitsets[prio].load(std::memory_order_acquire);
        while (set != 0) {
          size_t slot = static_cast<size_t>(std::countr_zero(set));
          set = set & ~(TMC_ONE_BIT << slot);
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
  tmc::work_item& Item, const size_t Slot, const size_t Prio,
  size_t& PrevPriority, bool& WasSpinning
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
    Prio == tmc::current_priority() &&
    "Tasks should not modify the priority directly. Use tmc::change_priority() "
    "or .with_priority() instead."
  );
}

// returns true if no tasks were found (caller should wait on cv)
// returns false if thread stop requested (caller should exit)
bool ex_cpu_st::try_run_some(
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
    for (size_t prio = 0; prio < PRIORITY_COUNT; ++prio) {
      if (!private_work[prio].empty()) {
        item = std::move(private_work[prio].back());
        private_work[prio].pop_back();
        run_one(item, Slot, prio, PrevPriority, wasSpinning);
        goto TOP;
      }
      if (work_queues[prio].try_pull(item)) {
        run_one(item, Slot, prio, PrevPriority, wasSpinning);
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
    notify_n(Priority);
  } else {
    auto handle = work_queues[Priority].get_hazard_ptr();
    auto& tok = handle.value;
    work_queues[Priority].post(&tok, static_cast<work_item&&>(Item));
    notify_n(Priority);
    handle.release();
  }
}

tmc::ex_any* ex_cpu_st::type_erased() { return &type_erased_this; }

// Default constructor does not call init() - you need to do it afterward
ex_cpu_st::ex_cpu_st()
    : init_params{nullptr}, type_erased_this(this),
      task_stopper_bitsets{nullptr}
#ifndef TMC_PRIORITY_COUNT
      ,
      PRIORITY_COUNT{1}
#endif
{
  initialized.store(false, std::memory_order_seq_cst);
}

auto ex_cpu_st::make_worker(
  size_t Slot, std::atomic<int>& InitThreadsBarrier,
  // actually a hwloc_bitmap_t
  // will be nullptr if hwloc is not enabled
  void* CpuSet
) {
  std::function<void(size_t)> ThreadTeardownHook = nullptr;
  if (init_params != nullptr && init_params->thread_teardown_hook != nullptr) {
    ThreadTeardownHook = init_params->thread_teardown_hook;
  }

  return [this, Slot, &InitThreadsBarrier, ThreadTeardownHook
#ifdef TMC_USE_HWLOC
          ,
          myCpuSet = hwloc_bitmap_dup(static_cast<hwloc_bitmap_t>(CpuSet))
#endif
  ](std::stop_token ThreadStopToken) {
    // Ensure this thread sees all non-atomic read-only values
    tmc::detail::memory_barrier();

#ifdef TMC_USE_HWLOC
    if (myCpuSet != nullptr) {
      tmc::detail::bind_thread(
        static_cast<hwloc_topology_t>(topology), myCpuSet
      );
    }
    hwloc_bitmap_free(myCpuSet);
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
    while (try_run_some(ThreadStopToken, Slot, previousPrio)) {
      // Transition from working to spinning
      set_state(WorkerState::SPINNING);
      for (size_t i = 0; i < 4; ++i) {
        TMC_CPU_PAUSE();
        for (size_t prio = 0; prio < PRIORITY_COUNT; ++prio) {
          if (!work_queues[prio].empty()) {
            goto TOP;
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
  tmc::detail::adjust_thread_groups(1, 0.0f, groupedCores, lasso);
#endif

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

  // Single thread starts in the spinning state
  thread_state.store(WorkerState::SPINNING);

  std::atomic<int> initThreadsBarrier(1);
  tmc::detail::memory_barrier();
  void* threadCpuSet = nullptr;
#ifdef TMC_USE_HWLOC
  if (!groupedCores.empty()) {
    auto& coreGroup = groupedCores[0];
    if (lasso) {
      threadCpuSet = static_cast<hwloc_obj_t>(coreGroup.l3cache)->cpuset;
    }
  }
#endif
  worker_thread =
    std::jthread(make_worker(0, initThreadsBarrier, threadCpuSet));
  thread_stopper = worker_thread.get_stop_source();

  // Wait for worker to finish init
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
ex_cpu_st& ex_cpu_st::set_priority_count(size_t PriorityCount) {
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
size_t ex_cpu_st::priority_count() { return PRIORITY_COUNT; }
#endif

ex_cpu_st& ex_cpu_st::set_thread_init_hook(std::function<void(size_t)> Hook) {
  assert(!is_initialized());
  if (init_params == nullptr) {
    init_params = new InitParams;
  }
  init_params->thread_init_hook =
    static_cast<std::function<void(size_t)>&&>(Hook);
  return *this;
}

ex_cpu_st&
ex_cpu_st::set_thread_teardown_hook(std::function<void(size_t)> Hook) {
  assert(!is_initialized());
  if (init_params == nullptr) {
    init_params = new InitParams;
  }
  init_params->thread_teardown_hook =
    static_cast<std::function<void(size_t)>&&>(Hook);
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

  for (size_t i = 0; i < work_queues.size(); ++i) {
    while (work_queues[i].is_in_use()) {
      TMC_CPU_PAUSE();
    }
  }

#ifdef TMC_USE_HWLOC
  hwloc_topology_destroy(static_cast<hwloc_topology_t>(topology));
#endif

  work_queues.clear();
  private_work.clear();
  if (task_stopper_bitsets != nullptr) {
    delete[] task_stopper_bitsets;
  }
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
