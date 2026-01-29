// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/task.hpp"

#include <coroutine>

namespace tmc {
namespace detail {

/// For internal usage only! To modify promises without taking ownership.
template <typename Result>
using task_unsafe = std::coroutine_handle<task_promise<Result>>;

template <typename Result>
struct awaitable_traits<tmc::detail::task_unsafe<Result>> {

  using result_type = Result;
  using self_type = tmc::detail::task_unsafe<Result>;
  using awaiter_type = tmc::aw_task<task<Result>, Result>;

  // Values controlling the behavior when awaited directly in a tmc::task
  static awaiter_type get_awaiter(self_type& Awaitable) noexcept {
    // deliberately convert this to task (not task_unsafe)
    return awaiter_type(tmc::task<Result>::from_address(Awaitable.address()));
  }

  // Values controlling the behavior when wrapped by a utility function
  // such as tmc::spawn_*()
  static constexpr configure_mode mode = TMC_TASK;
  static void set_result_ptr(
    self_type& Awaitable, tmc::detail::result_storage_t<Result>* ResultPtr
  ) noexcept {
    Awaitable.promise().customizer.result_ptr = ResultPtr;
  }

  static void
  set_continuation(self_type& Awaitable, void* Continuation) noexcept {
    Awaitable.promise().customizer.continuation = Continuation;
  }

  static void
  set_continuation_executor(self_type& Awaitable, void* ContExec) noexcept {
    Awaitable.promise().customizer.continuation_executor = ContExec;
  }

  static void set_done_count(self_type& Awaitable, void* DoneCount) noexcept {
    Awaitable.promise().customizer.done_count = DoneCount;
  }

  static void set_flags(self_type& Awaitable, size_t Flags) noexcept {
    Awaitable.promise().customizer.flags = Flags;
  }
};
} // namespace detail
} // namespace tmc
