// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/impl.hpp" // IWYU pragma: keep

#include "tmc/detail/thread_locals.hpp"
#include "tmc/detail/waiter_list.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <coroutine>
#include <cstddef>

namespace tmc {
namespace detail {

void waiter_list_node::suspend(
  tmc::detail::waiter_data_base* Parent, std::coroutine_handle<> Outer
) noexcept {
  auto& parentList = Parent->waiters;
  auto& parentValue = Parent->value;

  // Configure this awaiter and add it to the waiter list
  parentList.configure_and_add_waiter(*this, Outer);

  // Release the operation by increasing the waiter count
  size_t add = TMC_ONE_BIT << tmc::detail::WAITERS_OFFSET;
  size_t old = parentValue.fetch_add(add, std::memory_order_acq_rel);
  size_t v = add + old;

  // Using the fetched value, see if there are both resources available and
  // waiters to wake.
  parentList.maybe_wake(parentValue, v, old, false);
}

void waiter_list_waiter::resume() noexcept {
  tmc::detail::post_checked(
    continuation_executor, std::move(continuation), continuation_priority
  );
}

std::coroutine_handle<>
waiter_list_waiter::try_symmetric_transfer(std::coroutine_handle<> Outer) noexcept {
  if (tmc::detail::this_thread::exec_prio_is(
        continuation_executor, continuation_priority
      )) {
    // Post Outer, symmetric transfer to this.
    // We can post Outer to the continuation executor since it is the current
    // executor, per the above check.
    tmc::detail::post_checked(
      continuation_executor, std::move(Outer), continuation_priority
    );
    return continuation;
  } else {
    // Post this, return Outer (symmetric transfer back to caller)
    resume();
    return Outer;
  }
}

namespace {
// Reverses a singly-linked chain of waiter_list_nodes in place.
// The caller must have exclusive access to the chain.
inline tmc::detail::waiter_list_node*
reverse_chain(tmc::detail::waiter_list_node* curr TMC_LIFETIMEBOUND) noexcept {
  tmc::detail::waiter_list_node* prev = nullptr;
  while (curr != nullptr) {
    auto next = curr->next;
    curr->next = prev;
    prev = curr;
    curr = next;
  }
  return prev;
}
} // namespace

std::coroutine_handle<> try_symmetric_transfer2_waiter(
  waiter_list_waiter* ToWake, std::coroutine_handle<> Continuation, tmc::ex_any* Executor,
  size_t Priority
) noexcept {
  if (ToWake != nullptr) {
    std::coroutine_handle<> toContinuation = ToWake->continuation;
    tmc::ex_any* toExecutor = ToWake->continuation_executor;
    size_t toPriority = ToWake->continuation_priority;
    // If we can transfer to primary, then do so, and post backup.
    if (tmc::detail::this_thread::exec_prio_is(toExecutor, toPriority)) {
      if (Continuation != nullptr) {
        tmc::detail::post_checked(Executor, std::move(Continuation), Priority);
      }
      return toContinuation;
    }

    // Transfer to primary disallowed
    tmc::detail::post_checked(toExecutor, std::move(toContinuation), toPriority);
  }

  if (Continuation != nullptr) {
    // Try to transfer to backup
    if (tmc::detail::this_thread::exec_prio_is(Executor, Priority)) {
      return Continuation;
    }

    // Transfer to backup disallowed
    tmc::detail::post_checked(Executor, std::move(Continuation), Priority);
  }
  return std::noop_coroutine();
}

void waiter_list::configure_and_add_waiter(
  tmc::detail::waiter_list_node& Node, std::coroutine_handle<> Outer
) noexcept {
  Node.waiter.continuation = Outer;
  Node.waiter.continuation_executor = tmc::detail::this_thread::executor();
  Node.waiter.continuation_priority = tmc::detail::this_thread::this_task().prio;

  auto h = input.load(std::memory_order_acquire);
  do {
    Node.next = h;
  } while (!input.compare_exchange_strong(
    h, &Node, std::memory_order_acq_rel, std::memory_order_acquire
  ));
}

size_t wake_waiters_in_batches(waiter_list_node* Curr) noexcept {
  std::array<tmc::work_item, 64> batch;
  size_t batchSize = 0;
  tmc::ex_any* continuationExecutor = nullptr;
  size_t continuationPriority = 0;
  size_t wakeCount = 0;

  while (Curr != nullptr) {
    auto next = Curr->next;

    // A batch can only be posted to a single executor at a single priority.
    // If this waiter's executor or priority differs from the batch in
    // progress, post that batch before starting a new one for this waiter.
    if (batchSize != 0 && (Curr->waiter.continuation_executor != continuationExecutor ||
                           Curr->waiter.continuation_priority != continuationPriority)) {
      tmc::detail::post_bulk_checked(
        continuationExecutor, batch.data(), batchSize, continuationPriority
      );
      batchSize = 0;
    }

    if (batchSize == 0) {
      continuationExecutor = Curr->waiter.continuation_executor;
      continuationPriority = Curr->waiter.continuation_priority;
    }

    batch[batchSize] = Curr->waiter.continuation;
    ++batchSize;
    ++wakeCount;
    if (batchSize == batch.size()) {
      tmc::detail::post_bulk_checked(
        continuationExecutor, batch.data(), batchSize, continuationPriority
      );
      batchSize = 0;
    }

    Curr = next;
  }

  if (batchSize != 0) {
    tmc::detail::post_bulk_checked(
      continuationExecutor, batch.data(), batchSize, continuationPriority
    );
  }

  return wakeCount;
}

void waiter_list::wake_all() noexcept { wake_waiters_in_batches(take_all()); }

waiter_list_node* waiter_list::take_all() noexcept {
  // Drain the input stack. take_all is only used when every waiter will be
  // woken, so the order doesn't matter; no need to reverse.
  auto in = input.exchange(nullptr, std::memory_order_acq_rel);

  // If output is empty, the input chain is the entire list.
  if (output == nullptr) {
    return in;
  }

  // Otherwise, splice the input chain onto the end of output.
  auto result = output;
  auto tail = output;
  while (tail->next != nullptr) {
    tail = tail->next;
  }
  tail->next = in;
  output = nullptr;
  return result;
}

size_t waiter_list::size() const noexcept {
  size_t count = 0;
  for (auto curr = output; curr != nullptr; curr = curr->next) {
    ++count;
  }
  for (auto curr = input.load(std::memory_order_acquire); curr != nullptr;
       curr = curr->next) {
    ++count;
  }
  return count;
}

waiter_list_waiter* waiter_list::maybe_wake(
  std::atomic<size_t>& value, size_t v, size_t old, bool symmetric
) noexcept {
  {
    tmc::detail::half_word oldCount;
    size_t oldWaiterCount;
    tmc::detail::unpack_value(old, oldCount, oldWaiterCount);
    // 4 possible prior states:
    // - count == 0 && waitercount == 0 -> nothing to wake
    // - count > 0 && waiterCount > 0 -> another thread is already executing a
    // wake operation
    // - count == 0 && waiterCount > 0 -> can wake if count++
    // - count > 0 && waitercount == 0 -> can wake if waiterCount++
    if ((oldCount == 0) == (oldWaiterCount == 0)) {
      return nullptr;
    }
  }
  {
    tmc::detail::half_word count;
    size_t waiterCount, wakeCount;
    size_t totalWakeCount = 0;
    tmc::detail::waiter_list_node wakeHead;
    wakeHead.next = nullptr;
    tmc::detail::waiter_list_node* wakeTail = &wakeHead;
    // Only one thread can enter the below section at a time.
    // Transitioning from 0/N or N/0 to N/N state acquires the critical section.
    // Transitioning back to 0/0, 0/N, or N/0 state releases the critical
    // section. The critical section is only needed to control access to the
    // `next` pointer of the shared waiters list, which occurs in must_take_1.
    while (true) {
      tmc::detail::unpack_value(v, count, waiterCount);
      wakeCount = count < waiterCount ? count : waiterCount;
      if (wakeCount == 0) {
        break;
      }
      totalWakeCount += wakeCount;

      // Take N waiters
      for (size_t i = 0; i < wakeCount; ++i) {
        auto toWake = must_take_1();
        wakeTail->next = toWake;
        wakeTail = toWake;
      }

      // (maybe) release the critical section
      size_t newV = tmc::detail::pack_value(
        count - static_cast<half_word>(wakeCount), waiterCount - wakeCount
      );
      while (!value.compare_exchange_strong(
        v, newV, std::memory_order_acq_rel, std::memory_order_acquire
      )) {
        tmc::detail::unpack_value(v, count, waiterCount);
        assert(count >= wakeCount);
        assert(waiterCount >= wakeCount);
        newV = tmc::detail::pack_value(
          count - static_cast<half_word>(wakeCount), waiterCount - wakeCount
        );
      }

      // Update the value of v and run again. If both values are still non-zero,
      // then the critical section was not actually released and we should
      // continue to take more waiters.
      v = newV;
    }

    if (totalWakeCount == 0) {
      return nullptr;
    }

    // wakeHead is a dummy object; its next pointer is the first real waiter
    auto toWake = wakeHead.next;

    tmc::detail::waiter_list_waiter* symmetric_task = nullptr;
    if (symmetric) {
      // Capture the first element of the list for symmetric transfer.
      // Caller will handle it.
      symmetric_task = &toWake->waiter;
      toWake = toWake->next;
      --totalWakeCount;
    }

    // Resume the rest of the waiters.
    for (size_t i = 0; i < totalWakeCount; ++i) {
      auto next = toWake->next;
      toWake->waiter.resume();
      toWake = next;
    }

    return symmetric_task;
  }
}

waiter_list_node* waiter_list::must_take_1() noexcept {
  if (output == nullptr) {
    // Output is exhausted; atomically drain the input stack and reverse it
    // into output so that the oldest pusher is at the front.
    auto chain = input.exchange(nullptr, std::memory_order_acq_rel);
    // should be guaranteed to see at least 1 waiter
    assert(chain != nullptr);
    output = reverse_chain(chain);
  }
  auto toWake = output;
  output = output->next;
  return toWake;
}

bool try_acquire(std::atomic<size_t>& Value) noexcept {
  auto v = Value.load(std::memory_order_relaxed);

  tmc::detail::half_word count;
  size_t waiterCount, newV;
  do {
    tmc::detail::unpack_value(v, count, waiterCount);
    if (count <= waiterCount) {
      return false;
    }
    newV = tmc::detail::pack_value(count - 1, waiterCount);
  } while (!Value.compare_exchange_strong(
    v, newV, std::memory_order_acq_rel, std::memory_order_acquire
  ));
  return true;
}

} // namespace detail

bool aw_acquire::await_ready() noexcept {
  return tmc::detail::try_acquire(parent.load(std::memory_order_relaxed)->value);
}

void aw_acquire::await_suspend(std::coroutine_handle<> Outer) noexcept {
  // This may be resumed immediately after we call add_waiter(). Access to
  // any member variable after that point is UB. However we need to use the
  // value of parent after calling add_waiter(). Thus we need to ensure that
  // we have loaded it into a local variable prior.

  // The simplest way to ensure this is to make parent atomic (to guarantee
  // the load actually happens now) and use acquire-acquire ordering to ensure
  // the load cannot be moved past the cmpxchg in add_waiter().

  // In practice this results in identical codegen on Clang.
  me.suspend(parent.load(std::memory_order_acquire), Outer);
}
} // namespace tmc
