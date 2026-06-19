// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once
#include "tmc/detail/impl.hpp" // IWYU pragma: keep

#include "tmc/detail/concepts_awaitable.hpp"
#include "tmc/detail/waiter_list.hpp"

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace tmc::tests {
class waiter_count_accessor;
}

namespace tmc {
class rw_lock;

/// The read lock will be released when this goes out of scope.
class [[nodiscard("The read lock will be released when this goes out of scope.")]]
rw_lock_read_scope {
  rw_lock* parent;

  friend class aw_rw_lock_read_scope;

  inline rw_lock_read_scope(rw_lock* Parent) noexcept : parent(Parent) {}

public:
  // Movable but not copyable
  rw_lock_read_scope(rw_lock_read_scope const&) = delete;
  rw_lock_read_scope& operator=(rw_lock_read_scope const&) = delete;
  inline rw_lock_read_scope(rw_lock_read_scope&& Other) noexcept {
    parent = Other.parent;
    Other.parent = nullptr;
  }
  rw_lock_read_scope& operator=(rw_lock_read_scope&& Other) = delete;

  /// Releases the read lock on destruction. Does not symmetric transfer.
  TMC_DECL ~rw_lock_read_scope();
};

/// The write lock will be released when this goes out of scope.
class [[nodiscard("The write lock will be released when this goes out of scope.")]]
rw_lock_write_scope {
  rw_lock* parent;

  friend class aw_rw_lock_write_scope;

  inline rw_lock_write_scope(rw_lock* Parent) noexcept : parent(Parent) {}

public:
  // Movable but not copyable
  rw_lock_write_scope(rw_lock_write_scope const&) = delete;
  rw_lock_write_scope& operator=(rw_lock_write_scope const&) = delete;
  inline rw_lock_write_scope(rw_lock_write_scope&& Other) noexcept {
    parent = Other.parent;
    Other.parent = nullptr;
  }
  rw_lock_write_scope& operator=(rw_lock_write_scope&& Other) = delete;

  /// Releases the write lock on destruction. Does not symmetric transfer.
  TMC_DECL ~rw_lock_write_scope();
};

// Common state shared by all four rw_lock awaiters. Same structure as
// aw_acquire but `value` has different semantics.
class aw_rw_lock_base : tmc::detail::AwaitTagNoGroupAsIs {
protected:
  tmc::detail::waiter_list_node me;
  // parent is atomic to prevent use-after-resume. See waiter_list.ipp's
  // aw_acquire::await_suspend for more info.
  std::atomic<rw_lock*> parent;

  inline aw_rw_lock_base(rw_lock& Parent) noexcept : parent(&Parent) {}

public:
  // Cannot be moved or copied due to holding intrusive list pointer
  aw_rw_lock_base(aw_rw_lock_base const&) = delete;
  aw_rw_lock_base& operator=(aw_rw_lock_base const&) = delete;
  aw_rw_lock_base(aw_rw_lock_base&&) = delete;
  aw_rw_lock_base& operator=(aw_rw_lock_base&&) = delete;
};

// Shared await_ready()/await_suspend() for read awaiters.
class aw_rw_lock_read_base : public aw_rw_lock_base {
protected:
  inline aw_rw_lock_read_base(rw_lock& Parent) noexcept : aw_rw_lock_base(Parent) {}

public:
  TMC_DECL bool await_ready() noexcept;

  TMC_DECL void await_suspend(std::coroutine_handle<> Outer) noexcept;
};

// Shared await_ready()/await_suspend() for write awaiters.
class aw_rw_lock_write_base : public aw_rw_lock_base {
protected:
  inline aw_rw_lock_write_base(rw_lock& Parent) noexcept : aw_rw_lock_base(Parent) {}

public:
  TMC_DECL bool await_ready() noexcept;

  TMC_DECL void await_suspend(std::coroutine_handle<> Outer) noexcept;
};

class [[nodiscard("You must co_await aw_rw_lock_read for it to have any effect.")]]
aw_rw_lock_read : public aw_rw_lock_read_base {
  friend class rw_lock;

  inline aw_rw_lock_read(rw_lock& Parent) noexcept : aw_rw_lock_read_base(Parent) {}

public:
  inline void await_resume() noexcept {}
};

class [[nodiscard("You must co_await aw_rw_lock_write for it to have any effect.")]]
aw_rw_lock_write : public aw_rw_lock_write_base {
  friend class rw_lock;

  inline aw_rw_lock_write(rw_lock& Parent) noexcept : aw_rw_lock_write_base(Parent) {}

public:
  inline void await_resume() noexcept {}
};

/// Same as aw_rw_lock_read but returns a nodiscard rw_lock_read_scope that
/// releases the read lock on destruction.
class [[nodiscard("You must co_await aw_rw_lock_read_scope for it to have any effect.")]]
aw_rw_lock_read_scope : public aw_rw_lock_read_base {
  friend class rw_lock;

  inline aw_rw_lock_read_scope(rw_lock& Parent) noexcept : aw_rw_lock_read_base(Parent) {}

public:
  [[nodiscard]] inline rw_lock_read_scope await_resume() noexcept {
    return rw_lock_read_scope(parent.load(std::memory_order_relaxed));
  }
};

/// Same as aw_rw_lock_write but returns a nodiscard rw_lock_write_scope that
/// releases the write lock on destruction.
class [[nodiscard("You must co_await aw_rw_lock_write_scope for it to have any effect.")]]
aw_rw_lock_write_scope : public aw_rw_lock_write_base {
  friend class rw_lock;

  inline aw_rw_lock_write_scope(rw_lock& Parent) noexcept
      : aw_rw_lock_write_base(Parent) {}

public:
  [[nodiscard]] inline rw_lock_write_scope await_resume() noexcept {
    return rw_lock_write_scope(parent.load(std::memory_order_relaxed));
  }
};

/// An async reader-writer lock (a.k.a. std::shared_mutex). Any number of
/// readers may hold the lock simultaneously, or one writer may hold it
/// exclusively. Tasks that cannot acquire the lock immediately will suspend,
/// and be resumed when the lock is transferred to them. All operations are
/// implemented with lock-free atomics; no operation ever blocks or spins.
///
/// Phase-fair policy (neither readers nor writers can starve):
/// - New readers can acquire the lock immediately only if no writer holds it
///   and no writers are waiting. If a writer is waiting, new readers queue
///   behind it.
/// - When the last reader releases the lock, the longest-waiting writer is
///   woken and the lock is transferred to it.
/// - When a writer releases the lock, all currently-waiting readers are woken
///   as a batch. If no readers are waiting, the next writer is woken instead.
///
/// The reader count, waiting reader count, and waiting writer count are each
/// packed into (STATE_BITS - 2) / 3 bits of a single atomic word. When 64-bit atomics are
/// available, a 64-bit word is used, so none of these may exceed 1048575. Otherwise,
/// falls back to a size_t word; on a 32-bit platform that reduces each field to 10 bits
/// (max 1023).
class rw_lock {
  friend class aw_rw_lock_read;
  friend class aw_rw_lock_write;
  friend class aw_rw_lock_read_scope;
  friend class aw_rw_lock_write_scope;
  friend class aw_rw_lock_read_base;
  friend class aw_rw_lock_write_base;
  friend class ::tmc::tests::waiter_count_accessor;

  // Prefer a 64-bit state when possible. This includes on 32-bit x86 (which has
  // cmpxchg8b). Otherwise, fallback to a 32-bit state, which limits each count
  // to 10 bits.
  using state_t =
    std::conditional_t<std::atomic<uint64_t>::is_always_lock_free, uint64_t, size_t>;
  static inline constexpr state_t STATE_ONE = static_cast<state_t>(1);
  static inline constexpr size_t STATE_BITS = 8 * sizeof(state_t);

  // Bit 0: WAKING - a lock bit for waiter consumption.
  // Bit 1: WRITER - set if a writer holds the lock.
  // The remaining bits are divided into 3 equal fields:
  // active readers, waiting readers, and waiting writers.
  //
  // Logically there are 2x2 counts: (reader, writer) x (waiting, active),
  // with the active writer count limited to a max of 1 / single bit.
  static inline constexpr state_t WRITER = STATE_ONE << 1;
  static inline constexpr state_t WAKING = STATE_ONE;
  static inline constexpr size_t FIELD_BITS = (STATE_BITS - 2) / 3;
  static inline constexpr state_t FIELD_MASK = (STATE_ONE << FIELD_BITS) - STATE_ONE;
  static inline constexpr size_t READERS_OFFSET = 2;
  static inline constexpr size_t READ_WAITERS_OFFSET = READERS_OFFSET + FIELD_BITS;
  static inline constexpr size_t WRITE_WAITERS_OFFSET = READ_WAITERS_OFFSET + FIELD_BITS;
  static inline constexpr state_t READERS_ONE = STATE_ONE << READERS_OFFSET;
  static inline constexpr state_t READ_WAITERS_ONE = STATE_ONE << READ_WAITERS_OFFSET;
  static inline constexpr state_t WRITE_WAITERS_ONE = STATE_ONE << WRITE_WAITERS_OFFSET;

  static inline state_t unpack_readers(state_t V) noexcept {
    return (V >> READERS_OFFSET) & FIELD_MASK;
  }
  static inline state_t unpack_read_waiters(state_t V) noexcept {
    return (V >> READ_WAITERS_OFFSET) & FIELD_MASK;
  }
  static inline state_t unpack_write_waiters(state_t V) noexcept {
    return (V >> WRITE_WAITERS_OFFSET) & FIELD_MASK;
  }

  // Returns true if a wake handoff is possible and no other thread is already performing
  // one. A writer can be woken if the lock is fully released. Waiting readers can be
  // woken if no writer holds or is waiting for the lock. If readers are active, and both
  // writers and readers are waiting, we intentionally do not wake additional readers;
  // instead drain all active readers so the writer can be woken.
  static inline bool can_wake(state_t V) noexcept {
    if ((V & (WRITER | WAKING)) != 0) {
      return false;
    }
    return (unpack_readers(V) == 0 && unpack_write_waiters(V) > 0) ||
           (unpack_write_waiters(V) == 0 && unpack_read_waiters(V) > 0);
  }

  tmc::detail::waiter_list read_waiters;
  tmc::detail::waiter_list write_waiters;
  std::atomic<state_t> value{0};

  // Registers Outer as a waiting reader and wakes waiters if a release
  // happened concurrently.
  TMC_DECL void suspend_read(
    tmc::detail::waiter_list_node& Node, std::coroutine_handle<> Outer
  ) noexcept;

  // Registers Outer as a waiting writer and wakes waiters if a release
  // happened concurrently.
  TMC_DECL void suspend_write(
    tmc::detail::waiter_list_node& Node, std::coroutine_handle<> Outer
  ) noexcept;

  // If V represents a wakeable state, tries to claim the WAKING bit and
  // transfer the lock to one or more waiters. PreferWriters controls which
  // kind of waiter is woken when both readers amd writers are waiting.
  template <bool PreferWriters> void try_wake(state_t V) noexcept;

  // Returns the number of awaiters currently registered (suspended and
  // waiting to acquire, in either mode) on this lock. For testing purposes.
  // Thread-safe.
  inline size_t waiter_count() noexcept {
    state_t v = value.load(std::memory_order_acquire);
    return static_cast<size_t>(unpack_read_waiters(v) + unpack_write_waiters(v));
  }

public:
  /// The lock begins in the fully unlocked state.
  rw_lock() noexcept = default;

  /// Returns true if some task is holding the write lock.
  /// This value is not guaranteed to be consistent with any other operation.
  inline bool is_write_locked() noexcept {
    return 0 != (WRITER & value.load(std::memory_order_relaxed));
  }

  /// Returns the number of tasks currently holding the read lock.
  /// This value is not guaranteed to be consistent with any other operation.
  inline size_t reader_count() noexcept {
    return static_cast<size_t>(unpack_readers(value.load(std::memory_order_relaxed)));
  }

  /// Tries to acquire the read lock without suspending. Returns true on
  /// success. Returns false if a writer holds the lock or any writers are
  /// waiting. Not re-entrant.
  TMC_DECL bool try_lock_read() noexcept;

  /// Tries to acquire the write lock without suspending. Returns true on
  /// success. Returns false if the lock is held in any mode. Not re-entrant.
  TMC_DECL bool try_lock_write() noexcept;

  /// Tries to acquire the read lock. If a writer is holding or waiting for
  /// the lock, will suspend until the read lock can be acquired by this task.
  /// Multiple tasks may hold the read lock simultaneously. Not re-entrant.
  inline aw_rw_lock_read lock_read() noexcept { return aw_rw_lock_read(*this); }

  /// Tries to acquire the write lock. If the lock is held by any other task,
  /// will suspend until the write lock can be acquired exclusively by this
  /// task. Not re-entrant.
  inline aw_rw_lock_write lock_write() noexcept { return aw_rw_lock_write(*this); }

  /// Same as lock_read(), but returns an object that will release the read
  /// lock (and resume awaiters) when it goes out of scope. Not re-entrant.
  inline aw_rw_lock_read_scope lock_read_scope() noexcept {
    return aw_rw_lock_read_scope(*this);
  }

  /// Same as lock_write(), but returns an object that will release the write
  /// lock (and resume awaiters) when it goes out of scope. Not re-entrant.
  inline aw_rw_lock_write_scope lock_write_scope() noexcept {
    return aw_rw_lock_write_scope(*this);
  }

  /// Releases the read lock. If this was the last reader and writers are
  /// waiting, a writer will be resumed and the write lock will be transferred
  /// to it. Does not symmetric transfer; the awaiter will be posted to its
  /// executor.
  TMC_DECL void unlock_read() noexcept;

  /// Releases the write lock. If readers are waiting, all of them will be
  /// resumed and the read lock will be transferred to them. Otherwise, if
  /// writers are waiting, one writer will be resumed and the write lock will
  /// be transferred to it. Does not symmetric transfer; awaiters will be
  /// posted to their executors.
  TMC_DECL void unlock_write() noexcept;

  /// On destruction, any awaiters will be resumed.
  TMC_DECL ~rw_lock();
};
} // namespace tmc

#if !defined(TMC_STANDALONE_COMPILATION) || defined(TMC_IMPL)
#include "tmc/detail/rw_lock.ipp"
#endif
