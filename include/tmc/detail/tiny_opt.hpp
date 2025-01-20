// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include <cstddef>
#include <type_traits>

namespace tmc {
namespace detail {
// Allocates elements without constructing them, to be constructed later using
// placement new. T need not be default, copy, or move constructible.
// Similar to std::optional, but should always have a value by the time it is
// user-accessible, and therefore has no overhead.
template <typename T, size_t Alignment = alignof(T)>
union alignas(Alignment) tiny_opt {
  T value;

  operator T&() & { return value; }
  operator const T&() const& { return value; }
  operator T&&() && { return static_cast<T&&>(value); }

  tiny_opt() {}

  tiny_opt(const tiny_opt&) = delete;
  tiny_opt& operator=(const tiny_opt&) = delete;
  tiny_opt(tiny_opt&&) = delete;
  tiny_opt& operator=(tiny_opt&&) = delete;

  ~tiny_opt()
    requires(std::is_trivially_destructible_v<T>)
  = default;
  ~tiny_opt()
    requires(!std::is_trivially_destructible_v<T>)
  {
    value.~T();
  }
};
} // namespace detail
} // namespace tmc
