// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include <type_traits>

namespace tmc {
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
  using value_type = std::invoke_result_t<Transformer&, It&>;
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
