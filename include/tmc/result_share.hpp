// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

// Provides tmc::result_share() which allows awaiting the same awaitable
// multiple times from different awaiters. Each awaiter receives an lvalue
// reference to a shared_ptr<Result> pointing to the original value.

#include "tmc/detail/concepts.hpp"

#include <memory>

namespace tmc {

template <
  typename Awaitable,
  typename Result = tmc::detail::awaitable_result_t<Awaitable>>
class aw_result_clone {
  Awaitable wrapped;
  std::shared_ptr<Result> result;
  // list of waiters
};
template <typename Awaitable> static inline void result_clone(Awaitable&& Aw) {}
} // namespace tmc