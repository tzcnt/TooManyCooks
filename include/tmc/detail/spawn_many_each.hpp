// // Copyright (c) 2023-2024 Logan McDougall
// //
// // Distributed under the Boost Software License, Version 1.0. (See
// accompanying
// // file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

// #pragma once

// #include "tmc/detail/concepts.hpp" // IWYU pragma: keep
// #include "tmc/detail/thread_locals.hpp"
// #include "tmc/task.hpp"

// #include <array>
// #include <atomic>
// #include <cassert>
// #include <coroutine>
// #include <type_traits>
// #include <vector>

// namespace tmc {

// template <typename Result, size_t Count>
// class [[nodiscard("You must repeatedly await the result of each() until the "
//                   "result is equal to end().")]] aw_task_many_each_impl {
//   // This class uses an atomic bitmask with only 63 slots for tasks.
//   // each() doesn't seem like a good fit for larger task groups anyway.
//   // If you really need more room, please open a GitHub issue explaining
//   why... static_assert(Count < 64);

//   std::coroutine_handle<> continuation;
//   tmc::detail::type_erased_executor* continuation_executor;
//   int64_t remaining_count;
//   std::atomic<uint64_t> sync_flags;

//   struct empty {};
//   using ResultArray = std::conditional_t<
//     std::is_void_v<Result>, empty,
//     std::conditional_t<
//       Count == 0, std::vector<tmc::detail::result_storage_t<Result>>,
//       std::array<tmc::detail::result_storage_t<Result>, Count>>>;
//   ResultArray result_arr;

//   template <typename, size_t, typename, typename> friend class aw_task_many;

//   // Prepares the work item but does not initiate it.
//   template <typename T>
//   TMC_FORCE_INLINE inline void prepare_work(T& Task, size_t I) {
//     tmc::detail::awaitable_traits<T>::set_continuation(Task, &continuation);
//     tmc::detail::awaitable_traits<T>::set_continuation_executor(
//       Task, &continuation_executor
//     );
//     tmc::detail::awaitable_traits<T>::set_done_count(Task, &sync_flags);
//     tmc::detail::awaitable_traits<T>::set_flags(
//       Task, tmc::detail::task_flags::EACH | I
//     );
//     if constexpr (!std::is_void_v<Result>) {
//       tmc::detail::awaitable_traits<T>::set_result_ptr(Task, &result_arr[I]);
//     }
//   }

//   template <typename TaskIter>
//   inline aw_task_many_each_impl(
//     TaskIter Iter, size_t TaskCount,
//     tmc::detail::type_erased_executor* Executor,
//     tmc::detail::type_erased_executor* ContinuationExecutor, size_t Prio
//   )
//       : continuation_executor{ContinuationExecutor}, sync_flags{0},
//         remaining_count{0} {

//     // Wrap unknown awaitables into work_items (tasks). Preserve the type of
//     // known awaitables.
//     using Awaitable = std::remove_cvref_t<std::iter_value_t<TaskIter>>;
//     using WorkItem = std::conditional_t<
//       tmc::detail::awaitable_traits<Awaitable>::mode ==
//         tmc::detail::ASYNC_INITIATE,
//       Awaitable, work_item>;
//     using WorkItemArray = std::conditional_t<
//       Count == 0, std::vector<WorkItem>, std::array<WorkItem, Count>>;

//     WorkItemArray taskArr;
//     if constexpr (Count == 0) {
//       if (TaskCount > 63) {
//         TaskCount = 63;
//       }
//       taskArr.resize(TaskCount);
//       if constexpr (!std::is_void_v<Result>) {
//         result_arr.resize(TaskCount);
//       }
//     }
//     const size_t size = taskArr.size();
//     remaining_count = size;
//     sync_flags.store(tmc::detail::task_flags::EACH,
//     std::memory_order_release);

//     if constexpr (tmc::detail::awaitable_traits<Awaitable>::mode ==
//                   tmc::detail::ASYNC_INITIATE) {
//       // ASYNC_INITIATE types may possibly not be stored in a vector or array
//       // (no default/copy constructor), so initiate them individually
//       for (size_t i = 0; i < size; ++i) {
//         auto t = std::move(*Iter);
//         prepare_work(t, i);
//         tmc::detail::awaitable_traits<Awaitable>::async_initiate(
//           std::move(t), Executor, Prio
//         );
//         ++Iter;
//       }
//     } else {
//       for (size_t i = 0; i < size; ++i) {
//         if constexpr (tmc::detail::awaitable_traits<Awaitable>::mode ==
//                         tmc::detail::TMC_TASK ||
//                       tmc::detail::awaitable_traits<Awaitable>::mode ==
//                         tmc::detail::COROUTINE) {
//           auto t = std::move(*Iter);
//           prepare_work(t, i);
//           taskArr[i] = std::move(t);
//         } else if constexpr (tmc::detail::awaitable_traits<Awaitable>::mode
//         ==
//                              tmc::detail::UNKNOWN) {
//           // Wrap any unknown awaitable into a task
//           auto t = tmc::detail::safe_wrap(std::move(*Iter));
//           prepare_work(t, i);
//           taskArr[i] = std::move(t);
//         }
//         ++Iter;
//       }

//       // Initiate the tasks
//       tmc::detail::post_bulk_checked(Executor, taskArr.data(), size, Prio);
//     }
//   }

//   template <typename TaskIter>
//   inline aw_task_many_each_impl(
//     TaskIter Begin, TaskIter End, size_t MaxCount,
//     tmc::detail::type_erased_executor* Executor,
//     tmc::detail::type_erased_executor* ContinuationExecutor, size_t Prio
//   )
//     requires(requires(TaskIter a, TaskIter b) {
//               ++a;
//               *a;
//               a != b;
//             })
//       : continuation_executor{ContinuationExecutor}, sync_flags{0},
//         remaining_count{0} {

//     // Wrap unknown awaitables into work_items (tasks). Preserve the type of
//     // known awaitables.
//     using Awaitable = std::remove_cvref_t<std::iter_value_t<TaskIter>>;
//     using WorkItem = std::conditional_t<
//       tmc::detail::awaitable_traits<Awaitable>::mode ==
//         tmc::detail::ASYNC_INITIATE,
//       Awaitable, work_item>;
//     using WorkItemArray = std::conditional_t<
//       Count == 0, std::vector<WorkItem>, std::array<WorkItem, Count>>;

//     // Collect and prepare the tasks
//     WorkItemArray taskArr;
//     if (MaxCount > 63) {
//       MaxCount = 63;
//     }
//     if constexpr (Count == 0 && requires(TaskIter a, TaskIter b) { a - b; })
//     {
//       // Caller didn't specify capacity to preallocate, but we can calculate
//       size_t size = static_cast<size_t>(End - Begin);
//       if (MaxCount < size) {
//         size = MaxCount;
//       }
//       taskArr.resize(size);
//       if constexpr (!std::is_void_v<Result>) {
//         result_arr.resize(size);
//       }
//     }

//     size_t taskCount = 0;
//     if constexpr (Count != 0 || requires(TaskIter a, TaskIter b) { a - b; })
//     {
//       // Iterator could produce less than Count tasks, so count them.
//       // Iterator could produce more than Count tasks - stop after taking
//       Count. const size_t size = taskArr.size(); while (Begin != End &&
//       taskCount < size) {
//         if constexpr (tmc::detail::awaitable_traits<
//                         std::iter_value_t<TaskIter>>::mode ==
//                       tmc::detail::UNKNOWN) {
//           // Wrap any unknown awaitable into a task
//           auto t = tmc::detail::safe_wrap(std::move(*Begin));
//           prepare_work(t, taskCount);
//           taskArr[taskCount] = std::move(t);
//         } else {
//           auto t = std::move(*Begin);
//           prepare_work(t, taskCount);
//           taskArr[taskCount] = std::move(t);
//         }
//         ++Begin;
//         ++taskCount;
//       }
//     } else {
//       // We have no idea how many tasks there will be.
//       while (Begin != End && taskCount < MaxCount) {
//         if constexpr (tmc::detail::awaitable_traits<
//                         std::iter_value_t<TaskIter>>::mode ==
//                       tmc::detail::UNKNOWN) {
//           // Wrap any unknown awaitable into a task
//           taskArr.emplace_back(tmc::detail::safe_wrap(std::move(*Begin)));
//         } else {
//           taskArr.emplace_back(std::move(*Begin));
//         }
//         ++Begin;
//         ++taskCount;
//       }
//       // We couldn't bind result_ptr before we determined how many tasks
//       there
//       // are, because reallocation would invalidate those pointers. Now bind
//       // them.
//       if constexpr (!std::is_void_v<Result>) {
//         result_arr.resize(taskCount);
//       }
//       for (size_t i = 0; i < taskCount; ++i) {
//         if constexpr (tmc::detail::awaitable_traits<
//                         std::iter_value_t<TaskIter>>::mode ==
//                       tmc::detail::ASYNC_INITIATE) {
//           prepare_work(taskArr[i], i);
//         } else {
//           // TODO this is wrong - even if the mode is COROUTINE, it's
//           // not necessarily compatible with unsafe_task
//           auto t = tmc::detail::unsafe_task<Result>::from_address(
//             TMC_WORK_ITEM_AS_STD_CORO(taskArr[i]).address()
//           );
//           prepare_work(t, i);
//         }
//       }
//     }

//     // Initiate the tasks
//     remaining_count = taskCount;
//     sync_flags.store(tmc::detail::task_flags::EACH,
//     std::memory_order_release);

//     if constexpr (tmc::detail::awaitable_traits<
//                     std::iter_value_t<TaskIter>>::mode ==
//                   tmc::detail::ASYNC_INITIATE) {
//       for (size_t i = 0; i < taskCount; ++i) {
//         tmc::detail::awaitable_traits<std::iter_value_t<TaskIter>>::
//           async_initiate(std::move(taskArr[i]), Executor, Prio);
//       }
//     } else {
//       tmc::detail::post_bulk_checked(Executor, taskArr.data(), taskCount,
//       Prio);
//     }
//   }

// public:
//   /// Suspends if there are no ready results.
//   inline bool await_ready() const noexcept {
//     if (remaining_count == 0) {
//       return true;
//     }
//     auto resumeState = sync_flags.load(std::memory_order_acquire);
//     // High bit is set, because we are running
//     assert((resumeState & tmc::detail::task_flags::EACH) != 0);
//     auto readyBits = resumeState & ~tmc::detail::task_flags::EACH;
//     return readyBits != 0;
//   }

//   /// Suspends if there are no ready results.
//   TMC_FORCE_INLINE inline bool await_suspend(std::coroutine_handle<> Outer
//   ) noexcept {
//     continuation = Outer;
//   // This logic is necessary because we submitted all child tasks before the
//   // parent suspended. Allowing parent to be resumed before it suspends
//   // would be UB. Therefore we need to block the resumption until here.
//   // WARNING: We can use fetch_sub here because we know this bit wasn't set.
//   // It generates xadd instruction which is slightly more efficient than
//   // fetch_or. But not safe to use if the bit might already be set.
//   TRY_SUSPEND:
//     auto resumeState = sync_flags.fetch_sub(
//       tmc::detail::task_flags::EACH, std::memory_order_acq_rel
//     );
//     assert((resumeState & tmc::detail::task_flags::EACH) != 0);
//     auto readyBits = resumeState & ~tmc::detail::task_flags::EACH;
//     if (readyBits == 0) {
//       return true; // we suspended and no tasks were ready
//     }
//     // A result became ready, so try to resume immediately.
//     auto resumeState2 = sync_flags.fetch_or(
//       tmc::detail::task_flags::EACH, std::memory_order_acq_rel
//     );
//     bool didResume = (resumeState2 & tmc::detail::task_flags::EACH) == 0;
//     if (!didResume) {
//       return true; // Another thread already resumed
//     }
//     auto readyBits2 = resumeState2 & ~tmc::detail::task_flags::EACH;
//     if (readyBits2 == 0) {
//       // We resumed but another thread already consumed all the results
//       goto TRY_SUSPEND;
//     }
//     if (continuation_executor != nullptr &&
//         !tmc::detail::this_thread::exec_is(continuation_executor)) {
//       // Need to resume on a different executor
//       tmc::detail::post_checked(
//         continuation_executor, std::move(Outer),
//         tmc::detail::this_thread::this_task.prio
//       );
//       return true;
//     }
//     return false; // OK to resume inline
//   }

//   /// Returns the index of the current ready result. The result indexes
//   /// correspond to the indexes of the originally submitted tasks. Results
//   may
//   /// become ready in any order, but when awaited repeatedly, each index from
//   /// `[0..task_count)` will be returned exactly once. When there are no
//   /// more results to be returned, the returned index will be equal to
//   `end()`. inline size_t await_resume() noexcept {
//     if (remaining_count == 0) {
//       return end();
//     }
//     uint64_t resumeState = sync_flags.load(std::memory_order_acquire);
//     assert((resumeState & tmc::detail::task_flags::EACH) != 0);
//     // High bit is set, because we are resuming
//     uint64_t slots = resumeState & ~tmc::detail::task_flags::EACH;
//     assert(slots != 0);
// #ifdef _MSC_VER
//     size_t slot = static_cast<size_t>(_tzcnt_u64(slots));
// #else
//     size_t slot = static_cast<size_t>(__builtin_ctzll(slots));
// #endif
//     --remaining_count;
//     sync_flags.fetch_sub(1ULL << slot, std::memory_order_release);
//     return slot;
//   }

//   /// Provides a sentinel value that can be compared against the value
//   returned
//   /// from co_await.
//   inline size_t end() noexcept { return Count + 1; }

//   // Gets the ready result at the given index.
//   inline std::add_lvalue_reference_t<tmc::detail::result_storage_t<Result>>
//   operator[](size_t idx) noexcept
//     requires(!std::is_void_v<Result>)
//   {
//     assert(idx < result_arr.size());
//     return result_arr[idx];
//   }

//   // Provided for convenience only - to expose the same API as the
//   // Result-returning awaitable version. Does nothing.
//   inline void operator[]([[maybe_unused]] size_t idx) noexcept
//     requires(std::is_void_v<Result>)
//   {}

// // This must be awaited repeatedly until all child tasks have completed
// before
// // destruction.
// #ifndef NDEBUG
//   ~aw_task_many_each_impl() { assert(remaining_count == 0); }
// #endif
// };

// // template <typename Result, size_t Count>
// // using aw_task_many_each = aw_task_many_each_impl<Result, Count>;

// // namespace detail {

// // template <typename Result, size_t Count>
// // struct awaitable_traits<aw_task_many_each<Result, Count>> {
// //   static constexpr awaitable_mode mode = UNKNOWN;

// //   using result_type = size_t;
// //   using self_type = aw_task_many_each<Result, Count>;
// //   using awaiter_type = self_type;

// //   static awaiter_type get_awaiter(self_type&& Awaitable) {
// //     return std::forward<self_type>(Awaitable);
// //   }
// // };
// // } // namespace detail

// } // namespace tmc
