// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

// Common template helpers shared by the tuple-shaped awaitable groups
// (tmc::spawn_tuple() and tmc::mux_tuple).

#include "tmc/detail/concepts_awaitable.hpp" // IWYU pragma: keep

#include <type_traits>
#include <variant>

namespace tmc {
namespace detail {
// Replace void with std::monostate (void is not a valid tuple element type)
template <typename T>
using void_to_monostate = std::conditional_t<std::is_void_v<T>, std::monostate, T>;

// Get the last type of a parameter pack
// In C++26 you can use pack indexing instead: T...[sizeof...(T) - 1]
template <typename... T> struct last_type {
  using type = typename decltype((std::type_identity<T>{}, ...))::type;
};

template <> struct last_type<> {
  // workaround for empty tuples - the task object will be void / empty
  using type = void;
};

template <typename... T> using last_type_t = typename last_type<T...>::type;

// Create 2 instantiations of the Variadic, one where all of the types
// satisfy the predicate, and one where none of the types satisfy the predicate.
template <template <class> class Predicate, template <class...> class Variadic, class...>
struct predicate_partition;

// Definition for empty parameter pack
template <template <class> class Predicate, template <class...> class Variadic>
struct predicate_partition<Predicate, Variadic> {
  using true_types = Variadic<>;
  using false_types = Variadic<>;
};

template <
  template <class> class Predicate, template <class...> class Variadic, class T,
  class... Ts>
struct predicate_partition<Predicate, Variadic, T, Ts...> {
  template <class, class> struct Cons;
  template <class Head, class... Tail> struct Cons<Head, Variadic<Tail...>> {
    using type = Variadic<Head, Tail...>;
  };

  // Every type in the parameter pack satisfies the predicate
  using true_types = typename std::conditional<
    Predicate<T>::value,
    typename Cons<
      T, typename predicate_partition<Predicate, Variadic, Ts...>::true_types>::type,
    typename predicate_partition<Predicate, Variadic, Ts...>::true_types>::type;

  // No type in the parameter pack satisfies the predicate
  using false_types = typename std::conditional<
    !Predicate<T>::value,
    typename Cons<
      T, typename predicate_partition<Predicate, Variadic, Ts...>::false_types>::type,
    typename predicate_partition<Predicate, Variadic, Ts...>::false_types>::type;
};

// Partition the awaitables into two groups:
// 1. The awaitables that are coroutines, which will be submitted in bulk /
// symmetric transfer.
// 2. The awaitables that are not coroutines, which will be initiated by
// async_initiate.
template <typename T> struct treat_as_coroutine {
  static constexpr bool value =
    tmc::detail::get_awaitable_traits<T>::mode == tmc::detail::TMC_TASK ||
    tmc::detail::get_awaitable_traits<T>::mode == tmc::detail::COROUTINE ||
    tmc::detail::get_awaitable_traits<T>::mode == tmc::detail::WRAPPER;
};
} // namespace detail
} // namespace tmc
