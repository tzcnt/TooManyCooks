// Copyright (c) 2023-2024 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once
#include <type_traits>

namespace tmc {
namespace detail {
template <typename E>
concept TypeErasableExecutor = requires(E e) { e.type_erased(); };

struct AwaitTagNoGroupAsIs {};

template <typename T>
concept HasAwaitTagNoGroupAsIs = std::is_base_of_v<AwaitTagNoGroupAsIs, T>;

struct AwaitTagNoGroupCoAwait {};

template <typename T>
concept HasAwaitTagNoGroupCoAwait =
  std::is_base_of_v<AwaitTagNoGroupCoAwait, T>;
} // namespace detail
} // namespace tmc
