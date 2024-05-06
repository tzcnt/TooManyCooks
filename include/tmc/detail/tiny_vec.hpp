#pragma once
// MIT License

// Copyright (c) 2024 Logan McDougall

// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include <cassert>
#include <cstddef>
#include <new>

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
// placement new. T need not be copy or move constructible.
// You must call resize(), then emplace_at() each element, before destructor or
// clear();
template <typename T, size_t Alignment = alignof(T)> class tiny_vec {
  struct alignas(Alignment) AlignedT {
    T value;
  };

  AlignedT* data_;
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
    // equivalent to std::construct_at, but doesn't require including <memory>
    ::new (static_cast<void*>(&data_[Index].value))
      T(static_cast<ConstructArgs&&>(Args)...);
    return data_[Index].value;
  }

  void clear() {
    if (data_ != nullptr) {
      for (size_t i = 0; i < count_; ++i) {
        data_[i].value.~T();
      }
      ::operator delete(data_, std::align_val_t(Alignment));
      data_ = nullptr;
    }
    count_ = 0;
  }

  void resize(size_t Count) {
    if (Count == 0) {
      clear();
    } else {
      data_ = static_cast<AlignedT*>(
        ::operator new(Count * sizeof(AlignedT), std::align_val_t(Alignment))
      );
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
