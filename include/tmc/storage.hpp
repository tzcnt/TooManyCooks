// Copyright (c) 2023-2024 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include <cstddef>
#include <type_traits>

namespace tmc {
// Allocates elements without constructing them, to be constructed later using
// placement new. T need not be default, copy, or move constructible.
// Similar to std::optional, but should always have a value by the time it is
// user-accessible, and therefore has no overhead.
template <typename T, size_t Alignment = alignof(T)>
union alignas(Alignment) storage {
  T value;
  storage() {}
  ~storage()
    requires(std::is_trivially_destructible_v<T>)
  = default;
  ~storage()
    requires(!std::is_trivially_destructible_v<T>)
  {
    value.~T();
  }
};
} // namespace tmc
