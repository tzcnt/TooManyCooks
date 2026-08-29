// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

// An async implementation of std::atomic::wait().

#include "tmc/detail/compat.hpp"
#include "tmc/detail/concepts_awaitable.hpp"
#include "tmc/detail/thread_locals.hpp"
#include "tmc/detail/waiter_list.hpp"

#include <atomic>
#include <cassert>
#include <coroutine>
#include <cstddef>

namespace tmc::tests {
class waiter_count_accessor;
}

namespace tmc {
template <typename T> class atomic_condvar;
template <typename T> class aw_atomic_condvar;
template <typename T> class aw_atomic_condvar_co_notify;

/// The awaiter type produced by co_awaiting aw_atomic_condvar. It is
/// constructed in place in the awaiting coroutine's frame, where it lives
/// across the suspension.
template <typename T>
class aw_atomic_condvar_impl {
  // Intrusive next pointer for the parent's waiter list. Only manipulated by
  // the producer (before the node is published) and by the single consumer
  // that currently owns the parent's WAKING critical section.
  aw_atomic_condvar_impl<T>* next;
  T expected;
  atomic_condvar<T>& parent;
  tmc::detail::waiter_list_waiter waiter;

  friend class aw_atomic_condvar<T>;
  friend class atomic_condvar<T>;
  friend class aw_atomic_condvar_co_notify<T>;

  inline aw_atomic_condvar_impl(
    atomic_condvar<T>& Parent, T Expected
  ) noexcept
      : expected(Expected), parent(Parent) {}

public:
  inline bool await_ready() noexcept {
    // The user has free access to this atomic variable and may execute SeqCst
    // stores. These stores aren't synchronized via the waiter list if the
    // setter is on a different thread than the notifier. So if the user expects
    // SeqCst in this case then this is the only way to provide it.
    return parent.value.load(std::memory_order_seq_cst) != expected;
  }

  inline bool await_suspend(std::coroutine_handle<> Outer) noexcept {
    // Configure this awaiter
    waiter.continuation = Outer;
    waiter.continuation_executor = tmc::detail::this_thread::executor();
    waiter.continuation_priority = tmc::detail::this_thread::this_task().prio;

    // Capture everything we need into locals before publishing this node. The
    // instant `push` makes this node visible, a concurrent notifier may take
    // and resume it, after which touching any member of `this` is UB. See the
    // analogous comment in aw_acquire::await_suspend / rw_lock.
    atomic_condvar<T>* p = &parent;
    T exp = expected;

    // Count this waiter as registered before publishing it, so the count is
    // never observed too low for a node that is about to enter the list.
    p->waiter_total.fetch_add(1, std::memory_order_relaxed);

    // Publish this node onto the lock-free input stack.
    p->push(this);

    // StoreLoad barrier between publishing the node and loading the value.
    // This is the half of the lost-wakeup handshake that the old std::mutex
    // used to provide implicitly; a notifier performs the mirror-image
    // (store value; seq_cst exchange of the input stack) in `consume`.
    std::atomic_thread_fence(std::memory_order_seq_cst);

    if (p->value.load(std::memory_order_seq_cst) == exp) {
      // The value still matches; remain suspended. A future notifier (or the
      // notifier that is concurrently changing the value) will wake us.
      return true;
    }

    // The value changed between await_ready and the publish above; a notifier
    // may have already scanned the list without seeing us. Drive a consumer
    // pass to guarantee we (and any other now-eligible waiter) are woken. The
    // returned chain may include this very node.
    aw_atomic_condvar_impl<T>* chain = p->consume(TMC_ALL_ONES);
    while (chain != nullptr) {
      // Read next before resume; resume may destroy the node.
      aw_atomic_condvar_impl<T>* n = chain->next;
      chain->waiter.resume();
      chain = n;
    }
    return true;
  }

  inline void await_resume() noexcept {}

  // Cannot be moved or copied due to holding intrusive list pointer
  aw_atomic_condvar_impl(aw_atomic_condvar_impl const&) = delete;
  aw_atomic_condvar_impl& operator=(aw_atomic_condvar_impl const&) = delete;
  aw_atomic_condvar_impl(aw_atomic_condvar_impl&&) = delete;
  aw_atomic_condvar_impl& operator=(aw_atomic_condvar_impl&&) = delete;
};

/// The awaitable type returned by `atomic_condvar.await()`
template <typename T>
class [[nodiscard(
  "You must co_await aw_atomic_condvar for it to have any effect."
)]] aw_atomic_condvar : tmc::detail::AwaitTagNoGroupCoAwait {
  atomic_condvar<T>* parent;
  T expected;

  friend class atomic_condvar<T>;

  inline aw_atomic_condvar(
    atomic_condvar<T>& Parent TMC_LIFETIMEBOUND, T Expected
  ) noexcept
      : parent(&Parent), expected(Expected) {}

public:
  inline aw_atomic_condvar_impl<T> operator co_await() && noexcept {
    assert(parent != nullptr && "aw_atomic_condvar may only be awaited once");
    return aw_atomic_condvar_impl<T>(*parent, expected);
  }

  // Movable but not copyable
  aw_atomic_condvar(aw_atomic_condvar const&) = delete;
  aw_atomic_condvar& operator=(aw_atomic_condvar const&) = delete;
  inline aw_atomic_condvar(aw_atomic_condvar&& Other) noexcept
      : parent(Other.parent), expected(Other.expected) {
    Other.parent = nullptr;
  }
  aw_atomic_condvar& operator=(aw_atomic_condvar&&) = delete;
};

/// The awaitable type returned by `atomic_condvar.co_notify_one()`,
/// `atomic_condvar.co_notify_n()`, and `atomic_condvar.co_notify_all()`.
template <typename T>
class [[nodiscard(
  "You must co_await aw_atomic_condvar_co_notify for it to have any effect."
)]] aw_atomic_condvar_co_notify : tmc::detail::AwaitTagNoGroupAsIs {
  atomic_condvar<T>& parent;
  size_t notify_count;

  friend class atomic_condvar<T>;

  inline aw_atomic_condvar_co_notify(
    atomic_condvar<T>& Parent TMC_LIFETIMEBOUND, size_t NotifyCount
  ) noexcept
      : parent(Parent), notify_count(NotifyCount) {}

public:
  inline bool await_ready() noexcept { return notify_count == 0; }

  inline std::coroutine_handle<>
  await_suspend(std::coroutine_handle<> Outer) noexcept {
    aw_atomic_condvar_impl<T>* chain = parent.consume(notify_count);
    if (chain == nullptr) {
      // Either nothing was eligible, or another consumer holds the critical
      // section and will wake the eligible waiters on our behalf (without
      // symmetric transfer). Resume the caller directly.
      return Outer;
    }
    // Save the first / most recently added waiter for symmetric transfer, and
    // resume the rest by posting them to their executors.
    aw_atomic_condvar_impl<T>* first = chain;
    aw_atomic_condvar_impl<T>* rest = chain->next;
    while (rest != nullptr) {
      aw_atomic_condvar_impl<T>* n = rest->next;
      rest->waiter.resume();
      rest = n;
    }
    return first->waiter.try_symmetric_transfer(Outer);
  }

  inline void await_resume() noexcept {}

  // Movable so that it can be captured by value into a wrapper task when
  // passed to spawn() / fork(), but not copyable.
  aw_atomic_condvar_co_notify(aw_atomic_condvar_co_notify const&) = delete;
  aw_atomic_condvar_co_notify&
  operator=(aw_atomic_condvar_co_notify const&) = delete;
  aw_atomic_condvar_co_notify(aw_atomic_condvar_co_notify&&) = default;
  aw_atomic_condvar_co_notify&
  operator=(aw_atomic_condvar_co_notify&&) = delete;
};

/// Wraps an atomic integral type. Exposes async analogues to
/// `std::atomic<T>::wait()` and `std::atomic<T>::notify_*()`.
///
/// The waiter list is lock-free. Producers (`await`) publish onto a Treiber
/// input stack and never block. The consumer side (`notify_*`, `co_notify_*`,
/// and the producer's post-publish recheck) is serialized by a single-consumer
/// WAKING critical section, in the same style as tmc::rw_lock: a thread that
/// fails to claim WAKING sets PENDING and returns, and the thread that holds
/// WAKING re-scans before releasing. No operation ever blocks or spins.
///
/// `notify_n(N)` wakes up to N waiters whose `expected != value`. The count is
/// exact when notifies are not run concurrently with other notifies/producers;
/// under concurrency it remains "wake up to N", but a racing producer or
/// notifier may cause additional already-eligible waiters to be woken (which is
/// always safe, since their wait condition is genuinely satisfied).
template <typename T> class atomic_condvar {
  std::atomic<T> value;

  // Lock-free Treiber stack of newly-published waiters (LIFO).
  std::atomic<aw_atomic_condvar_impl<T>*> input{nullptr};
  // Private list of waiters whose condition was not yet met. Only touched by
  // the thread currently holding the WAKING bit; handed off between consumers
  // through the acquire/release on `consume_state`.
  aw_atomic_condvar_impl<T>* output{nullptr};

  // WAKING guards the single-consumer side of the waiter list. PENDING records
  // that another thread requested a wake while WAKING was held, forcing the
  // holder to re-scan before releasing.
  static inline constexpr size_t WAKING = 1;
  static inline constexpr size_t PENDING = 2;
  std::atomic<size_t> consume_state{0};

  // Number of registered (suspended, not-yet-woken) waiters. Maintained
  // separately so waiter_count() is trivially thread-safe.
  std::atomic<size_t> waiter_total{0};

  friend class aw_atomic_condvar_impl<T>;
  friend class aw_atomic_condvar_co_notify<T>;
  friend class ::tmc::tests::waiter_count_accessor;

  // Returns the number of awaiters currently registered (suspended) on this
  // condvar. For testing purposes. Thread-safe.
  inline size_t waiter_count() noexcept {
    return waiter_total.load(std::memory_order_acquire);
  }

  // Publishes w onto the lock-free input stack. Seq-cst on success so the push
  // participates in the total order that backs the lost-wakeup handshake.
  inline void push(aw_atomic_condvar_impl<T>* w) noexcept {
    auto h = input.load(std::memory_order_acquire);
    do {
      w->next = h;
    } while (!input.compare_exchange_weak(
      h, w, std::memory_order_seq_cst, std::memory_order_acquire
    ));
  }

  // Claims the WAKING critical section and wakes up to MaxWake waiters whose
  // `expected != value`. Returns a singly-linked chain (via `next`) of the
  // nodes that were removed and must be resumed by the caller; returns nullptr
  // if nothing was woken or if another consumer already holds WAKING (in which
  // case that consumer will service the request via PENDING). Decrements
  // waiter_total for each node returned.
  aw_atomic_condvar_impl<T>* consume(size_t MaxWake) noexcept {
    // Acquire the WAKING bit, or delegate via PENDING if it is held.
    size_t s = consume_state.load(std::memory_order_acquire);
    while (true) {
      if ((s & WAKING) != 0) {
        if (consume_state.compare_exchange_weak(
              s, s | PENDING, std::memory_order_acq_rel,
              std::memory_order_acquire
            )) {
          return nullptr;
        }
        continue;
      }
      if (consume_state.compare_exchange_weak(
            s, WAKING, std::memory_order_acq_rel, std::memory_order_acquire
          )) {
        break;
      }
    }

    aw_atomic_condvar_impl<T>* wakeHead = nullptr;
    aw_atomic_condvar_impl<T>** wakeTail = &wakeHead;
    size_t budget = MaxWake;

    while (true) {
      // Start a fresh pass. Clear PENDING first so that any request arriving
      // after this point keeps WAKING held (via a failed release CAS below).
      consume_state.fetch_and(~PENDING, std::memory_order_relaxed);

      // Mirror of the producer's StoreLoad barrier: order the value load and
      // the input drain after any prior store, so a producer that published
      // after our drain is guaranteed to have observed our value store side.
      std::atomic_thread_fence(std::memory_order_seq_cst);
      T v = value.load(std::memory_order_seq_cst);

      // Drain the input stack and merge it in front of output (order is
      // irrelevant; wake order is LIFO-ish, matching the prior implementation).
      aw_atomic_condvar_impl<T>* in =
        input.exchange(nullptr, std::memory_order_seq_cst);
      if (in != nullptr) {
        aw_atomic_condvar_impl<T>* tail = in;
        while (tail->next != nullptr) {
          tail = tail->next;
        }
        tail->next = output;
        output = in;
      }

      // Walk output, unlinking up to `budget` nodes whose condition is met.
      aw_atomic_condvar_impl<T>* prev = nullptr;
      aw_atomic_condvar_impl<T>* cur = output;
      while (cur != nullptr && budget != 0) {
        if (cur->expected != v) {
          aw_atomic_condvar_impl<T>* nxt = cur->next;
          if (prev != nullptr) {
            prev->next = nxt;
          } else {
            output = nxt;
          }
          cur->next = nullptr;
          *wakeTail = cur;
          wakeTail = &cur->next;
          if (budget != TMC_ALL_ONES) {
            --budget;
          }
          waiter_total.fetch_sub(1, std::memory_order_relaxed);
          cur = nxt;
        } else {
          prev = cur;
          cur = cur->next;
        }
      }

      // Try to release WAKING. If PENDING was set during the pass, the CAS
      // fails and we run another pass. A re-scan services a producer recheck or
      // a concurrent notifier, both of which want all currently-eligible
      // waiters flushed, so lift the budget for subsequent passes.
      size_t expected_state = WAKING;
      if (consume_state.compare_exchange_strong(
            expected_state, 0, std::memory_order_acq_rel,
            std::memory_order_acquire
          )) {
        return wakeHead;
      }
      budget = TMC_ALL_ONES;
    }
  }

public:
  /// Sets the initial value of the contained atomic variable.
  inline atomic_condvar(T InitialValue) noexcept : value{InitialValue} {}

  /// Returns a reference to the contained atomic variable.
  inline std::atomic<T>& ref() noexcept TMC_LIFETIMEBOUND { return value; }

  /// Returns a reference to the contained atomic variable.
  inline const std::atomic<T>& ref() const noexcept { return value; }

  /// Wakes 1 awaiter that meet the criteria (expected != current value).
  /// The awaiter may be resumed by symmetric transfer if it is eligible
  /// (it resumes on the same executor and priority as the caller).
  /// Awaiters are woken in LIFO order.
  inline aw_atomic_condvar_co_notify<T>
  co_notify_one(size_t NotifyCount = 1) noexcept TMC_LIFETIMEBOUND {
    return aw_atomic_condvar_co_notify<T>(*this, NotifyCount);
  }

  /// Wakes up to NotifyCount awaiters that meet the criteria (expected !=
  /// current value).
  /// Up to one awaiter may be resumed by symmetric transfer if it is eligible
  /// (it resumes on the same executor and priority as the caller).
  /// Awaiters are woken in LIFO order.
  inline aw_atomic_condvar_co_notify<T>
  co_notify_n(size_t NotifyCount = 1) noexcept TMC_LIFETIMEBOUND {
    return aw_atomic_condvar_co_notify<T>(*this, NotifyCount);
  }

  /// Wakes all awaiters that meet the criteria (expected != current value).
  /// Up to one awaiter may be resumed by symmetric transfer if it is eligible
  /// (it resumes on the same executor and priority as the caller).
  inline aw_atomic_condvar_co_notify<T> co_notify_all() noexcept TMC_LIFETIMEBOUND {
    return aw_atomic_condvar_co_notify<T>(*this, TMC_ALL_ONES);
  }

  /// Wakes 1 awaiter that meet the criteria (expected != current value).
  /// Does not symmetric transfer; the awaiter will be posted to its executor.
  /// Awaiters are woken in LIFO order.
  inline void notify_one() { notify_n(1); }

  /// Wakes up to NotifyCount awaiters that meet the criteria (expected !=
  /// current value).
  /// Does not symmetric transfer; awaiters will be posted to their executors.
  /// Awaiters are woken in LIFO order.
  inline void notify_n(size_t NotifyCount = 1) {
    if (NotifyCount == 0) {
      return;
    }
    aw_atomic_condvar_impl<T>* chain = consume(NotifyCount);
    while (chain != nullptr) {
      aw_atomic_condvar_impl<T>* n = chain->next;
      chain->waiter.resume();
      chain = n;
    }
  }

  /// Wakes all awaiters that meet the criteria (expected != current value).
  /// Does not symmetric transfer; awaiters will be posted to their executors.
  inline void notify_all() {
    aw_atomic_condvar_impl<T>* chain = consume(TMC_ALL_ONES);
    while (chain != nullptr) {
      aw_atomic_condvar_impl<T>* n = chain->next;
      chain->waiter.resume();
      chain = n;
    }
  }

  /// Suspends until Expected != current value. If this condition is already
  /// true, resumes immediately.
  inline aw_atomic_condvar<T> await(T Expected) noexcept TMC_LIFETIMEBOUND {
    return aw_atomic_condvar<T>(*this, Expected);
  }

  /// On destruction, any awaiters will be resumed. The user must ensure no
  /// other thread is concurrently operating on this condvar during destruction.
  inline ~atomic_condvar() {
    // Drain both lists unconditionally and resume everything.
    aw_atomic_condvar_impl<T>* chain =
      input.exchange(nullptr, std::memory_order_acquire);
    if (chain != nullptr) {
      aw_atomic_condvar_impl<T>* tail = chain;
      while (tail->next != nullptr) {
        tail = tail->next;
      }
      tail->next = output;
    } else {
      chain = output;
    }
    output = nullptr;
    while (chain != nullptr) {
      aw_atomic_condvar_impl<T>* n = chain->next;
      waiter_total.fetch_sub(1, std::memory_order_relaxed);
      chain->waiter.resume();
      chain = n;
    }
  }
};
} // namespace tmc
