// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/compat.hpp"
#include "tmc/detail/concepts_awaitable.hpp"
#include "tmc/detail/thread_locals.hpp"

#include <coroutine>
#include <cstddef>

namespace tmc {
namespace detail {
namespace task_flags {
// Flags bitfield section
static inline constexpr size_t NONE = 0;
static inline constexpr size_t EACH = TMC_ONE_BIT << (TMC_PLATFORM_BITS - 1);

// Numeric bitfield sections
static inline constexpr size_t TASKNUM_HIGH_OFF = 10;
static inline constexpr size_t TASKNUM_LOW_OFF = 4;
static inline constexpr size_t TASKNUM_MASK =
  (TMC_ONE_BIT << TASKNUM_HIGH_OFF) - (TMC_ONE_BIT << TASKNUM_LOW_OFF);

// This is the continuation priority, not the current task's priority
static inline constexpr size_t PRIORITY_HIGH_OFF = 4;
static inline constexpr size_t PRIORITY_LOW_OFF = 0;
static inline constexpr size_t PRIORITY_MASK =
  (TMC_ONE_BIT << PRIORITY_HIGH_OFF) - (TMC_ONE_BIT << PRIORITY_LOW_OFF);

} // namespace task_flags

/// Multipurpose awaitable type. Exposes fields that can be customized by most
/// TMC utility functions. Exposing this type allows various awaitables to be
/// compatible with the library and with each other.
///
/// `done_count` is used as an atomic barrier to synchronize with other tasks in
/// the same spawn group (in the case of spawn_many()), or the awaiting task (in
/// the case of fork()). In other scenarios, `done_count` is unused,and is
/// expected to be nullptr.
///
/// If `done_count` is nullptr, `continuation` and `continuation_executor` are
/// used directly.
///
/// If `done_count` is not nullptr, `continuation` and `continuation_executor`
/// are indirected. This allows them to be changed simultaneously for many tasks
/// in the same group.
///
/// `flags` is used to indicate other methods of coordinated resumption between
/// multiple awaitables.
struct awaitable_customizer_base {
  void* continuation;
  void* continuation_executor;
  void* done_count;
  size_t flags;

  static_assert(sizeof(void*) == sizeof(std::coroutine_handle<>));
  static_assert(alignof(void*) == alignof(std::coroutine_handle<>));
  static_assert(std::is_trivially_copyable_v<std::coroutine_handle<>>);
  static_assert(std::is_trivially_destructible_v<std::coroutine_handle<>>);
  static_assert(sizeof(void*) == sizeof(ptrdiff_t));
  static_assert(sizeof(ptrdiff_t) == sizeof(size_t));

  awaitable_customizer_base() noexcept
      : continuation{nullptr},
        continuation_executor{tmc::detail::this_thread::executor},
        done_count{nullptr}, flags{tmc::detail::this_thread::this_task.prio} {}

  // Either returns the awaiting coroutine (continuation) to be resumed
  // directly, or submits that awaiting coroutine to the continuation executor
  // to be resumed. This should be called exactly once, after the awaitable is
  // complete and any results are ready.
  TMC_FORCE_INLINE inline std::coroutine_handle<>
  resume_continuation() noexcept {
    std::coroutine_handle<> finalContinuation;
    tmc::ex_any* continuationExecutor = nullptr;
    if (done_count == nullptr) {
      // being awaited alone, or detached
      // continuation is a std::coroutine_handle<>
      // continuation_executor is a tmc::ex_any*
      continuationExecutor = static_cast<tmc::ex_any*>(continuation_executor);
      finalContinuation = std::coroutine_handle<>::from_address(continuation);
    } else {
      // being awaited as part of a group
      bool shouldResume;
      if (flags & task_flags::EACH) {
        // Each only supports 63 (or 31, on 32-bit) tasks. High bit of flags
        // indicates whether the awaiting task is ready to resume, or is already
        // resumed. Each of the low 63 or 31 bits are unique to a child task. We
        // will set our unique low bit, as well as try to set the high bit. If
        // high bit was already set, someone else is running the awaiting task
        // already.
        shouldResume = 0 == (task_flags::EACH &
                             static_cast<std::atomic<size_t>*>(done_count)
                               ->fetch_or(
                                 task_flags::EACH |
                                   (TMC_ONE_BIT
                                    << ((flags & task_flags::TASKNUM_MASK) >>
                                        task_flags::TASKNUM_LOW_OFF)),
                                 std::memory_order_acq_rel
                               ));
      } else {
        // task is part of a spawn_many group, or fork
        // continuation is a std::coroutine_handle<>*
        // continuation_executor is a tmc::ex_any**
        shouldResume = static_cast<std::atomic<ptrdiff_t>*>(done_count)
                         ->fetch_sub(1, std::memory_order_acq_rel) == 0;
      }
      if (shouldResume) {
        continuationExecutor =
          *static_cast<tmc::ex_any**>(continuation_executor);
        finalContinuation =
          *(static_cast<std::coroutine_handle<>*>(continuation));
      } else {
        finalContinuation = nullptr;
      }
    }

    // Common submission and continuation logic
    if (finalContinuation == nullptr) {
      finalContinuation = std::noop_coroutine();
    } else {
      size_t continuationPriority = flags & task_flags::PRIORITY_MASK;
      if (continuationExecutor != nullptr &&
          !tmc::detail::this_thread::exec_prio_is(
            continuationExecutor, continuationPriority
          )) {
        // post_checked is redundant with the prior check at the moment
        tmc::detail::post_checked(
          continuationExecutor, std::move(finalContinuation),
          continuationPriority
        );
        finalContinuation = std::noop_coroutine();
      }
    }

    // Single return to satisfy NRVO
    return finalContinuation;
  }
};

template <typename Result>
struct awaitable_customizer : awaitable_customizer_base {
  tmc::detail::result_storage_t<Result>* result_ptr;
  awaitable_customizer() noexcept
      : awaitable_customizer_base{}, result_ptr{nullptr} {}

  using result_type = Result;
};

template <> struct awaitable_customizer<void> : awaitable_customizer_base {
  awaitable_customizer() noexcept : awaitable_customizer_base{} {}

  using result_type = void;
};

// final_suspend type for tmc::task, tmc::detail::task_wrapper,
// tmc::detail::task_unsafe. a wrapper around awaitable_customizer
template <typename Promise> struct mt1_continuation_resumer {
  inline bool await_ready() const noexcept { return false; }

  // This is never called - tasks are destroyed at the final_suspend instead.
  [[maybe_unused]] inline void await_resume() const noexcept {}

  inline std::coroutine_handle<>
  await_suspend(std::coroutine_handle<Promise> Handle) const noexcept {
    auto& p = Handle.promise();
    auto continuation = p.customizer.resume_continuation();
    Handle.destroy();
    return continuation;
  }
};
} // namespace detail
} // namespace tmc
