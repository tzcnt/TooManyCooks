// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

// Implementation definition file for tmc::ex_cpu_st. This will be included
// anywhere TMC_IMPL is defined. If you prefer to manually separate compilation
// units, you can instead include this file directly in a CPP file.

#include "tmc/current.hpp"
#include "tmc/detail/compat.hpp"
#include "tmc/detail/qu_inbox.hpp"
#include "tmc/detail/qu_lockfree.hpp"
#include "tmc/detail/thread_layout.hpp"
#include "tmc/detail/thread_locals.hpp"
#include "tmc/ex_any.hpp"
#include "tmc/ex_cpu_st.hpp"
#include "tmc/sync.hpp"
#include "tmc/work_item.hpp"

#include <bit>

#ifdef TMC_USE_HWLOC
#include <hwloc.h>
#endif

namespace tmc {

bool ex_cpu_st::is_initialized() {
  return initialized.load(std::memory_order_relaxed);
}

void ex_cpu_st::notify(size_t Priority) {
  // As a performance optimization, we only try to wake when we know
  // there is at least 1 sleeping thread. In combination with the inverse
  // barrier/double-check in the main worker loop, prevents lost wakeups.
  tmc::detail::memory_barrier();

  auto status = thread_state.worker_status.load(std::memory_order_relaxed);
  switch (status) {
  case WorkerStatus::Spinning:
    return;
  case WorkerStatus::Working:
    for (size_t prio = PRIORITY_COUNT - 1; prio > Priority; --prio) {
      size_t set = task_stopper_bitsets[prio].load(std::memory_order_acquire);
      while (set != 0) {
        size_t slot = std::countr_zero(set);
        set = set & ~(TMC_ONE_BIT << slot);
        auto currentPrio =
          thread_state.yield_priority.load(std::memory_order_relaxed);

        // 2 threads may request a task to yield at the same time. The thread
        // with the higher priority (lower priority index) should prevail.
        while (currentPrio > Priority) {
          if (thread_state.yield_priority.compare_exchange_strong(
                currentPrio, Priority, std::memory_order_acq_rel
              )) {
            return;
            break;
          }
        }
      }
    }
    [[fallthrough]];
  case WorkerStatus::Sleeping:
    thread_state.sleep_wait.fetch_add(1, std::memory_order_acq_rel);
    thread_state.sleep_wait.notify_one();
    break;
  }
}

void ex_cpu_st::init_queue_iteration_order(std::vector<size_t> const& Forward) {
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

void ex_cpu_st::init_thread_locals(size_t Slot) {
  tmc::detail::this_thread::executor = &type_erased_this;
  tmc::detail::this_thread::this_task = {
    .prio = 0, .yield_priority = &thread_state.yield_priority
  };
  tmc::detail::this_thread::thread_index = Slot;
  if (init_params != nullptr && init_params->thread_init_hook != nullptr) {
    init_params->thread_init_hook(Slot);
  }
}

void ex_cpu_st::clear_thread_locals() {
  tmc::detail::this_thread::executor = nullptr;
  tmc::detail::this_thread::this_task = {};
}

void ex_cpu_st::run_one(
  tmc::work_item& item, const size_t Slot, const size_t Prio,
  size_t& PrevPriority, bool& WasSpinning
) {
  if (WasSpinning) {
    WasSpinning = false;
    thread_state.worker_status.store(
      WorkerStatus::Working, std::memory_order_relaxed
    );
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
    if (inbox->try_pull(item, inbox_prio)) {
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
  if (ThreadHint == 0) {
    if (inbox->try_push(static_cast<work_item&&>(Item), Priority)) {
      if (!fromExecThread) {
        notify(Priority);
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
    notify(Priority);
  }
}

tmc::ex_any* ex_cpu_st::type_erased() { return &type_erased_this; }

// Default constructor does not call init() - you need to do it afterward
ex_cpu_st::ex_cpu_st()
    : inbox{nullptr}, init_params{nullptr}, type_erased_this(this),
      thread_stoppers{}, task_stopper_bitsets{nullptr}
#ifndef TMC_PRIORITY_COUNT
      ,
      PRIORITY_COUNT{1}
#endif
{
  initialized.store(false, std::memory_order_seq_cst);
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

  threads.resize(1);

  inbox = new tmc::detail::qu_inbox<work_item, 4096>;

  work_queues.resize(PRIORITY_COUNT);
  for (size_t i = 0; i < PRIORITY_COUNT; ++i) {
    work_queues.emplace_at(i, thread_count() + 1);
  }

  for (size_t i = 0; i < thread_count(); ++i) {
    thread_state.yield_priority = NO_TASK_RUNNING;
    thread_state.sleep_wait = 0;
    thread_state.worker_status = WorkerStatus::Spinning;
  }

  thread_stoppers.resize(thread_count());

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
  // TODO pull this out into a separate struct
  threads.emplace_at(
    slot,
    [this, slot, thread_teardown_hook,
     barrier = &initThreadsBarrier](std::stop_token thread_stop_token) {
      // Ensure this thread sees all non-atomic read-only values
      tmc::detail::memory_barrier();
      init_thread_locals(slot);
      init_queue_iteration_order(std::vector<size_t>(1, 0));
      barrier->fetch_sub(1);
      barrier->notify_all();
      size_t previousPrio = NO_TASK_RUNNING;
    TOP:
      while (try_run_some(thread_stop_token, slot, previousPrio)) {
        thread_state.worker_status.store(
          WorkerStatus::Spinning, std::memory_order_relaxed
        );

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
              ~(TMC_ONE_BIT << slot), std::memory_order_acq_rel
            );
          }
        }
        previousPrio = NO_TASK_RUNNING;

        // Transition from spinning to sleeping.
        int waitValue = thread_state.sleep_wait.load(std::memory_order_relaxed);
        thread_state.worker_status.store(
          WorkerStatus::Sleeping, std::memory_order_relaxed
        );
        tmc::detail::memory_barrier(); // pairs with barrier in notify_n

        // Double check that the queue is empty after the memory
        // barrier. In combination with the inverse double-check in
        // notify_n, this prevents any lost wakeups.
        for (size_t prio = 0; prio < PRIORITY_COUNT; ++prio) {
          if (!work_queues[prio].empty()) {
            thread_state.worker_status.store(
              WorkerStatus::Spinning, std::memory_order_relaxed
            );
            goto TOP;
          }
        }

        // No work found. Go to sleep.
        if (thread_stop_token.stop_requested()) [[unlikely]] {
          break;
        }
        thread_state.sleep_wait.wait(waitValue);
        thread_state.worker_status.store(
          WorkerStatus::Spinning, std::memory_order_relaxed
        );
      }

      // Thread stop has been requested (executor is shutting down)
      thread_state.worker_status.store(
        WorkerStatus::Spinning, std::memory_order_relaxed
      );
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
size_t ex_cpu_st::priority_count() {
  assert(is_initialized());
  return PRIORITY_COUNT;
}
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

ex_cpu_st& ex_cpu_st::set_thread_teardown_hook(std::function<void(size_t)> Hook
) {
  assert(!is_initialized());
  if (init_params == nullptr) {
    init_params = new InitParams;
  }
  init_params->thread_teardown_hook =
    static_cast<std::function<void(size_t)>&&>(Hook);
  return *this;
}

size_t ex_cpu_st::thread_count() {
  assert(is_initialized());
  return threads.size();
}

void ex_cpu_st::teardown() {
  bool expected = true;
  if (!initialized.compare_exchange_strong(expected, false)) {
    return;
  }

  for (size_t i = 0; i < threads.size(); ++i) {
    thread_stoppers[i].request_stop();
    thread_state.sleep_wait.fetch_add(1, std::memory_order_seq_cst);
    thread_state.sleep_wait.notify_one();
  }
  for (size_t i = 0; i < threads.size(); ++i) {
    threads[i].join();
  }
  threads.clear();
  thread_stoppers.clear();
  delete inbox;

  for (size_t i = 0; i < work_queues.size(); ++i) {
    delete[] work_queues[i].staticProducers;
  }
  work_queues.clear();
  delete[] task_stopper_bitsets;
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
