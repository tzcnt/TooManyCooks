#pragma once
#include <type_traits>

namespace tmc {
/// A lightweight iterator adapter that can be used to convert any input
/// sequence into an output iterator.
/// Applies `func` to each value produced by the input iterator before returning
/// it. Passes through `operator++` to the input iterator.
template <typename It, typename Transformer> struct iter_adapter {
private:
  Transformer func;
  It it;

public:
  using value_type = std::invoke_result_t<Transformer, It>;
  iter_adapter(It it_in, Transformer func_in) : func(func_in), it{it_in} {}

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
} // namespace tmc
