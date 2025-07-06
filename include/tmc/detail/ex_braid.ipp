// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

// Implementation definition file for tmc::ex_braid. This will be included
// anywhere TMC_IMPL is defined. If you prefer to manually separate compilation
// units, you can instead include this file directly in a CPP file.

#include "tmc/channel.hpp"
#include "tmc/detail/thread_locals.hpp"
#include "tmc/ex_any.hpp"
#include "tmc/ex_braid.hpp"
#include "tmc/work_item.hpp"

#include <atomic>
#include <coroutine>
#include <memory>

namespace tmc {
tmc::task<void> ex_braid::run_loop(
  tmc::chan_tok<tmc::detail::braid_work_item, tmc::detail::braid_chan_config>
    Chan
) {
  auto parentExecutor = tmc::detail::this_thread::executor;
  while (true) {
    auto data = co_await Chan.pull();
    if (!data.has_value()) {
      co_return;
    }

    auto& item = data.value();

    auto oldYieldPrio = tmc::detail::this_thread::this_task.yield_priority;
    tmc::detail::this_thread::this_task.prio = item.prio;
    tmc::detail::this_thread::this_task.yield_priority =
      &tmc::detail::never_yield;
    tmc::detail::this_thread::executor = &type_erased_this;

    item.item();

    // Don't need to reset prio - it should be set by the parent executor before
    // running any work from its own queue.
    tmc::detail::this_thread::this_task.yield_priority = oldYieldPrio;
    tmc::detail::this_thread::executor = parentExecutor;
  }
}

void ex_braid::post(
  work_item&& Item, size_t Priority, [[maybe_unused]] size_t ThreadHint
) {
  auto* haz = queue->get_hazard_ptr();
  queue->post(
    haz, tmc::detail::braid_work_item{static_cast<work_item&&>(Item), Priority}
  );
  haz->release_ownership();
}

ex_braid::ex_braid(tmc::ex_any* Parent) : type_erased_this(this) {
  if (Parent == nullptr) {
    Parent = tmc::detail::g_ex_default.load(std::memory_order_acquire);
  }
  auto chan = tmc::make_channel<
    tmc::detail::braid_work_item, tmc::detail::braid_chan_config>();
  queue = chan.get_raw_channel_ptr();
  Parent->post(run_loop(chan));
}

ex_braid::ex_braid() : ex_braid(tmc::detail::this_thread::executor) {}

ex_braid::~ex_braid() { queue->drain_wait(); }

/// Post this task to the braid queue, and attempt to take the lock and
/// start executing tasks on the braid.
std::coroutine_handle<>
ex_braid::task_enter_context(std::coroutine_handle<> Outer, size_t Priority) {
  auto* haz = queue->get_hazard_ptr();
  queue->post(haz, tmc::detail::braid_work_item{std::move(Outer), Priority});
  haz->release_ownership();
  return std::noop_coroutine();
}

namespace detail {

void executor_traits<tmc::ex_braid>::post(
  tmc::ex_braid& ex, tmc::work_item&& Item, size_t Priority, size_t ThreadHint
) {
  ex.post(std::move(Item), Priority, ThreadHint);
}

tmc::ex_any* executor_traits<tmc::ex_braid>::type_erased(tmc::ex_braid& ex) {
  return ex.type_erased();
}

std::coroutine_handle<> executor_traits<tmc::ex_braid>::task_enter_context(
  tmc::ex_braid& ex, std::coroutine_handle<> Outer, size_t Priority
) {
  return ex.task_enter_context(Outer, Priority);
}

} // namespace detail
} // namespace tmc
