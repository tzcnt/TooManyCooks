// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

// Implementation definition file for tmc::ex_braid. This will be included
// anywhere TMC_IMPL is defined. If you prefer to manually separate compilation
// units, you can instead include this file directly in a CPP file.

#pragma once

#include "tmc/channel.hpp"
#include "tmc/detail/impl.hpp" // IWYU pragma: keep
#include "tmc/detail/thread_locals.hpp"
#include "tmc/ex_any.hpp"
#include "tmc/ex_braid.hpp"
#include "tmc/work_item.hpp"

#include <atomic>
#include <coroutine>

namespace tmc {
tmc::task<void> ex_braid::run_loop(
  tmc::chan_tok<tmc::detail::braid_work_item, tmc::detail::braid_chan_config>
    Chan
) {
  auto parentExecutor = tmc::detail::this_thread::executor();
  auto parentTask = tmc::detail::this_thread::this_task();

  // Set the thread context at the beginning so the precondition for entering
  // the below loop is "thread context was set by prior iteration". This makes
  // the initial suspension a bit less efficient, but simplifies future passes.
  tmc::detail::this_thread::this_task().yield_priority =
    &tmc::detail::never_yield;
  tmc::detail::this_thread::executor() = &type_erased_this;

  while (true) {
    if (auto started = Chan.start_pull_zc()) {
      auto data = co_await std::move(started).pull_zc();
      if (!data) {
        break;
      }
      auto& item = data->get();
      tmc::detail::this_thread::this_task().prio = item.prio;
      item.item();
    } else {
      // Restore the thread context before possibly suspending
      tmc::detail::this_thread::this_task() = parentTask;
      tmc::detail::this_thread::executor() = parentExecutor;

      // Expect this to suspend since started == false
      auto data = co_await std::move(started).pull_zc();
      if (!data) {
        break;
      }
      auto& item = data->get();

      // Refresh the parent thread-local snapshot since we may resume on a
      // different thread. However the executor remains the same since ex_braid
      // is bound to a single parent executor.
      parentTask = tmc::detail::this_thread::this_task();

      // Now we need to set the thread context before executing.
      tmc::detail::this_thread::this_task().yield_priority =
        &tmc::detail::never_yield;
      tmc::detail::this_thread::executor() = &type_erased_this;
      tmc::detail::this_thread::this_task().prio = item.prio;
      item.item();
    }
  }

  // Unconditionally restore the thread context before exiting.
  tmc::detail::this_thread::this_task() = parentTask;
  tmc::detail::this_thread::executor() = parentExecutor;
}

void ex_braid::post(
  work_item&& Item, size_t Priority, [[maybe_unused]] size_t ThreadHint
) {
  // This may be called from multiple threads. Thus, each call must
  // maintain its own refcount / hazard pointer.
  auto tok = queue.new_token();
  tok.post(
    tmc::detail::braid_work_item{static_cast<work_item&&>(Item), Priority}
  );
}

ex_braid::ex_braid(tmc::ex_any* Parent)
    : queue{tmc::make_channel<
        tmc::detail::braid_work_item, tmc::detail::braid_chan_config>()},
      type_erased_this(this) {
  if (Parent == nullptr) {
    Parent = tmc::detail::g_ex_default.load(std::memory_order_acquire);
  }
  Parent->post(run_loop(queue));
}

ex_braid::ex_braid() : ex_braid(tmc::detail::this_thread::executor()) {}

ex_braid::~ex_braid() { queue.drain_wait(); }

/// Post this task to the braid queue, and attempt to take the lock and
/// start executing tasks on the braid.
std::coroutine_handle<>
ex_braid::dispatch(std::coroutine_handle<> Outer, size_t Priority) {
  // This may be called from multiple threads. Thus, each call must
  // maintain its own refcount / hazard pointer.
  auto tok = queue.new_token();
  tok.post(tmc::detail::braid_work_item{std::move(Outer), Priority});
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

std::coroutine_handle<> executor_traits<tmc::ex_braid>::dispatch(
  tmc::ex_braid& ex, std::coroutine_handle<> Outer, size_t Priority
) {
  return ex.dispatch(Outer, Priority);
}

} // namespace detail
} // namespace tmc
