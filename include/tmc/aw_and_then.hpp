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
#include <tuple>
#include <type_traits>
#include <utility>

namespace tmc {
namespace detail {

template <typename Awaitable, typename Func, typename... RestFuncs>
tmc::task<void> and_then_bridge_chain_void(
  Awaitable&& awaitable, Func&& func, RestFuncs&&... rest_funcs
) {
  using awaitable_result = typename tmc::detail::get_awaitable_traits<
    std::remove_reference_t<Awaitable>>::result_type;

  if constexpr (std::is_void_v<awaitable_result>) {
    co_await std::move(awaitable);
    auto next = std::invoke(std::forward<Func>(func));
    
    if constexpr (sizeof...(RestFuncs) == 0) {
      co_await std::move(next);
    } else {
      co_await and_then_bridge_chain_void(
        std::move(next), std::forward<RestFuncs>(rest_funcs)...
      );
    }
  } else {
    auto result = co_await std::move(awaitable);
    auto next = std::invoke(std::forward<Func>(func), std::move(result));
    
    if constexpr (sizeof...(RestFuncs) == 0) {
      co_await std::move(next);
    } else {
      co_await and_then_bridge_chain_void(
        std::move(next), std::forward<RestFuncs>(rest_funcs)...
      );
    }
  }
}

template <typename Result, typename Awaitable, typename Func, typename... RestFuncs>
tmc::task<Result> and_then_bridge_chain(
  Awaitable&& awaitable, Func&& func, RestFuncs&&... rest_funcs
) {
  using awaitable_result = typename tmc::detail::get_awaitable_traits<
    std::remove_reference_t<Awaitable>>::result_type;

  if constexpr (std::is_void_v<awaitable_result>) {
    co_await std::move(awaitable);
    auto next = std::invoke(std::forward<Func>(func));
    
    if constexpr (sizeof...(RestFuncs) == 0) {
      co_return co_await std::move(next);
    } else {
      co_return co_await and_then_bridge_chain<
        Result, decltype(next), RestFuncs...>(
        std::move(next), std::forward<RestFuncs>(rest_funcs)...
      );
    }
  } else {
    auto result = co_await std::move(awaitable);
    auto next = std::invoke(std::forward<Func>(func), std::move(result));
    
    if constexpr (sizeof...(RestFuncs) == 0) {
      co_return co_await std::move(next);
    } else {
      co_return co_await and_then_bridge_chain<
        Result, decltype(next), RestFuncs...>(
        std::move(next), std::forward<RestFuncs>(rest_funcs)...
      );
    }
  }
}

template <typename Awaitable, size_t... Is, typename... Funcs>
auto and_then_bridge_unpack_void(
  Awaitable&& awaitable, std::tuple<Funcs...>&& funcs, std::index_sequence<Is...>
) {
  return and_then_bridge_chain_void(
    std::forward<Awaitable>(awaitable), std::move(std::get<Is>(funcs))...
  );
}

template <
  typename Result, typename Awaitable, size_t... Is, typename... Funcs>
auto and_then_bridge_unpack(
  Awaitable&& awaitable, std::tuple<Funcs...>&& funcs, std::index_sequence<Is...>
) {
  return and_then_bridge_chain<Result, Awaitable, Funcs...>(
    std::forward<Awaitable>(awaitable), std::move(std::get<Is>(funcs))...
  );
}

template <typename Result, typename Awaitable, typename... Funcs>
tmc::task<Result>
and_then_bridge(Awaitable&& awaitable, std::tuple<Funcs...>&& funcs) {
  if constexpr (std::is_void_v<Result>) {
    co_await and_then_bridge_unpack_void(
      std::forward<Awaitable>(awaitable), std::move(funcs),
      std::index_sequence_for<Funcs...>{}
    );
  } else {
    co_return co_await and_then_bridge_unpack<Result, Awaitable>(
      std::forward<Awaitable>(awaitable), std::move(funcs),
      std::index_sequence_for<Funcs...>{}
    );
  }
}

template <typename Result, typename Awaitable, typename... Funcs>
class aw_and_then_impl {
  tmc::task<Result> bridge_task;

  struct empty {};
  using result_storage = std::conditional_t<
    std::is_void_v<Result>, empty, tmc::detail::result_storage_t<Result>>;
  TMC_NO_UNIQUE_ADDRESS result_storage result;

public:
  aw_and_then_impl(
    Awaitable&& Aw, std::tuple<Funcs...>&& Fns, tmc::ex_any* RunExec,
    tmc::ex_any* ResumeExec
  ) noexcept
      : bridge_task(and_then_bridge<Result, Awaitable, Funcs...>(
          static_cast<Awaitable&&>(Aw), std::move(Fns)
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

template <typename Awaitable, typename Func>
struct compute_single_then_result_impl {
  using awaitable_result =
    typename tmc::detail::get_awaitable_traits<Awaitable>::result_type;
  using invoke_result = decltype(std::declval<Func>()(
    std::declval<awaitable_result>()
  ));
  using type =
    typename tmc::detail::get_awaitable_traits<invoke_result>::result_type;
};

template <typename Awaitable, typename Func>
  requires(std::is_void_v<
           typename tmc::detail::get_awaitable_traits<Awaitable>::result_type>)
struct compute_single_then_result_impl<Awaitable, Func> {
  using invoke_result = decltype(std::declval<Func>()());
  using type =
    typename tmc::detail::get_awaitable_traits<invoke_result>::result_type;
};

template <typename Awaitable, typename... Funcs>
struct compute_chain_result;

template <typename Awaitable, typename Func>
struct compute_chain_result<Awaitable, Func> {
  using type =
    typename compute_single_then_result_impl<Awaitable, Func>::type;
};

template <typename Awaitable, typename Func0, typename... Funcs>
struct compute_chain_result<Awaitable, Func0, Funcs...> {
  using first_result =
    typename compute_single_then_result_impl<Awaitable, Func0>::type;

  using first_result_awaitable = decltype(std::declval<Func0>()(
    std::declval<
      typename tmc::detail::get_awaitable_traits<Awaitable>::result_type>()
  ));

  using type = typename compute_chain_result<
    first_result_awaitable, Funcs...>::type;
};

template <typename Awaitable, typename Func0, typename... Funcs>
  requires(std::is_void_v<
           typename tmc::detail::get_awaitable_traits<Awaitable>::result_type>)
struct compute_chain_result<Awaitable, Func0, Funcs...> {
  using first_result_awaitable = decltype(std::declval<Func0>()());

  using type = typename compute_chain_result<
    first_result_awaitable, Funcs...>::type;
};

} // namespace detail

template <typename Awaitable, typename... Funcs>
class [[nodiscard(
  "You must co_await aw_and_then."
)]] aw_and_then
    : public tmc::detail::run_on_mixin<aw_and_then<Awaitable, Funcs...>>,
      public tmc::detail::resume_on_mixin<aw_and_then<Awaitable, Funcs...>>,
      public tmc::detail::with_priority_mixin<aw_and_then<Awaitable, Funcs...>> {
  friend class tmc::detail::run_on_mixin<aw_and_then<Awaitable, Funcs...>>;
  friend class tmc::detail::resume_on_mixin<aw_and_then<Awaitable, Funcs...>>;
  friend class tmc::detail::with_priority_mixin<aw_and_then<Awaitable, Funcs...>>;

  using Result =
    typename tmc::detail::compute_chain_result<Awaitable, Funcs...>::type;

  Awaitable awaitable;
  std::tuple<Funcs...> funcs;
  tmc::ex_any* executor;
  tmc::ex_any* continuation_executor;
  size_t prio;

public:
  aw_and_then(Awaitable&& Aw, std::tuple<Funcs...>&& Fns) noexcept
      : awaitable(static_cast<Awaitable&&>(Aw)), funcs(std::move(Fns)),
        executor(tmc::detail::this_thread::executor),
        continuation_executor(tmc::detail::this_thread::executor),
        prio(tmc::detail::this_thread::this_task.prio) {}

  tmc::detail::aw_and_then_impl<Result, Awaitable, Funcs...>
  operator co_await() && noexcept {
    return tmc::detail::aw_and_then_impl<Result, Awaitable, Funcs...>(
      static_cast<Awaitable&&>(awaitable), std::move(funcs), executor,
      continuation_executor
    );
  }

  template <typename Func>
  auto and_then(Func&& F) && noexcept {
    return aw_and_then<Awaitable, Funcs..., std::decay_t<Func>>(
      static_cast<Awaitable&&>(awaitable),
      std::tuple_cat(std::move(funcs), std::make_tuple(std::forward<Func>(F)))
    );
  }

  aw_and_then(const aw_and_then&) = delete;
  aw_and_then& operator=(const aw_and_then&) = delete;
  aw_and_then(aw_and_then&& Other) noexcept
      : awaitable(static_cast<Awaitable&&>(Other.awaitable)),
        funcs(std::move(Other.funcs)), executor(Other.executor),
        continuation_executor(Other.continuation_executor), prio(Other.prio) {}
  aw_and_then& operator=(aw_and_then&& Other) noexcept {
    awaitable = static_cast<Awaitable&&>(Other.awaitable);
    funcs = std::move(Other.funcs);
    executor = Other.executor;
    continuation_executor = Other.continuation_executor;
    prio = Other.prio;
    return *this;
  }
};

template <typename Awaitable, typename Func>
auto and_then(Awaitable&& Aw, Func&& F) noexcept {
  return aw_and_then<
    tmc::detail::forward_awaitable<Awaitable>, std::decay_t<Func>>(
    static_cast<Awaitable&&>(Aw), std::make_tuple(std::forward<Func>(F))
  );
}

namespace detail {

template <typename Awaitable, typename... Funcs>
struct awaitable_traits<aw_and_then<Awaitable, Funcs...>> {
  static constexpr configure_mode mode = WRAPPER;

  using result_type =
    typename compute_chain_result<Awaitable, Funcs...>::type;
  using self_type = aw_and_then<Awaitable, Funcs...>;
  using awaiter_type =
    aw_and_then_impl<result_type, Awaitable, Funcs...>;

  static awaiter_type get_awaiter(self_type&& awaitable) noexcept {
    return static_cast<self_type&&>(awaitable).operator co_await();
  }
};

} // namespace detail
} // namespace tmc
