// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/task.hpp"

#include <type_traits>

namespace tmc {
namespace detail {
template <class T, template <class...> class U>
inline constexpr bool is_instance_of_v = std::false_type{};

template <template <class...> class U, class... Vs>
inline constexpr bool is_instance_of_v<U<Vs...>, U> = std::true_type{};

struct not_found {};

/// begin task_result_t<T>
template <typename T>
concept HasTaskResult = requires { typename T::result_type; };
template <typename T> struct task_result_t_impl {
  using type = not_found;
};
template <HasTaskResult T> struct task_result_t_impl<T> {
  using type = typename T::result_type;
};
template <typename T>
using task_result_t = typename task_result_t_impl<T>::type;
/// end task_result_t<T>

/// begin func_result_t<T>
template <typename T>
concept HasFuncResult = requires { typename std::invoke_result_t<T>; };
template <typename T> struct func_result_t_impl {
  using type = not_found;
};
template <HasFuncResult T> struct func_result_t_impl<T> {
  using type = std::invoke_result_t<T>;
};
template <typename T>
using func_result_t = typename func_result_t_impl<T>::type;
/// end func_result_t<T>

// Can be converted to a `tmc::task<T::result_type>`
template <typename T>
concept is_task_v = std::is_convertible_v<T, task<task_result_t<T>>>;

// Can be converted to a `tmc::task<void>`
template <typename T>
concept is_task_void_v =
  std::is_convertible_v<T, task<void>> && std::is_void_v<task_result_t<T>>;

// Can be converted to a `tmc::task<T::result_type>` where `T::result_type` !=
// void
template <typename T>
concept is_task_nonvoid_v = std::is_convertible_v<T, task<task_result_t<T>>> &&
                            !std::is_void_v<task_result_t<T>>;

// Can be converted to a `tmc::task<Result>`
template <typename T, typename Result>
concept is_task_result_v = std::is_convertible_v<T, task<Result>>;

// A functor with `operator()()` that isn't a `tmc::task`
template <typename T>
concept is_func_v =
  !is_task_v<T> && !std::is_same_v<func_result_t<T>, not_found>;

// A functor with `void operator()()` that isn't a `tmc::task`
template <typename T>
concept is_func_void_v = !is_task_v<T> && std::is_void_v<func_result_t<T>>;

// A functor with `Result operator()()` that isn't a `tmc::task`, where Result
// != void
template <typename T>
concept is_func_nonvoid_v =
  !is_task_v<T> && !std::is_void_v<func_result_t<T>> &&
  !std::is_same_v<func_result_t<T>, not_found>;

// A functor with `Result operator()()` that isn't a `tmc::task`
template <typename T, typename Result>
concept is_func_result_v =
  !is_task_v<T> && std::is_same_v<func_result_t<T>, Result>;

/// Makes a task<Result> from a task<Result> or a Result(void)
/// functor.

template <typename Original, typename Result = Original::result_type>
  requires(is_task_result_v<Original, Result>)
task<Result> into_task(Original Task) noexcept {
  return Task;
}

template <typename Original, typename Result = std::invoke_result_t<Original>>
task<Result> into_task(Original FuncResult) noexcept
  requires(!std::is_void_v<Result> && is_func_result_v<Original, Result>)
{
  co_return FuncResult();
}

template <typename Original>
  requires(tmc::detail::is_func_void_v<Original>)
task<void> into_task(Original FuncVoid) noexcept {
  FuncVoid();
  co_return;
}

inline work_item into_work_item(task<void>&& Task) noexcept {
  return std::coroutine_handle<>(static_cast<task<void>&&>(Task));
}

template <typename Original>
  requires(tmc::detail::is_func_void_v<Original>)
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
