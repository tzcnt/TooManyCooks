// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/compat.hpp"
#include "tmc/detail/concepts_awaitable.hpp"
#include "tmc/task.hpp"

#include <type_traits>

namespace tmc {
namespace detail {
/// begin task_result_t<T>
template <typename T>
concept HasTaskResult = requires { typename T::result_type; };
template <typename T> struct task_result_t_impl {
  using type = unknown_t;
};
template <HasTaskResult T> struct task_result_t_impl<T> {
  using type = typename T::result_type;
};
template <typename T>
using task_result_t = typename task_result_t_impl<T>::type;
/// end task_result_t<T>

/// begin func_result_t<T>
template <typename T>
concept HasFuncResult = requires { typename std::invoke_result_t<T&>; };
template <typename T> struct func_result_t_impl {
  using type = unknown_t;
};
template <HasFuncResult T> struct func_result_t_impl<T> {
  using type = std::invoke_result_t<T&>;
};
template <typename T>
using func_result_t = typename func_result_t_impl<T>::type;
/// end func_result_t<T>

// Can be converted to a `tmc::task<T::result_type>`
template <typename T>
concept is_task = std::is_convertible_v<T, task<task_result_t<T>>>;

// Can be converted to a `tmc::task<void>`
template <typename T>
concept is_task_void =
  std::is_convertible_v<T, task<void>> && std::is_void_v<task_result_t<T>>;

// Can be converted to a `tmc::task<T::result_type>` where `T::result_type` !=
// void
template <typename T>
concept is_task_nonvoid = std::is_convertible_v<T, task<task_result_t<T>>> &&
                          !std::is_void_v<task_result_t<T>>;

// Can be converted to a `tmc::task<Result>`
template <typename T, typename Result>
concept is_task_result = std::is_convertible_v<T, task<Result>>;

// A functor with `operator()()` that isn't a `tmc::task`
template <typename T>
concept is_func = !is_task<T> && !std::is_same_v<func_result_t<T>, unknown_t>;

// A functor with `void operator()()` that isn't a `tmc::task`
template <typename T>
concept is_func_void = !is_task<T> && std::is_void_v<func_result_t<T>>;

// A functor with `Result operator()()` that isn't a `tmc::task`, where Result
// != void
template <typename T>
concept is_func_nonvoid = !is_task<T> && !std::is_void_v<func_result_t<T>> &&
                          !std::is_same_v<func_result_t<T>, unknown_t>;

// A functor with `Result operator()()` that isn't a `tmc::task`
template <typename T, typename Result>
concept is_func_result =
  !is_task<T> && std::is_same_v<func_result_t<T>, Result>;

/// begin call_or_await_result_t<T>

/// If a type is both awaitable and callable, the awaitable result takes
/// precedence.
template <typename T>
concept is_callable = !is_awaitable<T> && HasFuncResult<T>;

/// begin executable_kind<T>
enum class executable_kind { UNKNOWN, AWAITABLE, CALLABLE };
template <typename T> struct executable_kind_v_impl {
  static inline constexpr executable_kind value = executable_kind::UNKNOWN;
};
template <is_awaitable T> struct executable_kind_v_impl<T> {
  static inline constexpr executable_kind value = executable_kind::AWAITABLE;
};
template <is_callable T> struct executable_kind_v_impl<T> {
  static inline constexpr executable_kind value = executable_kind::CALLABLE;
};
template <typename T>
TMC_STATIC_LINKAGE constexpr executable_kind executable_kind_v =
  executable_kind_v_impl<T>::value;
/// end executable_kind<T>

/// begin executable_result_t<T>
template <typename T> struct executable_result_t_impl {
  using type = tmc::detail::unknown_t;
};
template <is_awaitable T> struct executable_result_t_impl<T> {
  using type = awaitable_result_t<T>;
};
template <is_callable T> struct executable_result_t_impl<T> {
  using type = std::invoke_result_t<T&>;
};
template <typename T>
using executable_result_t = typename executable_result_t_impl<T>::type;
/// end executable_result_t<T>

template <typename T> struct executable_traits {
  static inline constexpr executable_kind kind = executable_kind_v<T>;
  using result_type = executable_result_t<T>;
};

/// Makes a task<Result> from a task<Result> or a Result(void)
/// functor.
template <typename Original, typename Result = Original::result_type>
  requires(is_task_result<Original, Result>)
task<Result> into_task(Original Task) noexcept {
  return Task;
}

template <typename Original, typename Result = std::invoke_result_t<Original&>>
task<Result> into_task(Original FuncResult) noexcept
  requires(!std::is_void_v<Result> && is_func_result<Original, Result>)
{
  co_return FuncResult();
}

template <typename Original>
  requires(tmc::detail::is_func_void<Original>)
task<void> into_task(Original FuncVoid) noexcept {
  FuncVoid();
  co_return;
}

// Ensures Item is of a known awaitable type,
// that is, after calling this, the mode will not be WRAPPER.
template <bool IsFunc, typename Awaitable>
inline decltype(auto) into_known(Awaitable&& Item) {
  if constexpr (IsFunc) {
    return into_task(static_cast<Awaitable&&>(Item));
  } else {
    constexpr auto mode = tmc::detail::get_awaitable_traits<Awaitable>::mode;
    static_assert(
      mode != tmc::detail::UNKNOWN, "This doesn't appear to be an awaitable."
    );

    if constexpr (mode == TMC_TASK || mode == COROUTINE ||
                  mode == ASYNC_INITIATE) {
      return static_cast<Awaitable&&>(Item);
    } else { // WRAPPER
      return tmc::detail::safe_wrap(static_cast<Awaitable&&>(Item));
    }
  }
}

// Converts k into a coroutine handle, then into a work_item.
// This type erasure is necessary when TMC_WORK_ITEM=FUNC,
// so that func.target <std::coroutine_handle<>>() works. Otherwise,
// the func target would be of the real type (tmc::task).
template <typename Known>
tmc::work_item into_initiate(Known&& k)
  requires(
    tmc::detail::get_awaitable_traits<Known>::mode == TMC_TASK ||
    tmc::detail::get_awaitable_traits<Known>::mode == COROUTINE
  )
{
  return std::coroutine_handle<>(static_cast<Known&&>(k));
}

// Returns the ASYNC_INITIATE type unchanged, so that it can be initiated.
template <typename Known>
Known&& into_initiate(Known&& k)
  requires(tmc::detail::get_awaitable_traits<Known>::mode == ASYNC_INITIATE)
{
  return static_cast<Known&&>(k);
}

// todo how is task<Result> handled by caller?
inline work_item into_work_item(task<void>&& Task) noexcept {
  return std::coroutine_handle<>(static_cast<task<void>&&>(Task));
}

template <typename Original>
  requires(tmc::detail::is_func_void<Original>)
work_item into_work_item(Original&& FuncVoid) noexcept {
#if TMC_WORK_ITEM_IS(CORO)
  return std::coroutine_handle<>([](Original f) -> task<void> {
    f();
    co_return;
  }(static_cast<Original&&>(FuncVoid)));
#else
  return FuncVoid;
#endif
}
} // namespace detail
} // namespace tmc
