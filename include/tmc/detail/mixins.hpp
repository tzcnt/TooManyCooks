// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/concepts_awaitable.hpp"
#include "tmc/ex_any.hpp"

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
class rvalue_only_awaitable : public Base, private AwaitTagNoGroupCoAwait {
  using Base::Base;

  /// The purpose of this class is to enforce good code hygiene. This type must
  /// be awaited as an rvalue. If you get a compile error about private
  /// inheritance, you need to wrap your awaitable in std::move() in order to
  /// await it or pass it to spawn_*().
private:
  using Base::await_ready;
  using Base::await_resume;
  using Base::await_suspend;

public:
  Base&& operator co_await() && noexcept { return static_cast<Base&&>(*this); }
};

template <typename Base>
class lvalue_only_awaitable : public Base,
                              private AwaitTagNoGroupCoAwaitLvalue {
  using Base::Base;

  /// The purpose of this class is to enforce good code hygiene. This type must
  /// be awaited as an lvalue. A temporary cannot be used. If you get a compile
  /// error about private inheritance, you need to save this type to a
  /// standalone variable and pass that variable to co_await or spawn_*().
  /// Do not use std::move() with this type.
private:
  using Base::await_ready;
  using Base::await_resume;
  using Base::await_suspend;

public:
  Base& operator co_await() & noexcept { return static_cast<Base&>(*this); }
};

} // namespace detail
} // namespace tmc
