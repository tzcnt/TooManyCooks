// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/compat.hpp"
#include "tmc/detail/concepts_awaitable.hpp"
#include "tmc/detail/mixins.hpp"
#include "tmc/ex_any.hpp"
#include "tmc/task.hpp"

#include <coroutine>
#include <functional>
#include <type_traits>
#include <utility>

namespace tmc {
namespace detail {

template <typename A, typename Result>
decltype(auto) get_awaitable_result(A&& awaiter) {
  if constexpr (std::is_void_v<Result>) {
    awaiter.await_resume();
  } else {
    return awaiter.await_resume();
  }
}

template <typename Awaitable, typename Func, typename Result>
tmc::task<Result> and_then_bridge(
  Awaitable&& awaitable, Func&& func, tmc::ex_any* run_executor,
  tmc::ex_any* resume_executor
) {
  using awaitable_result = typename tmc::detail::get_awaitable_traits<
    std::remove_reference_t<Awaitable>>::result_type;

  if constexpr (std::is_void_v<awaitable_result>) {
    co_await std::move(awaitable);
    auto next = std::invoke(static_cast<Func&&>(func));
    if constexpr (std::is_void_v<Result>) {
      co_await std::move(next);
      co_return;
    } else {
      co_return co_await std::move(next);
    }
  } else {
    decltype(auto) first_result = co_await std::move(awaitable);
    auto next = std::invoke(static_cast<Func&&>(func), static_cast<decltype(first_result)>(first_result));
    if constexpr (std::is_void_v<Result>) {
      co_await std::move(next);
      co_return;
    } else {
      co_return co_await std::move(next);
    }
  }
}

template <typename Awaitable, typename Func, typename Result>
class aw_and_then_impl {
  tmc::task<Result> bridge_task;
  
  struct empty {};
  using result_storage = std::conditional_t<
    std::is_void_v<Result>, empty, tmc::detail::result_storage_t<Result>>;
  TMC_NO_UNIQUE_ADDRESS result_storage result;

public:
  aw_and_then_impl(
    Awaitable&& Aw, Func&& F, tmc::ex_any* RunExec, tmc::ex_any* ResumeExec
  ) noexcept
      : bridge_task(and_then_bridge<Awaitable, Func, Result>(
          static_cast<Awaitable&&>(Aw), static_cast<Func&&>(F), RunExec,
          ResumeExec
        )) {
    if (ResumeExec != nullptr) {
      bridge_task.resume_on(ResumeExec);
    }
  }

  inline bool await_ready() const noexcept { return bridge_task.done(); }

  inline std::coroutine_handle<>
  await_suspend(std::coroutine_handle<> Outer) noexcept {
    tmc::detail::get_awaitable_traits<tmc::task<Result>>::set_continuation(
      bridge_task, Outer.address()
    );
    if constexpr (!std::is_void_v<Result>) {
      tmc::detail::get_awaitable_traits<tmc::task<Result>>::set_result_ptr(
        bridge_task, &result
      );
    }
    return std::move(bridge_task);
  }

  inline std::add_rvalue_reference_t<Result> await_resume() noexcept
    requires(!std::is_void_v<Result>)
  {
    if constexpr (std::is_default_constructible_v<Result>) {
      return std::move(result);
    } else {
      return *std::move(result);
    }
  }

  inline void await_resume() noexcept
    requires(std::is_void_v<Result>)
  {}
};

} // namespace detail

template <typename Awaitable, typename Func, typename Result>
class [[nodiscard(
  "You must co_await aw_and_then."
)]] aw_and_then
    : public tmc::detail::run_on_mixin<aw_and_then<Awaitable, Func, Result>>,
      public tmc::detail::resume_on_mixin<aw_and_then<Awaitable, Func, Result>>,
      public tmc::detail::with_priority_mixin<
        aw_and_then<Awaitable, Func, Result>> {
  friend class tmc::detail::run_on_mixin<aw_and_then<Awaitable, Func, Result>>;
  friend class tmc::detail::resume_on_mixin<
    aw_and_then<Awaitable, Func, Result>>;
  friend class tmc::detail::with_priority_mixin<
    aw_and_then<Awaitable, Func, Result>>;

  Awaitable awaitable;
  Func func;
  tmc::ex_any* executor;
  tmc::ex_any* continuation_executor;
  size_t prio;

public:
  aw_and_then(Awaitable&& Aw, Func&& F) noexcept
      : awaitable(static_cast<Awaitable&&>(Aw)),
        func(static_cast<Func&&>(F)),
        executor(tmc::detail::this_thread::executor),
        continuation_executor(tmc::detail::this_thread::executor),
        prio(tmc::detail::this_thread::this_task.prio) {}

  tmc::detail::aw_and_then_impl<Awaitable, Func, Result>
  operator co_await() && noexcept {
    return tmc::detail::aw_and_then_impl<Awaitable, Func, Result>(
      static_cast<Awaitable&&>(awaitable), static_cast<Func&&>(func),
      executor, continuation_executor
    );
  }

  template <typename Func2, typename NextAwaitable = decltype(std::declval<Func2>()(std::declval<Result>()))>
  auto and_then(Func2&& F) && noexcept {
    using next_result = typename tmc::detail::get_awaitable_traits<NextAwaitable>::result_type;
    return aw_and_then<aw_and_then<Awaitable, Func, Result>, std::decay_t<Func2>, next_result>(
      static_cast<aw_and_then&&>(*this), static_cast<Func2&&>(F)
    );
  }

  aw_and_then(const aw_and_then&) = delete;
  aw_and_then& operator=(const aw_and_then&) = delete;
  aw_and_then(aw_and_then&& Other) noexcept
      : awaitable(static_cast<Awaitable&&>(Other.awaitable)),
        func(static_cast<Func&&>(Other.func)),
        executor(Other.executor),
        continuation_executor(Other.continuation_executor), prio(Other.prio) {}
  aw_and_then& operator=(aw_and_then&& Other) noexcept {
    awaitable = static_cast<Awaitable&&>(Other.awaitable);
    func = static_cast<Func&&>(Other.func);
    executor = Other.executor;
    continuation_executor = Other.continuation_executor;
    prio = Other.prio;
    return *this;
  }
};

template <typename Awaitable, typename Func, bool IsVoid>
struct compute_then_result_impl;

template <typename Awaitable, typename Func>
struct compute_then_result_impl<Awaitable, Func, true> {
  using invoke_result = decltype(std::declval<Func>()());
  using type = typename tmc::detail::get_awaitable_traits<invoke_result>::result_type;
};

template <typename Awaitable, typename Func>
struct compute_then_result_impl<Awaitable, Func, false> {
  using awaitable_result = typename tmc::detail::get_awaitable_traits<Awaitable>::result_type;
  using invoke_result = decltype(std::declval<Func>()(std::declval<awaitable_result>()));
  using type = typename tmc::detail::get_awaitable_traits<invoke_result>::result_type;
};

template <typename Awaitable, typename Func>
struct compute_then_result {
  using awaitable_result = typename tmc::detail::get_awaitable_traits<Awaitable>::result_type;
  using type = typename compute_then_result_impl<
    Awaitable, Func, std::is_void_v<awaitable_result>>::type;
};

template <typename Awaitable, typename Func>
auto and_then(Awaitable&& Aw, Func&& F) noexcept {
  using result_type = typename compute_then_result<
    std::remove_reference_t<Awaitable>, std::decay_t<Func>>::type;
  
  return aw_and_then<
    tmc::detail::forward_awaitable<Awaitable>, std::decay_t<Func>, result_type>(
    static_cast<Awaitable&&>(Aw), static_cast<Func&&>(F)
  );
}

namespace detail {

template <typename Awaitable, typename Func, typename Result>
struct awaitable_traits<aw_and_then<Awaitable, Func, Result>> {
  static constexpr configure_mode mode = WRAPPER;

  using result_type = Result;
  using self_type = aw_and_then<Awaitable, Func, Result>;
  using awaiter_type = aw_and_then_impl<Awaitable, Func, Result>;

  static awaiter_type get_awaiter(self_type&& awaitable) noexcept {
    return static_cast<self_type&&>(awaitable).operator co_await();
  }
};

} // namespace detail
} // namespace tmc
