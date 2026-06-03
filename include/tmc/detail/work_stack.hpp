// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/compat.hpp"
#include "tmc/work_item.hpp"

#include <cstring>
#include <type_traits>

namespace tmc {

namespace detail {
// A lightweight owner-only LIFO stack used for the per-thread q_private.
// Only the owning worker thread reads/writes it - no synchronization needed.
// Assumes work_item is trivially copyable and trivially destructible, so no
// placement new or destructor calls are needed; growth is a single allocate +
// memcpy.
struct work_stack {
  work_item* data_;
  size_t size_;
  size_t cap_;

  work_stack() noexcept : data_(nullptr), size_(0), cap_(0) {
    static_assert(std::is_trivially_copyable_v<work_item>);
    static_assert(std::is_trivially_destructible_v<work_item>);
  }

  ~work_stack() {
    if (data_ != nullptr) {
      ::operator delete[](static_cast<void*>(data_));
    }
  }

  work_stack(const work_stack&) = delete;
  work_stack& operator=(const work_stack&) = delete;
  work_stack(work_stack&&) = delete;
  work_stack& operator=(work_stack&&) = delete;

  TMC_FORCE_INLINE bool empty() const noexcept { return size_ == 0; }
  TMC_FORCE_INLINE size_t size() const noexcept { return size_; }
  TMC_FORCE_INLINE work_item& back() noexcept { return data_[size_ - 1]; }
  TMC_FORCE_INLINE void pop_back() noexcept { --size_; }

  TMC_FORCE_INLINE void push_back(work_item Item) {
    if (size_ == cap_) [[unlikely]] {
      grow(size_ + 1);
    }
    data_[size_++] = Item;
  }

  // Bulk append. For raw `work_item*` input, collapses to a single memcpy.
  // For other iterator types, iterates and assigns.
  template <typename It>
  TMC_FORCE_INLINE void push_bulk(It&& Items, size_t Count) {
    if (size_ + Count > cap_) [[unlikely]] {
      grow(size_ + Count);
    }
    using ItNoRef = std::remove_reference_t<It>;
    if constexpr (std::is_pointer_v<ItNoRef> &&
                  std::is_same_v<
                    std::remove_cv_t<std::remove_pointer_t<ItNoRef>>,
                    work_item>) {
      std::memcpy(
        data_ + size_, static_cast<work_item*>(Items), Count * sizeof(work_item)
      );
    } else {
      for (size_t i = 0; i < Count; ++i) {
        data_[size_ + i] = static_cast<work_item&&>(*Items);
        ++Items;
      }
    }
    size_ += Count;
  }

private:
  // Out-of-line slow path. Doubles capacity (starting at 16) until Needed
  // fits.
  void grow(size_t Needed) {
    size_t newCap = cap_ == 0 ? 16 : cap_ * 2;
    while (newCap < Needed) {
      newCap *= 2;
    }
    auto* newData =
      static_cast<work_item*>(::operator new[](newCap * sizeof(work_item)));
    if (size_ != 0) {
      std::memcpy(newData, data_, size_ * sizeof(work_item));
    }
    if (data_ != nullptr) {
      ::operator delete[](static_cast<void*>(data_));
    }
    data_ = newData;
    cap_ = newCap;
  }
};

} // namespace detail
} // namespace tmc
