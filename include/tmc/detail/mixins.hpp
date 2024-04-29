#pragma once
#include "tmc/detail/concepts.hpp"
#include "tmc/detail/thread_locals.hpp"
namespace tmc {
namespace detail {

template <typename Derived> class run_on_mixin {
public:
  /// The wrapped task will run on the provided executor.
  inline Derived& run_on(detail::type_erased_executor* Executor) & {
    static_cast<Derived*>(this)->executor = Executor;
    return static_cast<Derived&>(*this);
  }
  /// The wrapped task will run on the provided executor.
  template <detail::TypeErasableExecutor Exec>
  Derived& run_on(Exec& Executor) & {
    static_cast<Derived*>(this)->executor = Executor.type_erased();
    return static_cast<Derived&>(*this);
  }
  /// The wrapped task will run on the provided executor.
  template <detail::TypeErasableExecutor Exec>
  Derived& run_on(Exec* Executor) & {
    static_cast<Derived*>(this)->executor = Executor->type_erased();
    return static_cast<Derived&>(*this);
  }

  /// The wrapped task will run on the provided executor.
  inline Derived&& run_on(detail::type_erased_executor* Executor) && {
    static_cast<Derived*>(this)->executor = Executor;
    return static_cast<Derived&&>(*this);
  }
  /// The wrapped task will run on the provided executor.
  template <detail::TypeErasableExecutor Exec>
  Derived&& run_on(Exec& Executor) && {
    static_cast<Derived*>(this)->executor = Executor.type_erased();
    return static_cast<Derived&&>(*this);
  }
  /// The wrapped task will run on the provided executor.
  template <detail::TypeErasableExecutor Exec>
  Derived&& run_on(Exec* Executor) && {
    static_cast<Derived*>(this)->executor = Executor->type_erased();
    return static_cast<Derived&&>(*this);
  }
};

template <typename Derived> class resume_on_mixin {
public:
  /// The wrapped task will run on the provided executor.
  inline Derived& resume_on(detail::type_erased_executor* Executor) & {
    static_cast<Derived*>(this)->continuation_executor = Executor;
    return static_cast<Derived&>(*this);
  }
  /// The wrapped task will run on the provided executor.
  template <detail::TypeErasableExecutor Exec>
  Derived& resume_on(Exec& Executor) & {
    static_cast<Derived*>(this)->continuation_executor = Executor.type_erased();
    return static_cast<Derived&>(*this);
  }
  /// The wrapped task will run on the provided executor.
  template <detail::TypeErasableExecutor Exec>
  Derived& resume_on(Exec* Executor) & {
    static_cast<Derived*>(this)->continuation_executor =
      Executor->type_erased();
    return static_cast<Derived&>(*this);
  }

  /// The wrapped task will run on the provided executor.
  inline Derived&& resume_on(detail::type_erased_executor* Executor) && {
    static_cast<Derived*>(this)->continuation_executor = Executor;
    return static_cast<Derived&&>(*this);
  }
  /// The wrapped task will run on the provided executor.
  template <detail::TypeErasableExecutor Exec>
  Derived&& resume_on(Exec& Executor) && {
    static_cast<Derived*>(this)->continuation_executor = Executor.type_erased();
    return static_cast<Derived&&>(*this);
  }
  /// The wrapped task will run on the provided executor.
  template <detail::TypeErasableExecutor Exec>
  Derived&& resume_on(Exec* Executor) && {
    static_cast<Derived*>(this)->continuation_executor =
      Executor->type_erased();
    return static_cast<Derived&&>(*this);
  }
};

template <typename Derived> class with_priority_mixin {
public:
  /// Sets the priority of the wrapped task. If co_awaited, the outer
  /// coroutine will also be resumed with this priority.
  inline Derived& with_priority(size_t Priority) & {
    static_cast<Derived*>(this)->prio = Priority;
    return static_cast<Derived&>(*this);
  }

  /// Sets the priority of the wrapped task. If co_awaited, the outer
  /// coroutine will also be resumed with this priority.
  inline Derived&& with_priority(size_t Priority) && {
    static_cast<Derived*>(this)->prio = Priority;
    return static_cast<Derived&&>(*this);
  }
};

} // namespace detail
} // namespace tmc