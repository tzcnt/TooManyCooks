#pragma once
#include <type_traits>

namespace tmc {
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
  // auto operator++(int) {
  //   auto tmp = *this;
  //   ++(*this);
  //   return tmp;
  // }
};
} // namespace tmc
