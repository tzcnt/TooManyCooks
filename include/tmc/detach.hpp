// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/current.hpp"
#include "tmc/detail/concepts_awaitable.hpp" // IWYU pragma: keep
#include "tmc/fork_group.hpp"
#include "tmc/task.hpp"

namespace tmc {
inline tmc::aw_fork_group<0, void> g_detached_tasks;

/// Waits for any detached tasks to finish. It is recommended, but not required,
/// to await this function before the end of your program if you have run
/// any detached tasks. If you use `tmc::async_main`, it will do this for you
/// automatically.
///
/// You can await this earlier in your program any number of times, if you need
/// to synchronize with detached tasks, but you must not await when
/// new detached tasks might be created simultaneously.
///
/// Detached tasks can be created with `tmc::detach()`, or `.detach()` members.
inline tmc::task<void> wait_for_detached() {
  co_await static_cast<tmc::aw_fork_group<0, void>&&>(g_detached_tasks);
  g_detached_tasks.reset();
}

/// Submits the awaitable to the executor immediately.
/// The awaitable's return type must be `void`.
/// It cannot be directly awaited, but all detached tasks share a global group,
/// which can optionally be awaited with `co_await tmc::wait_for_detached()`.
template <typename Awaitable, typename Exec = tmc::ex_any*>
void detach(
  Awaitable&& Aw, Exec&& Executor = tmc::current_executor(),
  size_t Priority = tmc::current_priority()
)
  requires std::is_void_v<tmc::detail::awaitable_result_t<Awaitable>>
{
  g_detached_tasks.fork(
    static_cast<Awaitable&&>(Aw), static_cast<Exec&&>(Executor), Priority
  );
}
} // namespace tmc
