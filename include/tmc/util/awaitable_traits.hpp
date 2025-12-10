// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/concepts_awaitable.hpp"
#include "tmc/detail/concepts_work_item.hpp"

#include <type_traits>

namespace tmc {
namespace util {
/************** Awaitable Traits **************/

/// Constrains types to those that are known to be awaitable by TMC.
template <typename T>
concept IsAwaitable = tmc::detail::IsAwaitable<T>;

/// Returns true if TMC can await this type.
template <typename T>
static inline constexpr bool is_awaitable_v = tmc::detail::is_awaitable_v<T>;

/// Get the result type of the expression `co_await Awaitable{}`.
template <typename Awaitable>
using awaitable_result_t = tmc::detail::awaitable_result_t<Awaitable>;

/************** Callable Traits **************/

/// Constrains types to those that are callable and NOT awaitable.
/// To be callable, it must expose `operator()()` with no parameters.
template <typename T>
concept IsCallableOnly = tmc::detail::CallableOnly<T>;

/// Returns true if this type is callable and NOT awaitable.
/// To be callable, it must expose `operator()()` with no parameters.
template <typename T>
using is_callable_only_v = tmc::detail::is_callable_only_v<T>;

/// Get the result type of calling `T()`.
/// This is derived using `std::invoke_result_t<T>`.
template <typename Awaitable>
using callable_result_t = tmc::detail::func_result_t<Awaitable>;

/************** Executable (Awaitable OR Callable) Traits **************/

using unknown_result = tmc::detail::not_found;
using executable_kind = tmc::detail::executable_kind;

/// If T is an awaitable type, returns AWAITABLE.
/// Else if T is a callable type, returns CALLABLE.
/// Else returns UNKNOWN.
template <typename T>
static inline constexpr executable_kind executable_kind_v =
  tmc::detail::executable_kind_v<T>;

/// If T is an awaitable type, returns `awaitable_result_t<T>`.
/// Else if T is a callable type, returns `std::invoke_result_T<T>`.
/// Else returns `tmc::util::unknown_result`.
template <typename T>
using executable_result_t = tmc::detail::executable_result_t<T>;

/// Combines `executable_kind_v` and `executable_result_t`.
/// Returns a type with two fields:
/// `static inline constexpr executable_kind kind = executable_kind_v<T>`;
/// `using result_type = executable_result_t<T>`;
template <typename T>
using executable_traits = tmc::detail::executable_traits<T>;

} // namespace util
} // namespace tmc
