// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/compat.hpp"
#include "tmc/detail/concepts_awaitable.hpp"
#include "tmc/detail/tiny_vec.hpp"
#include "tmc/work_item.hpp"

#include <coroutine>
#include <cstddef>

namespace tmc {
/// A type-erased executor that may represent any kind of TMC executor.
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

namespace detail {
template <> struct executor_traits<tmc::ex_any> {
  static inline void post(
    tmc::ex_any& ex, tmc::work_item&& Item, size_t Priority, size_t ThreadHint
  ) {
    ex.post(static_cast<tmc::work_item&&>(Item), Priority, ThreadHint);
  }

  template <typename It>
  static inline void post_bulk(
    tmc::ex_any& ex, It&& Items, size_t Count, size_t Priority,
    size_t ThreadHint
  ) {
    // ex_any only accepts work_item* as the post_bulk type
    if constexpr (std::is_convertible_v<It&&, tmc::work_item*>) {
      ex.post_bulk(
        static_cast<tmc::work_item*>(static_cast<It&&>(Items)), Count, Priority,
        ThreadHint
      );
    } else {
      tmc::detail::tiny_vec<tmc::work_item> workItems;
      workItems.resize(Count);
      for (size_t i = 0; i < Count; ++i) {
        workItems.emplace_at(
          i, static_cast<tmc::work_item&&>(*(static_cast<It&&>(Items)))
        );
        ++(static_cast<It&&>(Items));
      }
      ex.post_bulk(&workItems[0], Count, Priority, ThreadHint);
    }
  }

  static inline tmc::ex_any* type_erased(tmc::ex_any& ex) { return &ex; }

  static inline std::coroutine_handle<>
  dispatch(tmc::ex_any& ex, std::coroutine_handle<> Outer, size_t Priority) {
    ex.post(static_cast<std::coroutine_handle<>&&>(Outer), Priority);
    return std::noop_coroutine();
  }
};

template <> struct executor_traits<tmc::ex_any*> {
  static inline void post(
    tmc::ex_any* ex, tmc::work_item&& Item, size_t Priority, size_t ThreadHint
  ) {
    ex->post(static_cast<tmc::work_item&&>(Item), Priority, ThreadHint);
  }

  template <typename It>
  static inline void post_bulk(
    tmc::ex_any* ex, It&& Items, size_t Count, size_t Priority,
    size_t ThreadHint
  ) {
    // ex_any only accepts work_item* as the post_bulk type
    if constexpr (std::is_convertible_v<It&&, tmc::work_item*>) {
      ex->post_bulk(
        static_cast<tmc::work_item*>(static_cast<It&&>(Items)), Count, Priority,
        ThreadHint
      );
    } else {
      tmc::detail::tiny_vec<tmc::work_item> workItems;
      workItems.resize(Count);
      for (size_t i = 0; i < Count; ++i) {
        workItems.emplace_at(
          i, static_cast<tmc::work_item&&>(*(static_cast<It&&>(Items)))
        );
        ++(static_cast<It&&>(Items));
      }
      ex->post_bulk(&workItems[0], Count, Priority, ThreadHint);
    }
  }

  static inline tmc::ex_any* type_erased(tmc::ex_any* ex) { return ex; }

  static inline std::coroutine_handle<>
  dispatch(tmc::ex_any* ex, std::coroutine_handle<> Outer, size_t Priority) {
    ex->post(static_cast<std::coroutine_handle<>&&>(Outer), Priority);
    return std::noop_coroutine();
  }
};
} // namespace detail

} // namespace tmc
