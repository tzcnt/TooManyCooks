// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/concepts.hpp"
#include "tmc/detail/thread_locals.hpp"

namespace tmc {
namespace detail {
// These mixins provide the `run_on`, `resume_on`, and `with_priority` methods
// for the fluent pattern that preserve the value category of the object.

template <typename Derived> class run_on_mixin {
public:
  /// The wrapped task will run on the provided executor.
  [[nodiscard]] inline Derived& run_on(tmc::ex_any* Executor) & {
    static_cast<Derived*>(this)->executor = Executor;
    return static_cast<Derived&>(*this);
  }
  /// The wrapped task will run on the provided executor.
  template <typename Exec> [[nodiscard]] Derived& run_on(Exec& Executor) & {
    static_cast<Derived*>(this)->executor =
      tmc::detail::executor_traits<Exec>::type_erased(Executor);
    return static_cast<Derived&>(*this);
  }
  /// The wrapped task will run on the provided executor.
  template <typename Exec> [[nodiscard]] Derived& run_on(Exec* Executor) & {
    static_cast<Derived*>(this)->executor =
      tmc::detail::executor_traits<Exec>::type_erased(*Executor);
    return static_cast<Derived&>(*this);
  }

  /// The wrapped task will run on the provided executor.
  [[nodiscard]] inline Derived&& run_on(tmc::ex_any* Executor) && {
    static_cast<Derived*>(this)->executor = Executor;
    return static_cast<Derived&&>(*this);
  }
  /// The wrapped task will run on the provided executor.
  template <typename Exec> [[nodiscard]] Derived&& run_on(Exec& Executor) && {
    static_cast<Derived*>(this)->executor =
      tmc::detail::executor_traits<Exec>::type_erased(Executor);
    return static_cast<Derived&&>(*this);
  }
  /// The wrapped task will run on the provided executor.
  template <typename Exec> [[nodiscard]] Derived&& run_on(Exec* Executor) && {
    static_cast<Derived*>(this)->executor =
      tmc::detail::executor_traits<Exec>::type_erased(*Executor);
    return static_cast<Derived&&>(*this);
  }
};

template <typename Derived> class resume_on_mixin {
public:
  /// The wrapped task will run on the provided executor.
  [[nodiscard]] inline Derived& resume_on(tmc::ex_any* Executor) & {
    static_cast<Derived*>(this)->continuation_executor = Executor;
    return static_cast<Derived&>(*this);
  }
  /// The wrapped task will run on the provided executor.
  template <typename Exec> [[nodiscard]] Derived& resume_on(Exec& Executor) & {
    static_cast<Derived*>(this)->continuation_executor =
      tmc::detail::executor_traits<Exec>::type_erased(Executor);
    return static_cast<Derived&>(*this);
  }
  /// The wrapped task will run on the provided executor.
  template <typename Exec> [[nodiscard]] Derived& resume_on(Exec* Executor) & {
    static_cast<Derived*>(this)->continuation_executor =
      tmc::detail::executor_traits<Exec>::type_erased(*Executor);
    return static_cast<Derived&>(*this);
  }

  /// The wrapped task will run on the provided executor.
  [[nodiscard]] inline Derived&& resume_on(tmc::ex_any* Executor) && {
    static_cast<Derived*>(this)->continuation_executor = Executor;
    return static_cast<Derived&&>(*this);
  }
  /// The wrapped task will run on the provided executor.
  template <typename Exec>
  [[nodiscard]] Derived&& resume_on(Exec& Executor) && {
    static_cast<Derived*>(this)->continuation_executor =
      tmc::detail::executor_traits<Exec>::type_erased(Executor);
    return static_cast<Derived&&>(*this);
  }
  /// The wrapped task will run on the provided executor.
  template <typename Exec>
  [[nodiscard]] Derived&& resume_on(Exec* Executor) && {
    static_cast<Derived*>(this)->continuation_executor =
      tmc::detail::executor_traits<Exec>::type_erased(*Executor);
    return static_cast<Derived&&>(*this);
  }
};

template <typename Derived> class with_priority_mixin {
public:
  /// Sets the priority of the wrapped task. If co_awaited, the outer
  /// coroutine will also be resumed with this priority.
  [[nodiscard]] inline Derived& with_priority(size_t Priority) & {
    static_cast<Derived*>(this)->prio = Priority;
    return static_cast<Derived&>(*this);
  }

  /// Sets the priority of the wrapped task. If co_awaited, the outer
  /// coroutine will also be resumed with this priority.
  [[nodiscard]] inline Derived&& with_priority(size_t Priority) && {
    static_cast<Derived*>(this)->prio = Priority;
    return static_cast<Derived&&>(*this);
  }
};

template <typename Base>
class rvalue_only_awaitable : private Base, private AwaitTagNoGroupCoAwait {
  /// The purpose of this class is to enforce good code hygiene. You must
  /// move-from your awaitables.
  /// If you get a compile error about private inheritance, you need to
  /// `co_await std::move(your_object);`
  using Base::Base;

public:
  Base&& operator co_await() && { return static_cast<Base&&>(*this); }
};

} // namespace detail
} // namespace tmc
