// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#include "tmc/detail/qu_mpsc.hpp"
#include "tmc/detail/thread_locals.hpp"
#include "tmc/ex_any.hpp"
#include "tmc/ex_manual_st.hpp"
#include "tmc/work_item.hpp"

#include <cassert>
#include <coroutine>

namespace tmc {

bool ex_manual_st::is_initialized() {
  return initialized.load(std::memory_order_relaxed);
}

bool ex_manual_st::try_get_work(work_item& Item, size_t& Prio) {
  Prio = 0;
  for (; Prio < PRIORITY_COUNT; ++Prio) {
    if (!private_work[Prio].empty()) {
      Item = std::move(private_work[Prio].back());
      private_work[Prio].pop_back();
      return true;
    }
    if (work_queues[Prio].try_pull(Item)) {
      return true;
    }
  }
  return false;
}

size_t ex_manual_st::run_n(const size_t MaxCount) {
  assert(MaxCount != 0);
  size_t count = 0;
  work_item item;
  size_t prio;

  if (!try_get_work(item, prio)) {
    return count;
  }

  auto storedContext = tmc::detail::this_thread::this_task;
  auto storedExecutor = tmc::detail::this_thread::executor;
  tmc::detail::this_thread::this_task.yield_priority = &yield_priority;
  tmc::detail::this_thread::executor = &type_erased_this;

  do {
    ++count;
    tmc::detail::this_thread::this_task.prio = prio;
    yield_priority.store(prio, std::memory_order_relaxed);

    item();
  } while (count < MaxCount && try_get_work(item, prio));

  yield_priority.store(0);
  tmc::detail::this_thread::this_task = storedContext;
  tmc::detail::this_thread::executor = storedExecutor;
  return count;
}

size_t ex_manual_st::run_all() { return run_n(TMC_ALL_ONES); }

bool ex_manual_st::run_one() { return run_n(1) != 0; }

bool ex_manual_st::empty() {
  for (size_t prio = 0; prio < PRIORITY_COUNT; ++prio) {
    if (!private_work[prio].empty()) {
      return false;
    }
    if (!work_queues[prio].empty()) {
      return false;
    }
  }
  return true;
}

void ex_manual_st::notify_n(size_t Priority) {
  // Request a task to suspend, if new task priority is higher
  if TMC_PRIORITY_CONSTEXPR (PRIORITY_COUNT > 1) {
    auto currentPrio = yield_priority.load(std::memory_order_relaxed);
    // 2 threads may request a task to yield at the same time. The
    // thread with the higher priority (lower priority index) should
    // prevail.
    while (currentPrio > Priority) {
      if (yield_priority.compare_exchange_strong(
            currentPrio, Priority, std::memory_order_acq_rel
          )) {
        return;
      }
    }
  }
}

void ex_manual_st::clamp_priority(size_t& Priority) {
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

void ex_manual_st::post(work_item&& Item, size_t Priority, size_t ThreadHint) {
  clamp_priority(Priority);
  bool fromExecThread = tmc::detail::this_thread::executor == &type_erased_this;
  if (fromExecThread && ThreadHint != 0) [[likely]] {
    private_work[Priority].push_back(static_cast<work_item&&>(Item));
    notify_n(Priority);
  } else {
    auto handle = work_queues[Priority].get_hazard_ptr();
    auto& haz = handle.value;
    work_queues[Priority].post(&haz, static_cast<work_item&&>(Item));
    notify_n(Priority);
    // Hold the handle until after notify_n() to prevent race
    // with destructor on another thread
    handle.release();
  }
}

tmc::ex_any* ex_manual_st::type_erased() { return &type_erased_this; }

// Default constructor does not call init() - you need to do it afterward
ex_manual_st::ex_manual_st()
    : init_params{nullptr}, type_erased_this(this)
#ifndef TMC_PRIORITY_COUNT
      ,
      PRIORITY_COUNT{1}
#endif
{
  initialized.store(false, std::memory_order_seq_cst);
}

void ex_manual_st::init() {
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
#endif

  work_queues.resize(PRIORITY_COUNT);
  for (size_t i = 0; i < PRIORITY_COUNT; ++i) {
    work_queues.emplace_at(i);
  }

  private_work.resize(PRIORITY_COUNT);
  for (size_t i = 0; i < PRIORITY_COUNT; ++i) {
    private_work.emplace_at(i);
  }

  yield_priority = 0;

  if (init_params != nullptr) {
    delete init_params;
    init_params = nullptr;
  }
}

#ifndef TMC_PRIORITY_COUNT
ex_manual_st& ex_manual_st::set_priority_count(size_t PriorityCount) {
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
size_t ex_manual_st::priority_count() { return PRIORITY_COUNT; }
#endif

void ex_manual_st::teardown() {
  bool expected = true;
  if (!initialized.compare_exchange_strong(expected, false)) {
    return;
  }

  work_queues.clear();
  private_work.clear();
}

ex_manual_st::~ex_manual_st() { teardown(); }

std::coroutine_handle<> ex_manual_st::task_enter_context(
  std::coroutine_handle<> Outer, size_t Priority
) {
  if (tmc::detail::this_thread::exec_prio_is(&type_erased_this, Priority)) {
    return Outer;
  } else {
    post(static_cast<std::coroutine_handle<>&&>(Outer), Priority);
    return std::noop_coroutine();
  }
}

namespace detail {

void executor_traits<tmc::ex_manual_st>::post(
  tmc::ex_manual_st& ex, tmc::work_item&& Item, size_t Priority,
  size_t ThreadHint
) {
  ex.post(static_cast<tmc::work_item&&>(Item), Priority, ThreadHint);
}

tmc::ex_any*
executor_traits<tmc::ex_manual_st>::type_erased(tmc::ex_manual_st& ex) {
  return ex.type_erased();
}

std::coroutine_handle<> executor_traits<tmc::ex_manual_st>::task_enter_context(
  tmc::ex_manual_st& ex, std::coroutine_handle<> Outer, size_t Priority
) {
  return ex.task_enter_context(Outer, Priority);
}

} // namespace detail
} // namespace tmc
