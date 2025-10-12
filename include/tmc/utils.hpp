// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once
#include <memory_resource>
#include <type_traits>

namespace tmc {
class scoped_buffer {
public:
  using value_type = std::byte;
  using allocator_type = std::pmr::polymorphic_allocator<value_type>;

  // constexpr scoped_buffer() noexcept
  //     : buffer(new T[Size]), mbr(buffer, Size), pa(&mbr) {}
  inline scoped_buffer(size_t Count) noexcept
      : buffer(nullptr), elem_count(Count) {}

  inline void* try_alloc(size_t Size) {
    if (buffer == nullptr) {
      auto byteCount =
        Size * elem_count + 1000; // make space for all the elements
      buffer = new value_type[byteCount];
      new (&mbr) std::pmr::monotonic_buffer_resource(buffer, byteCount);
      new (&pa) allocator_type(&mbr);
    }
    return pa.allocate(Size); // return space for 1 element
  }

  inline allocator_type& allocator() noexcept { return pa; }

  inline ~scoped_buffer() noexcept {
    if (buffer == nullptr) {
      return;
    }
    delete[] buffer;
  }

  size_t elem_count;
  value_type* buffer;
  std::pmr::monotonic_buffer_resource mbr;
  allocator_type pa;
};

/// A lightweight iterator adapter that can be used to convert any input
/// sequence into an output iterator. (a replacement for
/// std::ranges::views::transform that doesn't require including <ranges>)
///
/// Applies `func` to each value produced by the input iterator before returning
/// it. Passes through `operator++` to the input iterator.
template <typename It, typename Transformer> class iter_adapter {
  Transformer func;
  It it;

public:
  using value_type = std::invoke_result_t<Transformer, It&>;
  template <typename Iter_, typename Transformer_>
  iter_adapter(Iter_&& Iterator, Transformer_&& TransformFunc)
      : func(static_cast<Transformer_&&>(TransformFunc)),
        it{static_cast<Iter_&&>(Iterator)} {}

  value_type operator*() { return func(it); }

  auto& operator++() {
    ++it;
    return *this;
  }

  /// Don't support postfix operator++ because it requires making a copy of
  /// this.
  // auto operator++(int) {
  //   auto tmp = *this;
  //   ++(*this);
  //   return tmp;
  // }
};

template <typename I, typename T>
iter_adapter(I&& i, T&& t) -> iter_adapter<std::decay_t<I>, std::decay_t<T>>;
} // namespace tmc
