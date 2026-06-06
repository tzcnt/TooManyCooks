// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

// Storage type used internally by several public-API queues:
// - qu_spsc_bounded
// - qu_spsc_unbounded
// - qu_mpsc_bounded
// - qu_mpsc_unbounded
// - channel

// Allocates elements without constructing them, to be constructed later using
// placement new. T need not be default, copy, or move constructible.
// The caller must track whether the element exists, and manually invoke the
// destructor if necessary.

#include <cassert>
#include <type_traits>

namespace tmc {
namespace detail {
template <typename T> struct qu_storage {
  union alignas(alignof(T)) {
    T value;
  };
#ifndef NDEBUG
  bool exists = false;
#endif

  qu_storage() noexcept {}

  template <typename... ConstructArgs>
  void emplace(ConstructArgs&&... Args) noexcept {
#ifndef NDEBUG
    assert(!exists);
    exists = true;
#endif
    ::new (static_cast<void*>(&value)) T(static_cast<ConstructArgs&&>(Args)...);
  }

  void destroy() noexcept {
#ifndef NDEBUG
    assert(exists);
    exists = false;
#endif
    value.~T();
  }

  // Precondition: Other.value must exist
  qu_storage(qu_storage&& Other) noexcept {
    emplace(static_cast<T&&>(Other.value));
    Other.destroy();
  }
  qu_storage& operator=(qu_storage&& Other) noexcept {
    emplace(static_cast<T&&>(Other.value));
    Other.destroy();
    return *this;
  }

  // If data was present, the caller is responsible for destroying it.
#ifndef NDEBUG
  ~qu_storage() { assert(!exists); }
#else
  ~qu_storage()
    requires(std::is_trivially_destructible_v<T>)
  = default;
  ~qu_storage()
    requires(!std::is_trivially_destructible_v<T>)
  {}
#endif

  qu_storage(const qu_storage&) = delete;
  qu_storage& operator=(const qu_storage&) = delete;
};
} // namespace detail
} // namespace tmc
