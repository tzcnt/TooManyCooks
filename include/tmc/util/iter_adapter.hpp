// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include <type_traits>

namespace tmc {
namespace util {

template <typename T>
concept Subtractable = requires(T a, T b) {
  { a - b };
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

  /// Invokes the wrapper function on the contained iterator.
  /// TODO - this type actually constructs an iterator from something that isn't
  /// an iterator (doesn't have an operator*); but if the underlying type does
  /// have operator* then we should deref it. Perhaps there should be 2 types -
  /// into_iterator and iter_adapter.
  value_type operator*() { return func(it); }

  auto& operator++() {
    ++it;
    return *this;
  }

  /// Calculates the difference between the underlying iterators.
  std::ptrdiff_t operator-(const iter_adapter& other) const
    requires Subtractable<It>
  {
    return static_cast<std::ptrdiff_t>(it - other.it);
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

} // namespace util
} // namespace tmc
