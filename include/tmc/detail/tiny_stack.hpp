// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/compat.hpp"
#include "tmc/work_item.hpp"

#include <cstddef>
#include <cstring>
#include <new>
#include <type_traits>

// A lightweight owner-only LIFO stack used for per-thread private queues.
// Only the owning worker thread reads/writes it - no synchronization needed.
// Assumes work_item is trivially copyable and trivially destructible, so no
// placement new or destructor calls are needed; growth is a single allocate +
// memcpy.

// Currently not a template since it's only used for tmc::work_item.

namespace tmc {
namespace detail {
class tiny_stack {
  work_item* data;
  size_t sz;
  size_t cap;

  static_assert(std::is_trivially_copyable_v<work_item>);
  static_assert(std::is_trivially_destructible_v<work_item>);

public:
  tiny_stack() noexcept : data(new_data(64)), sz(0), cap(64) {}

  ~tiny_stack() { delete_data(data); }

  tiny_stack(const tiny_stack&) = delete;
  tiny_stack& operator=(const tiny_stack&) = delete;
  tiny_stack(tiny_stack&&) = delete;
  tiny_stack& operator=(tiny_stack&&) = delete;

  TMC_FORCE_INLINE bool empty() const noexcept { return sz == 0; }
  TMC_FORCE_INLINE work_item& back() noexcept { return data[sz - 1]; }
  TMC_FORCE_INLINE void pop_back() noexcept { --sz; }

  TMC_FORCE_INLINE void push_back(work_item Item) {
    if (sz == cap) [[unlikely]] {
      grow(sz + 1);
    }
    data[sz++] = Item;
  }

  // Bulk append. For raw `work_item*` input, collapses to a single memcpy.
  // For other iterator types, iterates and copies.
  template <typename It>
  TMC_FORCE_INLINE void push_back_bulk(It&& Items, size_t Count) {
    if (sz + Count > cap) [[unlikely]] {
      grow(sz + Count);
    }
    using ItNoRef = std::decay_t<It>;
    if constexpr (std::is_pointer_v<ItNoRef> &&
                  std::is_same_v<
                    std::remove_cv_t<std::remove_pointer_t<ItNoRef>>,
                    work_item>) {
      std::memcpy(data + sz, Items, Count * sizeof(work_item));
    } else {
      auto items = Items;
      for (size_t i = 0; i < Count; ++i) {
        data[sz + i] = *items;
        ++items;
      }
    }
    sz += Count;
  }

private:
  // Out-of-line slow path. Doubles capacity until Needed fits.
  void grow(size_t Needed) {
    size_t newCap = cap * 2;
    while (newCap < Needed) {
      newCap *= 2;
    }
    auto* newData = new_data(newCap);
    if (sz != 0) {
      std::memcpy(newData, data, sz * sizeof(work_item));
    }
    delete_data(data);
    data = newData;
    cap = newCap;
  }

  TMC_FORCE_INLINE work_item* new_data(size_t Capacity) {
    if constexpr (alignof(work_item) > __STDCPP_DEFAULT_NEW_ALIGNMENT__) {
      return static_cast<work_item*>(::operator new(
        sizeof(work_item) * Capacity, std::align_val_t(alignof(work_item))
      ));
    } else {
      return static_cast<work_item*>(
        ::operator new(sizeof(work_item) * Capacity)
      );
    }
  }

  TMC_FORCE_INLINE void delete_data(work_item* Data) {
    if constexpr (alignof(work_item) > __STDCPP_DEFAULT_NEW_ALIGNMENT__) {
      ::operator delete(Data, std::align_val_t(alignof(work_item)));
    } else {
      ::operator delete(Data);
    }
  }
};

} // namespace detail
} // namespace tmc
