// Copyright (c) 2023-2024 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/storage.hpp"

#include <cassert>
#include <cstddef>

// This class exists to solve 2 problems:
// 1. std::vector is variably 24 or 32 bytes depending on stdlib, which makes it
// difficulty to properly pack cachelines in ex_cpu for efficient sharing
// 2. std::vector requires its types be default / copy / move constructible
namespace tmc {
namespace detail {
// A vector-like class that allocates its data on the heap, respecting custom
// alignment. Does not support automatic resizing by append, or separate
// capacity and length.
// Allocates elements without constructing them, to be constructed later using
// placement new. T need not be default, copy, or move constructible. You must
// call resize(), then emplace_at() each element, before destructor or clear();
template <typename T, size_t Alignment = alignof(T)> class tiny_vec {
  tmc::storage<T, Alignment>* data_;
  size_t count_;

public:
  T& operator[](size_t Index) {
    assert(Index < count_);
    return data_[Index].value;
  }

  T* ptr(size_t Index) {
    assert(Index < count_);
    return &data_[Index].value;
  }

  template <typename... ConstructArgs>
  T& emplace_at(size_t Index, ConstructArgs&&... Args) {
    ::new (static_cast<void*>(&data_[Index].value))
      T(static_cast<ConstructArgs&&>(Args)...);
    return data_[Index].value;
  }

  void clear() {
    if (data_ != nullptr) {
      delete[] data_;
      data_ = nullptr;
    }
    count_ = 0;
  }

  void resize(size_t Count) {
    if (Count == 0) {
      clear();
    } else {
      data_ = new tmc::storage<T, Alignment>[Count];
      count_ = Count;
    }
  }

  size_t size() { return count_; }

  tiny_vec() : data_{nullptr}, count_{0} {}

  ~tiny_vec() { clear(); }
};

static_assert(sizeof(tiny_vec<int, 8>) == 16);
} // namespace detail
} // namespace tmc
