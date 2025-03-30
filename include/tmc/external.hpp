// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

// external.hpp provides functions to simplify safe integration of TMC with
// external coroutines, awaitables, and executors.

#include "tmc/detail/concepts.hpp" // IWYU pragma: keep
#include "tmc/detail/thread_locals.hpp"

#include <atomic>

namespace tmc {
namespace external {

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
inline void set_default_executor(tmc::detail::ex_any* Executor) {
  tmc::detail::g_ex_default.store(Executor, std::memory_order_release);
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
template <typename Exec> inline void set_default_executor(Exec& Executor) {
  tmc::detail::g_ex_default.store(
    tmc::detail::executor_traits<Exec>::type_erased(Executor),
    std::memory_order_release
  );
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
template <typename Exec> inline void set_default_executor(Exec* Executor) {
  tmc::detail::g_ex_default.store(
    tmc::detail::executor_traits<Exec>::type_erased(*Executor),
    std::memory_order_release
  );
}

} // namespace external
} // namespace tmc
