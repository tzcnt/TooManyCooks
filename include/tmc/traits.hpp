// Copyright (c) 2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/concepts_awaitable.hpp"
#include "tmc/detail/concepts_work_item.hpp"

namespace tmc {
namespace traits {
/// If this type is returned from a type trait, it indicates that the type trait
/// doesn't apply to whatever you're inspecting. For example, `unknown_t` will
/// be returned from `awaitable_result_t` if the provided type is not an
/// awaitable.
using unknown_t = tmc::detail::unknown_t;

/*******************************************************************
 * Traits used to distinguish data types, awaitables, or functors. *
 * For use when consuming data in a generic fashion.               *
 *******************************************************************/

/*** Awaitable Traits ***/

/// Constrains types to those that can be co_awaited.
/// This detection is based on the presence of `await_resume()`
/// or `operator co_await()` methods.
///
/// Note: For an unknown (non-TMC) awaitable, when passed template type T, this
/// will detect unqualified or rvalue-qualified member functions. If your
/// awaitable type only has lvalue-qualified `await_resume() &`
///  or `operator co_await() &`, you must pass template type T&.
/// See the `is_awaitable_unknown_*` tests for example:
/// https://github.com/tzcnt/tmc-examples/blob/main/tests/test_traits.cpp#L87
///
/// If this causes issues, the fix is to provide a specialization
/// of `tmc::detail::awaitable_traits` for your awaitable type.
template <typename T>
concept is_awaitable = tmc::detail::is_awaitable<T>;

/// Get the result type of the expression `co_await Awaitable{}`.
template <typename Awaitable>
using awaitable_result_t = tmc::detail::awaitable_result_t<Awaitable>;

/*** Callable Traits ***/

/// Constrains types to those that are callable and NOT awaitable.
/// Types must expose `operator()()` with no parameters, and not expose any
/// `await_*()` or `operator co_await()` functions.
template <typename T>
concept is_callable = tmc::detail::is_callable<T>;

/// Get the result type of calling `T::operator()()`.
/// This is derived using `std::invoke_result_t<T&>`.
template <typename Callable>
using callable_result_t = tmc::detail::func_result_t<Callable>;

/*** Executable (Awaitable OR Callable) Traits ***/

using executable_kind = tmc::detail::executable_kind;

/// - If T is an awaitable type, returns AWAITABLE.
/// - Else if T is a callable type, returns CALLABLE.
/// - Else returns UNKNOWN.
template <typename T>
static inline constexpr executable_kind executable_kind_v =
  tmc::detail::executable_kind_v<T>;

/// - If T is an awaitable type, returns `awaitable_result_t<T>`.
/// - Else if T is a callable type, returns `std::invoke_result_t<T&>`.
/// - Else returns `tmc::unknown_t`.
template <typename T>
using executable_result_t = tmc::detail::executable_result_t<T>;

/// Combines `executable_kind_v` and `executable_result_t`.
/// Returns a type with two fields:
/// `static inline constexpr executable_kind kind = executable_kind_v<T>`;
/// `using result_type = executable_result_t<T>`;
template <typename T>
using executable_traits = tmc::detail::executable_traits<T>;

/***********************************************
 * Traits used to distinguish work item types. *
 * For use when post()ing work.                *
 ***********************************************/

/*** Task Traits ***/

/// Constrains types to those convertible to `tmc::task<T::result_type>`.
template <typename T>
concept is_task = tmc::detail::is_task<T>;

/// Constrains types to those convertible to `tmc::task<void>`.
template <typename T>
concept is_task_void = tmc::detail::is_task_void<T>;

/// Constrains types to those convertible to `tmc::task<T::result_type>`
/// where `T::result_type` is not void.
template <typename T>
concept is_task_nonvoid = tmc::detail::is_task_nonvoid<T>;

/// Constrains types to those convertible to `tmc::task<Result>`.
template <typename T, typename Result>
concept is_task_result = tmc::detail::is_task_result<T, Result>;

/*** Functor Traits ***/

/// Constrains types to functors with `operator()()` that aren't a `tmc::task`.
template <typename T>
concept is_func = tmc::detail::is_func<T>;

/// Constrains types to functors with `void operator()()` that aren't a
/// `tmc::task`.
template <typename T>
concept is_func_void = tmc::detail::is_func_void<T>;

/// Constrains types to functors with `Result operator()()` (Result != void)
/// that aren't a `tmc::task`.
template <typename T>
concept is_func_nonvoid = tmc::detail::is_func_nonvoid<T>;

/// Constrains types to functors with `Result operator()()` that aren't a
/// `tmc::task`.
template <typename T, typename Result>
concept is_func_result = tmc::detail::is_func_result<T, Result>;

} // namespace traits
} // namespace tmc
