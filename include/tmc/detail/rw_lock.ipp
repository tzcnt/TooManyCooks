// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/impl.hpp" // IWYU pragma: keep

#include "tmc/detail/waiter_list.hpp"
#include "tmc/rw_lock.hpp"

#include <atomic>
#include <cassert>
#include <coroutine>

namespace tmc {
rw_lock_read_scope::~rw_lock_read_scope() {
  if (parent != nullptr) {
    parent->unlock_read();
  }
}

rw_lock_write_scope::~rw_lock_write_scope() {
  if (parent != nullptr) {
    parent->unlock_write();
  }
}

bool rw_lock::try_lock_read() noexcept {
  state_t v = value.load(std::memory_order_relaxed);
  do {
    // Readers cannot enter while a writer holds the lock, or while writers
    // are waiting (so that a steady stream of readers cannot starve writers).
    if ((v & WRITER) != 0 || unpack_write_waiters(v) != 0) {
      return false;
    }
  } while (!value.compare_exchange_weak(
    v, v + READERS_ONE, std::memory_order_acquire, std::memory_order_relaxed
  ));
  return true;
}

bool rw_lock::try_lock_write() noexcept {
  state_t v = value.load(std::memory_order_relaxed);
  do {
    // Writers can only enter when the lock is idle and uncontended.
    if (v != 0) {
      return false;
    }
  } while (!value.compare_exchange_weak(
    v, WRITER, std::memory_order_acquire, std::memory_order_relaxed
  ));
  return true;
}

void rw_lock::suspend_read(
  tmc::detail::waiter_list_node& Node, std::coroutine_handle<> Outer
) noexcept {
  read_waiters.configure_and_add_waiter(Node, Outer);

  // Release the operation by increasing the waiter count.
  state_t old = value.fetch_add(READ_WAITERS_ONE, std::memory_order_acq_rel);

  // Using the fetched value, double-check if we can wake ourselves.
  try_wake<false>(old + READ_WAITERS_ONE);
}

void rw_lock::suspend_write(
  tmc::detail::waiter_list_node& Node, std::coroutine_handle<> Outer
) noexcept {
  write_waiters.configure_and_add_waiter(Node, Outer);

  // Release the operation by increasing the waiter count.
  state_t old = value.fetch_add(WRITE_WAITERS_ONE, std::memory_order_acq_rel);

  // Using the fetched value, double-check if we can wake ourselves.
  try_wake<true>(old + WRITE_WAITERS_ONE);
}

template <bool PreferWriters> void rw_lock::try_wake(state_t V) noexcept {
  // Try to claim the WAKING bit. Only one thread may hold it at a time; it
  // guards the consumer side of both waiter lists.
  while (true) {
    if (!can_wake(V)) {
      return;
    }
    if (value.compare_exchange_weak(
          V, V | WAKING, std::memory_order_acq_rel, std::memory_order_relaxed
        )) {
      V |= WAKING;
      break;
    }
  }

  // While the WAKING bit is held:
  // - No writer can hold the lock. The writer fast path requires the entire
  //   value to be 0, and the handoff to a waiting writer releases the WAKING
  //   bit in the same operation that sets the WRITER bit.
  // - Other threads may register new waiters and acquire/release the read
  //   lock concurrently, but only this thread may decrease the waiting reader
  //   / waiting writer counts (or take nodes from the waiter lists).
  //
  // The WAKING bit must be released via CAS. On CAS failure, re-check for
  // new writers or readers in order to prevent lost wakeups.
  while (true) {
    assert((V & WRITER) == 0);
    state_t readers = unpack_readers(V);
    state_t readWaiterCount = unpack_read_waiters(V);
    state_t writeWaiterCount = unpack_write_waiters(V);

    // Decide who to wake. When both readers and writers are waiting,
    // PreferWriters breaks the tie; it is set by the caller to alternate
    // between reader and writer phases (writers wake readers / readers wake
    // writers).
    bool wakeWriter, wakeReaders;
    if constexpr (PreferWriters) {
      wakeWriter = readers == 0 && writeWaiterCount > 0;
      wakeReaders = !wakeWriter && writeWaiterCount == 0 && readWaiterCount > 0;
    } else {
      wakeReaders = readWaiterCount > 0;
      wakeWriter = !wakeReaders && readers == 0 && writeWaiterCount > 0;
    }

    if (!wakeWriter && !wakeReaders) {
      // Nothing left to wake.
      if (value.compare_exchange_strong(
            V, V & ~WAKING, std::memory_order_acq_rel, std::memory_order_acquire
          )) {
        return;
      }
      // CAS failure means another thread submitted a new writer or reader.
      continue;
    }

    if (wakeWriter) {
      // Extract a single writer from the waiter list and transfer the lock to it.
      auto toWake = write_waiters.must_take_1();
      state_t newV;
      do {
        assert(unpack_readers(V) == 0);
        assert(unpack_write_waiters(V) > 0);
        // Atomically transfer 1 write waiter -> active writer, and release WAKING.
        newV = V - WAKING - WRITE_WAITERS_ONE + WRITER;
      } while (!value.compare_exchange_weak(
        V, newV, std::memory_order_acq_rel, std::memory_order_relaxed
      ));
      toWake->waiter.resume();
      return;
    } else {
      // Wake the waiting readers. We can't take more than the observed count,
      // because a waiter may add itself to the list before incrementing the
      // count; over-taking would corrupt the state.
      tmc::detail::waiter_list_node wakeHead;
      tmc::detail::waiter_list_node* wakeTail = &wakeHead;
      state_t taken = 0;

      while (true) {
        // `readWaiterCount` starts with the initial value from the top of the function,
        // but may increase after a subsequent CAS failure in this loop.
        for (; taken < readWaiterCount; ++taken) {
          auto node = read_waiters.must_take_1();
          wakeTail->next = node;
          wakeTail = node;
        }

        // Atomically transfer `taken` read waiters -> active readers, and release WAKING.
        state_t newV = V - WAKING - taken * READ_WAITERS_ONE + taken * READERS_ONE;
        if (value.compare_exchange_weak(
              V, newV, std::memory_order_acq_rel, std::memory_order_relaxed
            )) {
          break;
        }

        // CAS failed - accept new readers into the batch if there is no writer waiting.
        if (unpack_write_waiters(V) == 0) {
          readWaiterCount = unpack_read_waiters(V);
          assert(readWaiterCount >= taken);
        }
      }
      wakeTail->next = nullptr;

      auto toWake = wakeHead.next;
      while (toWake != nullptr) {
        auto next = toWake->next;
        toWake->waiter.resume();
        toWake = next;
      }
      return;
    }
  }
}

void rw_lock::unlock_read() noexcept {
  state_t old = value.fetch_sub(READERS_ONE, std::memory_order_acq_rel);
  assert(unpack_readers(old) > 0);
  try_wake<true>(old - READERS_ONE);
}

void rw_lock::unlock_write() noexcept {
  assert(is_write_locked());
  state_t old = value.fetch_sub(WRITER, std::memory_order_acq_rel);
  assert((old & WRITER) != 0);
  assert(unpack_readers(old) == 0);
  try_wake<false>(old - WRITER);
}

rw_lock::~rw_lock() {
  read_waiters.wake_all();
  write_waiters.wake_all();
}

// parent is atomic to prevent use-after-resume. See waiter_list.ipp's
// aw_acquire::await_suspend for more info.

bool aw_rw_lock_read_base::await_ready() noexcept {
  return parent.load(std::memory_order_relaxed)->try_lock_read();
}

void aw_rw_lock_read_base::await_suspend(std::coroutine_handle<> Outer) noexcept {
  parent.load(std::memory_order_acquire)->suspend_read(me, Outer);
}

bool aw_rw_lock_write_base::await_ready() noexcept {
  return parent.load(std::memory_order_relaxed)->try_lock_write();
}

void aw_rw_lock_write_base::await_suspend(std::coroutine_handle<> Outer) noexcept {
  parent.load(std::memory_order_acquire)->suspend_write(me, Outer);
}
} // namespace tmc
