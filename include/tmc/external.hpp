// Copyright (c) 2023-2024 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

// external.hpp provides functions to simplify safe integration of TMC with
// external coroutines, awaitables, and executors.

#include "tmc/aw_resume_on.hpp"
#include "tmc/detail/concepts.hpp" // IWYU pragma: keep
#include "tmc/detail/thread_locals.hpp"
#include "tmc/task.hpp"

#include <atomic>

namespace tmc {
namespace external {
/// Saves the current TMC executor and priority level before awaiting the
/// provided awaitable. After the awaitable completes, returns the awaiting task
/// back to the saved executor / priority.
///
/// Use of this function isn't *strictly* necessary, if you are sure that an
/// external awaitable won't lasso your task onto a different executor.
template <typename Result, typename ExternalAwaitable>
[[nodiscard("You must await the return type of safe_await()"
)]] tmc::task<Result>
safe_await(ExternalAwaitable&& Awaitable) {
  return [](
           ExternalAwaitable ExAw, tmc::aw_resume_on TakeMeHome
         ) -> tmc::task<Result> {
    auto result = co_await ExAw;
    co_await TakeMeHome;
    co_return result;
  }(static_cast<ExternalAwaitable&&>(Awaitable),
           tmc::resume_on(tmc::detail::this_thread::executor));
}

/// You only need to set this if you are planning to integrate TMC with external
/// threads of execution that don't configure
/// `tmc::detail::this_thread::executor`.
///
/// If a TMC function that submits work to the current executor implicitly, such
/// as `spawn()`, is invoked:
/// - on a non-TMC thread that has not set `tmc::detail::this_thread::executor`
/// - without explicitly specifying an executor via `.run_on()` / `.resume_on()`
///
/// then that function will use this default executor (instead of deferencing
/// nullptr and crashing).
inline void set_default_executor(detail::type_erased_executor* Executor) {
  detail::g_ex_default.store(Executor, std::memory_order_release);
}
/// You only need to set this if you are planning to integrate TMC with external
/// threads of execution that don't configure
/// `tmc::detail::this_thread::executor`.
///
/// If a TMC function that submits work to the current executor implicitly, such
/// as `spawn()`, is invoked:
/// - on a non-TMC thread that has not set `tmc::detail::this_thread::executor`
/// - without explicitly specifying an executor via `.run_on()` / `.resume_on()`
///
/// then that function will use this default executor (instead of deferencing
/// nullptr and crashing).
template <detail::TypeErasableExecutor Exec>
inline void set_default_executor(Exec& Executor) {
  detail::g_ex_default.store(Executor.type_erased(), std::memory_order_release);
}
/// You only need to set this if you are planning to integrate TMC with external
/// threads of execution that don't configure
/// `tmc::detail::this_thread::executor`.
///
/// If a TMC function that submits work to the current executor implicitly, such
/// as `spawn()`, is invoked:
/// - on a non-TMC thread that has not set `tmc::detail::this_thread::executor`
/// - without explicitly specifying an executor via `.run_on()` / `.resume_on()`
///
/// then that function will use this default executor (instead of deferencing
/// nullptr and crashing).
template <detail::TypeErasableExecutor Exec>
inline void set_default_executor(Exec* Executor) {
  detail::g_ex_default.store(
    Executor->type_erased(), std::memory_order_release
  );
}

} // namespace external
} // namespace tmc
