#pragma once
#include "tmc/detail/thread_locals.hpp"
#include "tmc/task.hpp"
#include <cassert>
#include <coroutine>
#include <vector>

namespace tmc {
// Forward declared friend classes.
// Defined in "tmc/spawn_task.hpp" / "tmc/spawn_task_many.hpp" which includes
// this header.

/// The customizable task wrapper / awaitable type returned by
/// `tmc::spawn(tmc::task<Result>)`.
template <typename Result> class aw_spawned_task;
/// The customizable task wrapper / awaitable type returned by
/// `tmc::spawn_many(tmc::task<Result>)`.
template <typename Result, size_t Count, typename Iter> class aw_task_many;

// /// A wrapper that converts lazy task(s) to eager task(s),
// /// and allows the task(s) to be awaited after it has been started.
// /// It is created by calling run_early() on a parent awaitable
// /// from spawn() or spawn_many().
// ///
// /// `Result` is the type of a single result value (same as that of the
// wrapped
// /// `task<Result>`).
// ///
// /// `Output` may be a `Result`, `std::vector<Result>`,
// /// or `std::array<Result, Count>` depending on what type of awaitable this
// /// was created from.
// template <typename Result, typename Output>
// class [[nodiscard("You must co_await aw_run_early. "
//                   "It is not safe to destroy aw_run_early without first "
//                   "awaiting it.")]] aw_run_early;

// template <typename Result, typename Output, bool RValue>
// class aw_run_early_impl;

// template <typename Result, typename Output, bool RValue>
// class aw_run_early_impl {
//   aw_run_early<Result, Output>& me;
//   friend aw_run_early<Result, Output>;
//   aw_run_early_impl(aw_run_early<Result, Output>& Me);

// public:
//   /// Always suspends.
//   inline bool await_ready() const noexcept;

//   /// Suspends the outer coroutine, submits the wrapped task to the
//   /// executor, and waits for it to complete.
//   inline bool await_suspend(std::coroutine_handle<> Outer) noexcept;

//   /// Returns the value provided by the wrapped task.
//   inline Output& await_resume() noexcept
//     requires(!RValue);

//   /// Returns the value provided by the wrapped task.
//   inline Output&& await_resume() noexcept
//     requires(RValue);
// };

// template <> class aw_run_early_impl<void, void, false> {
//   aw_run_early<void, void>& me;
//   friend aw_run_early<void, void>;

//   inline aw_run_early_impl(aw_run_early<void, void>& Me);

// public:
//   /// Always suspends.
//   inline bool await_ready() const noexcept;

//   /// Suspends the outer coroutine, submits the wrapped task to the
//   /// executor, and waits for it to complete.
//   inline bool await_suspend(std::coroutine_handle<> Outer) noexcept;

//   /// Does nothing.
//   inline void await_resume() noexcept;
// };

// template <typename Result, typename Output> class aw_run_early {
//   friend class aw_spawned_task<Result>;
//   template <typename R, size_t Count> friend class aw_task_many;
//   friend class aw_run_early_impl<Result, Output, true>;
//   friend class aw_run_early_impl<Result, Output, false>;
//   std::coroutine_handle<> continuation;
//   detail::type_erased_executor* continuation_executor;
//   Output result;
//   std::atomic<int64_t> done_count;

//   // Private constructor from aw_spawned_task. Takes ownership of parent's
//   // task.
//   aw_run_early(
//     task<Result>&& Task, size_t Priority,
//     detail::type_erased_executor* Executor,
//     detail::type_erased_executor* ContinuationExecutor
//   )
//       : continuation{nullptr}, continuation_executor(ContinuationExecutor),
//         done_count(1) {
//     auto& p = Task.promise();
//     p.continuation = &continuation;
//     p.continuation_executor = &continuation_executor;
//     p.result_ptr = &result;
//     p.done_count = &done_count;
//     // TODO fence maybe not required if there's one inside the queue?
//     std::atomic_thread_fence(std::memory_order_release);
//     Executor->post(std::move(Task), Priority);
//   }

//   // Private constructor from aw_task_many. Takes ownership of parent's
//   tasks.
//   // For use when count is runtime dynamic - take ownership of parent's
//   result
//   // vector.
//   template <size_t Count> aw_run_early(aw_task_many<Result, Count>&& Parent)
//   {
//     continuation_executor = Parent.continuation_executor;
//     if constexpr (std::is_same_v<Output, std::vector<Result>>) {
//       result = std::move(Parent.result);
//     }
//     const auto size = Parent.wrapped.size();
//     for (size_t i = 0; i < size; ++i) {
//       auto& p = detail::unsafe_task<Result>::from_address(
//                   TMC_WORK_ITEM_AS_STD_CORO(Parent.wrapped[i]).address()
//       )
//                   .promise();
//       p.continuation = &continuation;
//       p.continuation_executor = &continuation_executor;
//       p.done_count = &done_count;
//       p.result_ptr = &result[i];
//     }
//     done_count.store(static_cast<int64_t>(size), std::memory_order_release);
//     Parent.executor->post_bulk(Parent.wrapped.data(), Parent.prio, size);
//     Parent.did_await = true;
//   }

// public:
//   aw_run_early_impl<Result, Output, false> operator co_await() & {
//     return aw_run_early_impl<Result, Output, false>(*this);
//   }

//   aw_run_early_impl<Result, Output, true> operator co_await() && {
//     return aw_run_early_impl<Result, Output, true>(*this);
//   }

//   // This must be awaited and the child task completed before destruction.
//   ~aw_run_early() noexcept { assert(done_count.load() < 0); }

//   // Not movable or copyable due to child task being spawned in constructor,
//   // and having pointers to this.
//   aw_run_early& operator=(const aw_run_early& other) = delete;
//   aw_run_early(const aw_run_early& other) = delete;
//   aw_run_early& operator=(const aw_run_early&& other) = delete;
//   aw_run_early(const aw_run_early&& other) = delete;
// };

// template <> class aw_run_early<void, void> {
//   friend class aw_spawned_task<void>;
//   friend class aw_run_early_impl<void, void, false>;
//   template <typename R, size_t Count> friend class aw_task_many;
//   std::coroutine_handle<> continuation;
//   detail::type_erased_executor* continuation_executor;
//   std::atomic<int64_t> done_count;

//   // Private constructor from aw_spawned_task. Takes ownership of parent's
//   // task.
//   aw_run_early(
//     task<void>&& Task, size_t Priority, detail::type_erased_executor*
//     Executor, detail::type_erased_executor* ContinuationExecutor
//   )
//       : continuation{nullptr}, continuation_executor(ContinuationExecutor),
//         done_count(1) {
//     auto& p = Task.promise();
//     p.continuation = &continuation;
//     p.continuation_executor = &continuation_executor;
//     p.done_count = &done_count;
//     // TODO fence maybe not required if there's one inside the queue?
//     std::atomic_thread_fence(std::memory_order_release);
//     Executor->post(std::move(Task), Priority);
//   }

//   // Private constructor from aw_task_many. Takes ownership of parent's
//   tasks. template <size_t Count> aw_run_early(aw_task_many<void, Count>&&
//   Parent) {
//     continuation_executor = Parent.continuation_executor;
//     const auto size = Parent.wrapped.size();
//     for (size_t i = 0; i < size; ++i) {
//       auto& p = detail::unsafe_task<void>::from_address(
//                   TMC_WORK_ITEM_AS_STD_CORO(Parent.wrapped[i]).address()
//       )
//                   .promise();
//       p.continuation = &continuation;
//       p.continuation_executor = &continuation_executor;
//       p.done_count = &done_count;
//     }
//     done_count.store(static_cast<int64_t>(size), std::memory_order_release);
//     Parent.executor->post_bulk(Parent.wrapped.data(), Parent.prio, size);
//     Parent.did_await = true;
//   }

// public:
//   aw_run_early_impl<void, void, false> operator co_await() {
//     return aw_run_early_impl<void, void, false>(*this);
//   }

//   // This must be awaited and the child task completed before destruction.
//   ~aw_run_early() noexcept { assert(done_count.load() < 0); }

//   // Not movable or copyable due to child task being spawned in constructor,
//   // and having pointers to this.
//   aw_run_early& operator=(const aw_run_early& Other) = delete;
//   aw_run_early(const aw_run_early& Other) = delete;
//   aw_run_early& operator=(const aw_run_early&& Other) = delete;
//   aw_run_early(const aw_run_early&& Other) = delete;
// };

// template <typename Result, typename Output, bool RValue>
// aw_run_early_impl<Result, Output, RValue>::aw_run_early_impl(
//   aw_run_early<Result, Output>& Me
// )
//     : me(Me) {}

// /// Always suspends.
// template <typename Result, typename Output, bool RValue>
// inline bool
// aw_run_early_impl<Result, Output, RValue>::await_ready() const noexcept {
//   return false;
// }

// /// Suspends the outer coroutine, submits the wrapped task to the
// /// executor, and waits for it to complete.
// template <typename Result, typename Output, bool RValue>
// inline bool aw_run_early_impl<Result, Output, RValue>::await_suspend(
//   std::coroutine_handle<> Outer
// ) noexcept {
//   // For unknown reasons, it doesn't work to start with done_count at 0,
//   // Then increment here and check before storing continuation...
//   me.continuation = Outer;
//   auto remaining = me.done_count.fetch_sub(1, std::memory_order_acq_rel);
//   // Worker was already posted.
//   // Suspend if remaining > 0 (worker is still running)
//   if (remaining > 0) {
//     return true;
//   }
//   // Resume if remaining <= 0 (worker already finished)
//   if (me.continuation_executor == nullptr ||
//       me.continuation_executor == detail::this_thread::executor) {
//     return false;
//   } else {
//     // Need to resume on a different executor
//     me.continuation_executor->post(
//       std::move(Outer), detail::this_thread::this_task.prio
//     );
//     return true;
//   }
// }

// /// Returns the value provided by the wrapped function.
// template <typename Result, typename Output, bool RValue>
// inline Output&
// aw_run_early_impl<Result, Output, RValue>::await_resume() noexcept
//   requires(!RValue)
// {
//   return me.result;
// }

// /// Returns the value provided by the wrapped function.
// template <typename Result, typename Output, bool RValue>
// inline Output&&
// aw_run_early_impl<Result, Output, RValue>::await_resume() noexcept
//   requires(RValue)
// {
//   return std::move(me.result);
// }

// inline aw_run_early_impl<void, void, false>::aw_run_early_impl(
//   aw_run_early<void, void>& Me
// )
//     : me(Me) {}

// inline bool aw_run_early_impl<void, void, false>::await_ready() const
// noexcept {
//   return false;
// }

// /// Suspends the outer coroutine, submits the wrapped task to the
// /// executor, and waits for it to complete.
// inline bool aw_run_early_impl<void, void, false>::await_suspend(
//   std::coroutine_handle<> Outer
// ) noexcept {
//   // For unknown reasons, it doesn't work to start with done_count at 0,
//   // Then increment here and check before storing continuation...
//   me.continuation = Outer;
//   auto remaining = me.done_count.fetch_sub(1, std::memory_order_acq_rel);
//   // Worker was already posted.
//   // Suspend if remaining > 0 (worker is still running)
//   if (remaining > 0) {
//     return true;
//   }
//   // Resume if remaining <= 0 (worker already finished)
//   if (me.continuation_executor == nullptr ||
//       me.continuation_executor == detail::this_thread::executor) {
//     return false;
//   } else {
//     // Need to resume on a different executor
//     me.continuation_executor->post(
//       std::move(Outer), detail::this_thread::this_task.prio
//     );
//     return true;
//   }
// }

// /// Does nothing.
// inline void aw_run_early_impl<void, void, false>::await_resume() noexcept {}
} // namespace tmc
