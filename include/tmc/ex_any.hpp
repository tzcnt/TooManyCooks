// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/compat.hpp"
#include "tmc/work_item.hpp"

#include <cstddef>

namespace tmc {
// A type-erased executor that may represent any kind of TMC executor.
class ex_any {
public:
  // Pointers to the real executor and its function implementations.
  void* executor;
  void (*s_post)(
    void* Erased, work_item&& Item, size_t Priority, size_t ThreadHint
  ) noexcept;
  void (*s_post_bulk)(
    void* Erased, work_item* Items, size_t Count, size_t Priority,
    size_t ThreadHint
  ) noexcept;

  // API functions that delegate to the real executor.

  /// Submits a single work item to the delegated executor.
  inline void post(
    work_item&& Item, size_t Priority = 0, size_t ThreadHint = NO_HINT
  ) noexcept {
    s_post(executor, static_cast<work_item&&>(Item), Priority, ThreadHint);
  }

  /// Submits `Count` work items from the array pointed to by `Items` to the
  /// delegated executor.
  inline void post_bulk(
    work_item* Items, size_t Count, size_t Priority = 0,
    size_t ThreadHint = NO_HINT
  ) noexcept {
    s_post_bulk(executor, Items, Count, Priority, ThreadHint);
  }

  /// A default constructor is offered so that other executors can initialize
  /// this with their own function pointers.
  ex_any() {}

  /// This constructor is used by TMC executors.
  template <typename T> ex_any(T* Executor) {
    executor = Executor;
    s_post = [](
               void* Erased, work_item&& Item, size_t Priority,
               size_t ThreadHint
             ) noexcept {
      static_cast<T*>(Erased)->post(std::move(Item), Priority, ThreadHint);
    };
    s_post_bulk = [](
                    void* Erased, work_item* Items, size_t Count,
                    size_t Priority, size_t ThreadHint
                  ) noexcept {
      static_cast<T*>(Erased)->post_bulk(Items, Count, Priority, ThreadHint);
    };
  }
};

} // namespace tmc