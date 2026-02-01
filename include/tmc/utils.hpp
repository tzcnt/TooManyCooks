// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include <type_traits>

namespace tmc {
/// A lightweight iterator adapter that can be used to transform any input
/// sequence into an iterator. (a replacement for
/// std::ranges::views::transform that doesn't require including <ranges>)
///
/// The only requirement is that the input type implement prefix operator++.
/// This passes through `operator++` to the input iterator.
///
/// Dereferencing this will apply `func` to the original iterator. Note that
/// this does not dereference the original iterator. This means that you can use
/// this with something that isn't really an iterator (such as just an int):
/// `tmc::iter_adapter tasks(0, [](int i) -> tmc::task<int> {co_return i;});`
/// will produce a sequence of tmc::tasks.
///
/// Or, if you use a real iterator, then you need to dereference it yourself:
/// ```
/// std::vector<int> values(5, 5);
/// tmc::iter_adapter tasks(values.begin(), [](auto it) -> tmc::task<int> {
///   return [](int i) -> tmc::task<int> { co_return i; }(*it);
/// });
/// ```
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
