// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

// Provides tmc::qu_mpsc_bounded, an async MPSC bounded linearizable queue.
// All enqueue and dequeue operations are zero-copy.
// A single allocation is created in the constructor. Subsequent operations are
// allocation-free.

// try_push() and push_bulk() operations are not provided because they cannot be
// implemented correctly for this queue scheme. I also tested a
// tmc::semaphore-based version, but it had the same constraints:
// - try_push() cannot reliably acquire a slot without possibly failing and
// being forced to suspend.
// - push_bulk() cannot acquire a contiguous block of slots.

#include "tmc/detail/compat.hpp"
#include "tmc/detail/concepts_awaitable.hpp"
#include "tmc/detail/qu_storage.hpp"
#include "tmc/detail/thread_locals.hpp"
#include "tmc/ex_any.hpp"

#include <atomic>
#include <cassert>
#include <coroutine>
#include <cstdint>
#include <tuple>
#include <type_traits>
#include <utility>

namespace tmc {
struct qu_mpsc_bounded_default_config {
  /// If true, enables the suspending `pull()` operation.
  static inline constexpr bool ConsumerCanSuspend = true;

  /// At level 0, queue elements will be padded up to the next increment of
  /// TMC_CACHE_LINE_SIZE bytes. This reduces false sharing between neighboring
  /// elements.
  /// At level 1, no padding will be applied.
  static inline constexpr size_t PackingLevel = 0;
};

/// Status code returned by qu_mpsc_bounded.try_pull().status()
enum class qu_mpsc_bounded_err { OK, EMPTY, CLOSED };

template <typename T, typename Config = tmc::qu_mpsc_bounded_default_config>
class qu_mpsc_bounded {
  static inline constexpr bool ConsumerCanSuspend = Config::ConsumerCanSuspend;

  // Flag bits in element::flags. Upper bits encode the input producer_base*
  // chain head (low 3 bits guaranteed 0 by alignment).
  static inline constexpr uintptr_t PROD_BIT = TMC_ONE_BIT;
  static inline constexpr uintptr_t DATA_BIT = TMC_ONE_BIT << 1;
  static inline constexpr uintptr_t CONS_BIT = TMC_ONE_BIT << 2;
  static inline constexpr uintptr_t FLAG_MASK = PROD_BIT | DATA_BIT | CONS_BIT;
  static inline constexpr uintptr_t CHAIN_MASK = ~FLAG_MASK;
  static inline constexpr uintptr_t CLOSE_SENTINEL = CHAIN_MASK;

  struct element;

  // A suspended producer. Lives in the producer's coroutine frame.
  struct alignas(8) producer_base {
    producer_base* next; // Input/output producer chain link.
    tmc::ex_any* continuation_executor;
    std::coroutine_handle<> continuation;
    size_t prio;
  };

  // A suspended consumer. Lives in the consumer's coroutine frame.
  struct alignas(8) consumer_base {
    tmc::ex_any* continuation_executor;
    std::coroutine_handle<> continuation;
    size_t prio;
    element* elem;
    size_t idx; // Logical offset
  };

  // 3 low bits are used for flags. In order to ensure 8-byte alignment on
  // 32-bit machines, we also decorate these structures with alignas(8).
  static_assert(alignof(producer_base) >= 8);
  static_assert(alignof(consumer_base) >= 8);
  static_assert(Config::PackingLevel < 2);

  struct element {
    // Encodes {PROD_BIT, DATA_BIT, CONS_BIT, input_producer_chain_head*}.
    // States:
    //   0                         empty (claimable)
    //   CONS_BIT                  empty, consumer waiting (claimable)
    //   PROD_BIT                  producer claimed, currently writing
    //   PROD_BIT | CONS_BIT       producer writing, consumer waiting
    //   DATA_BIT                  data published, no waiters
    //   PROD_BIT | chain          producer writing, others parked
    //   PROD_BIT | CONS_BIT | chain
    //                             producer writing, consumer waiting, others
    //                             parked
    //   DATA_BIT | chain          data published, producers parked
    //   CLOSE_SENTINEL            close marker at this physical slot
    //   chain alone               unreachable in steady state, except sentinel
    //   PROD_BIT | DATA_BIT       unreachable (CAS transitions PROD->DATA)
    std::atomic<uintptr_t> flags;

    // Consumer-owned output producer list. Producers never read or write this;
    // the consumer fills it by draining and reversing the atomic input chain
    // from flags, then pops from it to wake producers in FIFO order.
    producer_base* output_producers;

    // Suspended consumer pointer or nullptr. Written exclusively by the
    // single consumer (install) before setting flags | CONS_BIT, and modified
    // by producers / close() (extract via exchange / CAS).
    std::atomic<consumer_base*> waiting_consumer;

    // Logical offset of the suspended consumer, copied out of the consumer's
    // coroutine frame BEFORE waiting_consumer is published. close() reads
    // this field (not cons->idx) to decide whether to wake at cutoff, so it
    // never dereferences a coroutine frame that may be concurrently resumed
    // and destroyed.
    std::atomic<size_t> waiting_consumer_idx;

    tmc::detail::qu_storage<T> data;

    static constexpr size_t UNPADLEN =
      sizeof(std::atomic<uintptr_t>) + sizeof(producer_base*) +
      sizeof(std::atomic<consumer_base*>) + sizeof(std::atomic<size_t>) +
      sizeof(tmc::detail::qu_storage<T>);
    static constexpr size_t WANTLEN =
      (UNPADLEN + TMC_CACHE_LINE_SIZE - 1) &
      static_cast<size_t>(0 - TMC_CACHE_LINE_SIZE);
    static constexpr size_t PADLEN =
      UNPADLEN < WANTLEN ? (WANTLEN - UNPADLEN) : 999;

    struct empty {};
    using Padding = std::conditional_t<
      Config::PackingLevel == 0 && PADLEN != 999, char[PADLEN], empty>;
    TMC_NO_UNIQUE_ADDRESS Padding pad;

    void init() noexcept {
      flags.store(0, std::memory_order_relaxed);
      output_producers = nullptr;
      waiting_consumer.store(nullptr, std::memory_order_relaxed);
      waiting_consumer_idx.store(0, std::memory_order_relaxed);
    }
  };

  char pad0[TMC_CACHE_LINE_SIZE - sizeof(size_t)];
  // Producer's hot field, written on every push
  std::atomic<size_t> write_offset;
  char pad1[TMC_CACHE_LINE_SIZE - sizeof(size_t)];
  // Read and written only by consumer
  size_t read_offset;
  char pad2[TMC_CACHE_LINE_SIZE - sizeof(size_t)];
  // Producer/consumer shared read-only data

  // Constructor-initialized, never modified
  size_t capacity;
  element* values;

  // Mostly read-only; written exactly once by close()
  std::atomic<bool> closed;
  std::atomic<bool> closed_ready;
  std::atomic<size_t> write_closed_at;
  char pad3[TMC_CACHE_LINE_SIZE - sizeof(size_t)];

public:
  class aw_pull;

  /// A zero-copy handle to an object in the queue's storage. The object is
  /// exclusively available to this handle. When this handle is destroyed, the
  /// queued object will be destroyed and the queue slot will be freed for
  /// reuse. Returned by `try_pull()`.
  ///
  /// The status of the pull is exposed via `status()`:
  /// `qu_mpsc_bounded_err::OK` if a value is held, `EMPTY` if no value was
  /// available, or `CLOSED` if the queue has been closed and drained.
  class try_pull_zc_scope {
    friend qu_mpsc_bounded;
    qu_mpsc_bounded* queue;
    element* elem;
    size_t idx;
    tmc::qu_mpsc_bounded_err err;

    try_pull_zc_scope(
      qu_mpsc_bounded* Queue, element* Elem, size_t Idx
    ) noexcept
        : queue{Queue}, elem{Elem}, idx{Idx},
          err{tmc::qu_mpsc_bounded_err::OK} {}

    explicit try_pull_zc_scope(tmc::qu_mpsc_bounded_err Err) noexcept
        : queue{nullptr}, elem{nullptr}, idx{0}, err{Err} {}

  public:
    /// Constructs an empty scope (status EMPTY). Evaluates to false when
    /// converted to bool.
    try_pull_zc_scope() noexcept
        : queue{nullptr}, elem{nullptr}, idx{0},
          err{tmc::qu_mpsc_bounded_err::EMPTY} {}

    try_pull_zc_scope(const try_pull_zc_scope&) = delete;
    try_pull_zc_scope& operator=(const try_pull_zc_scope&) = delete;

    try_pull_zc_scope(try_pull_zc_scope&& Other) noexcept
        : queue{Other.queue}, elem{Other.elem}, idx{Other.idx}, err{Other.err} {
      Other.elem = nullptr;
      Other.err = tmc::qu_mpsc_bounded_err::EMPTY;
    }

    try_pull_zc_scope& operator=(try_pull_zc_scope&& Other) noexcept {
      if (this != &Other) {
        if (elem != nullptr) {
          queue->finish_read(elem, idx);
          elem = nullptr;
        }
        queue = Other.queue;
        elem = Other.elem;
        idx = Other.idx;
        err = Other.err;
        Other.elem = nullptr;
        Other.err = tmc::qu_mpsc_bounded_err::EMPTY;
      }
      return *this;
    }

    /// Returns true if this scope holds a value from the queue (status == OK).
    explicit operator bool() const noexcept { return elem != nullptr; }

    /// Returns true if this scope holds a value from the queue (status == OK).
    bool has_value() const noexcept { return elem != nullptr; }

    /// Returns the status of this pull: OK, EMPTY, or CLOSED.
    tmc::qu_mpsc_bounded_err status() const noexcept { return err; }

    /// Returns a reference to the object in the queue storage.
    /// Only valid to call if `status()` is OK / `operator bool()` is true.
    T& value() noexcept { return elem->data.value; }

    /// Returns a reference to the object in the queue storage.
    /// Only valid to call if `status()` is OK / `operator bool()` is true.
    T& operator*() noexcept { return elem->data.value; }

    /// Returns a pointer to the object in the queue storage.
    /// Only valid to call if `status()` is OK / `operator bool()` is true.
    T* operator->() noexcept { return &elem->data.value; }

    /// Destroys the object in the queue storage and releases the queue slot.
    ~try_pull_zc_scope() {
      if (elem != nullptr) {
        queue->finish_read(elem, idx);
        elem = nullptr;
      }
    }
  };

  /// A zero-copy handle to an object in the queue's storage. The object is
  /// exclusively available to this handle. When this handle is destroyed, the
  /// queued object will be destroyed and the queue slot will be freed for
  /// reuse. Returned by `co_await pull()`.
  ///
  /// If the queue has been closed and is drained, `pull()` will resume
  /// with an empty `pull_zc_scope` (operator bool returns false).
  class pull_zc_scope {
    friend qu_mpsc_bounded;
    qu_mpsc_bounded* queue;
    element* elem;
    size_t idx;

    pull_zc_scope(qu_mpsc_bounded* Queue, element* Elem, size_t Idx) noexcept
        : queue{Queue}, elem{Elem}, idx{Idx} {}

  public:
    /// Constructs an empty scope. Evaluates to false when converted to bool.
    pull_zc_scope() noexcept : queue{nullptr}, elem{nullptr}, idx{0} {}

    pull_zc_scope(const pull_zc_scope&) = delete;
    pull_zc_scope& operator=(const pull_zc_scope&) = delete;

    pull_zc_scope(pull_zc_scope&& Other) noexcept
        : queue{Other.queue}, elem{Other.elem}, idx{Other.idx} {
      Other.elem = nullptr;
    }

    /// Returns true if this scope holds a value from the queue.
    explicit operator bool() const noexcept { return elem != nullptr; }

    /// Returns true if this scope holds a value from the queue.
    bool has_value() const noexcept { return elem != nullptr; }

    pull_zc_scope& operator=(pull_zc_scope&& Other) noexcept {
      if (this != &Other) {
        if (elem != nullptr) {
          queue->finish_read(elem, idx);
          elem = nullptr;
        }
        queue = Other.queue;
        elem = Other.elem;
        idx = Other.idx;
        Other.elem = nullptr;
      }
      return *this;
    }

    /// Returns a reference to the object in the queue storage.
    /// Only valid to call if `operator bool()` is true.
    T& value() noexcept { return elem->data.value; }

    /// Returns a reference to the object in the queue storage.
    /// Only valid to call if `operator bool()` is true.
    T& operator*() noexcept { return elem->data.value; }

    /// Returns a pointer to the object in the queue storage.
    /// Only valid to call if `operator bool()` is true.
    T* operator->() noexcept { return &elem->data.value; }

    /// Destroys the object in the queue storage and releases the queue slot.
    TMC_FORCE_INLINE ~pull_zc_scope() {
      if (elem != nullptr) [[likely]] {
        queue->finish_read(elem, idx);
        elem = nullptr;
      }
    }
  };

  /// Constructs a qu_mpsc_bounded with the given capacity.
  explicit qu_mpsc_bounded(size_t Capacity) noexcept
      : capacity{Capacity}, values{new element[Capacity]} {
    assert(Capacity > 0 && "Capacity must be greater than 0");
    // Ensure that the subtraction of unsigned offsets always results in a
    // value that can be represented as a signed integer.
    assert(
      Capacity <= (TMC_ONE_BIT << (TMC_PLATFORM_BITS - 1)) &&
      "Capacity must not be larger than half the max value that can be "
      "represented by a platform word"
    );
    write_offset.store(0, std::memory_order_relaxed);
    closed.store(false, std::memory_order_relaxed);
    write_closed_at.store(0, std::memory_order_relaxed);
    closed_ready.store(false, std::memory_order_relaxed);
    read_offset = 0;
    for (size_t i = 0; i < capacity; ++i) {
      values[i].init();
    }
    tmc::detail::memory_barrier();
  }

private:
  static inline bool circular_less_than(size_t a, size_t b) noexcept {
    return a - b > (TMC_ONE_BIT << (TMC_PLATFORM_BITS - 1));
  }

  // Extract the chain-head pointer encoded in the upper bits of a flags word.
  static inline producer_base* chain_ptr(uintptr_t Flags) noexcept {
    return reinterpret_cast<producer_base*>(Flags & CHAIN_MASK);
  }

  static inline bool has_close_sentinel(uintptr_t Flags) noexcept {
    return (Flags & CHAIN_MASK) == CLOSE_SENTINEL;
  }

  // Reverses a singly-linked producer chain in place. The caller must have
  // exclusive access to the chain.
  static inline producer_base* reverse_chain(producer_base* curr) noexcept {
    producer_base* prev = nullptr;
    while (curr != nullptr) {
      producer_base* next = curr->next;
      curr->next = prev;
      prev = curr;
      curr = next;
    }
    return prev;
  }

  // FAA write_offset and check for post-close. Returns nullptr if the
  // reservation is past the close cutoff (caller must bail). Otherwise
  // returns the picked slot.
  //
  // Producers use seq_cst fetch_add on `write_offset` to pick a physical slot.
  // Multiple producers whose reservations hash to the same slot are serialized
  // by an intrusive Treiber chain on the slot's `flags` word. When the consumer
  // drains a slot, it wakes producers in FIFO order by reversing that input
  // chain into a consumer-owned output chain. The handoff CAS clears DATA_BIT
  // and sets PROD_BIT on the selected producer's behalf - the woken producer
  // then proceeds directly to emplacing its data without re-racing for the
  // slot.
  //
  // Ordering semantics: FAA on write_offset serves only to find the correct
  // slot. CAS on that slot's flags is the source of truth for a producer's
  // output index. For example if producer A gets 0 from FAA and producer B gets
  // 2 from FAA, but then producer B wins the CAS race, then producer B
  // effectively becomes the index 0 producer, and producer A becomes the index
  // 2 producer. This behavior is as-if the only racing operation was FAA, and
  // producer B won that race.
  element* get_write_ticket() noexcept {
    size_t idx = write_offset.fetch_add(1, std::memory_order_seq_cst);
    if (closed.load(std::memory_order_acquire)) [[unlikely]] {
      // Spin until the closer has published write_closed_at.
      while (!closed_ready.load(std::memory_order_acquire)) {
        TMC_CPU_PAUSE();
      }
      if (!circular_less_than(
            idx, write_closed_at.load(std::memory_order_acquire)
          )) {
        return nullptr;
      }
    }
    return &values[idx % capacity];
  }

  enum class producer_wait_result { CLAIMED, PARKED, RETRY, CLOSED };

  // Attempt to claim the slot for writing. On success, the caller owns the
  // slot and must subsequently call producer_publish. A bare CONS_BIT still
  // represents an empty slot, but the bit must be preserved so
  // producer_publish can wake the waiting consumer.
  producer_wait_result producer_try_claim(element* Elem) noexcept {
    uintptr_t cur = Elem->flags.load(std::memory_order_acquire);
    while ((cur & ~CONS_BIT) == 0) {
      if (Elem->flags.compare_exchange_weak(
            cur, cur | PROD_BIT, std::memory_order_acq_rel,
            std::memory_order_acquire
          )) {
        return producer_wait_result::CLAIMED;
      }
    }
    if (has_close_sentinel(cur)) {
      return producer_wait_result::CLOSED;
    }
    return producer_wait_result::RETRY;
  }

  // Try to push MyNode onto the slot's chain.
  producer_wait_result
  producer_park(element* Elem, producer_base* MyNode) noexcept {
    uintptr_t cur = Elem->flags.load(std::memory_order_acquire);
    while (true) {
      if ((cur & ~CONS_BIT) == 0) {
        // Slot raced free; caller should retry producer_try_claim.
        return producer_wait_result::RETRY;
      }
      if (has_close_sentinel(cur)) {
        return producer_wait_result::CLOSED;
      }
      MyNode->next = chain_ptr(cur);
      uintptr_t newv = (cur & FLAG_MASK) | reinterpret_cast<uintptr_t>(MyNode);
      if (Elem->flags.compare_exchange_weak(
            cur, newv, std::memory_order_acq_rel, std::memory_order_acquire
          )) {
        return producer_wait_result::PARKED;
      }
      // cur was updated by the failed CAS; loop with the fresh value.
    }
  }

  // Transition this slot from PROD_BIT -> DATA_BIT, preserving any chain
  // that arrived during our write. Then atomically extract and return any
  // suspended consumer at this slot (caller wakes it).
  consumer_base* producer_publish(element* Elem) noexcept {
    uintptr_t cur = Elem->flags.load(std::memory_order_acquire);
    while (true) {
      // Invariant: we hold PROD_BIT; DATA_BIT must be clear.
      assert((cur & PROD_BIT) != 0);
      assert((cur & DATA_BIT) == 0);
      uintptr_t newv = (cur & ~(PROD_BIT | CONS_BIT)) | DATA_BIT;
      if (Elem->flags.compare_exchange_weak(
            cur, newv, std::memory_order_acq_rel, std::memory_order_acquire
          )) {
        break;
      }
    }
    if constexpr (ConsumerCanSuspend) {
      if ((cur & CONS_BIT) == 0) {
        return nullptr;
      }
      consumer_base* cons =
        Elem->waiting_consumer.exchange(nullptr, std::memory_order_acq_rel);
      return cons;
    } else {
      return nullptr;
    }
  }

  // Called by the consumer after destroying the slot's data. Atomically
  // clears DATA_BIT; if producers are waiting, sets PROD_BIT on the selected
  // producer's behalf. This ensures that another producer cannot steal the slot
  // before the selected producer wakes.
  //
  // Producers are selected from the consumer-owned output list first. When
  // output is empty, this drains the atomic input stack from flags and reverses
  // it into output so producers are woken FIFO. Returns the selected producer
  // (caller must wake it) or nullptr.
  producer_base* consumer_drain_and_handoff(element* Elem) noexcept {
    uintptr_t cur = Elem->flags.load(std::memory_order_acquire);
    if (Elem->output_producers != nullptr) {
      while (true) {
        // Invariant: DATA_BIT set, PROD_BIT clear (publishing producer
        // already transitioned). Concurrent operations only push onto the
        // input chain (preserving DATA_BIT).
        assert((cur & DATA_BIT) != 0);
        assert((cur & PROD_BIT) == 0);
        // Wake the first element of output_producers. Claim PROD_BIT for it but
        // don't modify the flags waiter list otherwise.
        producer_base* toWake = Elem->output_producers;
        uintptr_t newv = PROD_BIT | (cur & CHAIN_MASK);
        if (Elem->flags.compare_exchange_weak(
              cur, newv, std::memory_order_acq_rel, std::memory_order_acquire
            )) {
          Elem->output_producers = toWake->next;
          return toWake;
        }
      }
    }

    // output_producers is empty.
    // Invariant: DATA_BIT set, PROD_BIT clear (publishing producer
    // already transitioned). Concurrent operations only push onto the
    // input chain (preserving DATA_BIT).
    assert((cur & DATA_BIT) != 0);
    assert((cur & PROD_BIT) == 0);

    if (cur == DATA_BIT) {
      // We think there are no producers waiting, but we need to CAS to be
      // sure, because if a producer raced and inserted itself, then we need
      // to set PROD_BIT instead of 0, so that we can maintain FIFO ordering.
      if (Elem->flags.compare_exchange_strong(
            cur, 0, std::memory_order_acq_rel, std::memory_order_acquire
          )) {
        return nullptr;
      }
    }
    // There's at least 1 producer waiting. Take the input producer chain
    // and set PROD_BIT so we can wake the first producer after reversing
    // the list into FIFO order.
    producer_base* input =
      chain_ptr(Elem->flags.exchange(PROD_BIT, std::memory_order_acq_rel));
    producer_base* toWake = reverse_chain(input);
    Elem->output_producers = toWake->next;
    return toWake;
  }

  // Called from the pull / try_pull scope destructor.
  void finish_read(element* Elem, size_t Idx) noexcept {
    Elem->data.destroy();
    read_offset = Idx + 1;
    producer_base* next = consumer_drain_and_handoff(Elem);
    if (next != nullptr) {
      tmc::detail::post_checked(
        next->continuation_executor, std::move(next->continuation), next->prio
      );
    }
  }

public:
  template <typename... Args> class aw_push;

  /// Enqueues a new value in the queue by in-place construction, forwarding
  /// `ConstructArgs` to T's constructor.
  ///
  /// Returns an awaitable. If the queue is full, the producer suspends until
  /// the consumer reads a value and frees a slot. Otherwise it completes
  /// synchronously.
  ///
  /// The awaited result is `bool`: `true` if the value was enqueued, `false` if
  /// the queue was closed and the value was not enqueued.
  ///
  /// If a consumer is currently suspended waiting for a value, it will be
  /// resumed once the the value is enqueued.
  ///
  /// LIFETIME REQUIREMENT: the returned awaitable holds the arguments by
  /// reference (T& for lvalues, T&& for rvalues / temporaries). If you pass a
  /// temporary into this, you must `co_await` it immediately, so the lifetime
  /// of the argument can be extended to the end of the full-expression.
  /// ```
  /// // Safe: the temporary T's lifetime is extended to the end of the
  /// // full-expression.
  /// co_await q.push(T{...});
  ///
  /// // Unsafe: `a` holds a dangling reference to the temporary T
  /// auto a = q.push(T{...});
  /// co_await std::move(a);
  ///
  /// // Safe: passing a reference to a named variable
  /// auto v = T{...};
  /// auto a = q.push(std::move(v));
  /// co_await std::move(a);
  /// ```
  template <typename... Args>
  [[nodiscard(
    "You must co_await push(). The value will not be enqueued until co_await."
  )]]
  aw_push<Args...> push(Args&&... ConstructArgs) noexcept {
    return aw_push<Args...>(*this, static_cast<Args&&>(ConstructArgs)...);
  }

private:
  // Performs the common close work and returns the waiting consumer (if any)
  // that needs to be woken. Returns nullptr if the queue was already closed,
  // or if no consumer was waiting at the cutoff slot at the cutoff offset.
  consumer_base* close_get_waiting_consumer() noexcept {
    bool expected = false;
    if (!closed.compare_exchange_strong(
          expected, true, std::memory_order_release, std::memory_order_acquire
        )) {
      // Already closed.
      return nullptr;
    }

    // We are the unique closer. The release store of `closed` above is
    // sequenced-before the following seq_cst fetch_add; any producer whose
    // own seq_cst fetch_add on write_offset is mod-order after ours will
    // synchronize-with us via the RMW chain and see `closed == true` on
    // its subsequent acquire load.
    size_t cutoff = write_offset.fetch_add(1, std::memory_order_seq_cst);
    write_closed_at.store(cutoff, std::memory_order_release);
    closed_ready.store(true, std::memory_order_release);

    if constexpr (!ConsumerCanSuspend) {
      return nullptr;
    }

    // Mark the cutoff slot closed, or wake a consumer already armed at the
    // cutoff. Earlier-round consumers/producers at this physical slot are left
    // alone; they will publish/drain normally, and the consumer will observe
    // the closed cutoff on a later pull.
    element* elem = &values[cutoff % capacity];
    uintptr_t cur = elem->flags.load(std::memory_order_acquire);
    while (true) {
      if ((cur & (DATA_BIT | PROD_BIT)) != 0) {
        return nullptr;
      }
      if ((cur & CONS_BIT) != 0) {
        size_t wait_idx =
          elem->waiting_consumer_idx.load(std::memory_order_relaxed);
        if (circular_less_than(wait_idx, cutoff)) {
          return nullptr;
        }
        uintptr_t newv = (cur & ~CONS_BIT) | CLOSE_SENTINEL;
        if (elem->flags.compare_exchange_weak(
              cur, newv, std::memory_order_acq_rel, std::memory_order_acquire
            )) {
          consumer_base* cons =
            elem->waiting_consumer.exchange(nullptr, std::memory_order_acq_rel);
          if (cons != nullptr) {
            cons->elem = nullptr; // signal CLOSED scope
          }
          return cons;
        }
      } else {
        if (elem->flags.compare_exchange_weak(
              cur, CLOSE_SENTINEL, std::memory_order_acq_rel,
              std::memory_order_acquire
            )) {
          return nullptr;
        }
      }
    }
  }

public:
  /// All future calls to `co_await push()` will immediately return
  /// false. Calls to `pull()` and `try_pull()` will continue to read data until
  /// all messages have been consumed, at which point all subsequent calls will
  /// immediately return an empty scope. If the queue was already empty, any
  /// waiting consumers will be awoken immediately and return an empty scope.
  ///
  /// Producers that were already suspended in `push()` waiting for a slot to be
  /// freed will be allowed to complete their production normally.
  ///
  /// `close()` is idempotent and safe to call from any thread.
  void close() noexcept {
    consumer_base* cons = close_get_waiting_consumer();
    if (cons != nullptr) {
      tmc::detail::post_checked(
        cons->continuation_executor, std::move(cons->continuation), cons->prio
      );
    }
  }

  /// Closes the queue and resumes any waiting consumer inline on the caller's
  /// thread instead of posting its continuation to its continuation executor.
  /// This should only be used when the caller knows that the waiting consumer
  /// may safely run on the caller's thread.
  ///
  /// Behaves like `close()` in all other respects. `close_resume_inline()` is
  /// idempotent and safe to call from any thread.
  void close_resume_inline() noexcept {
    consumer_base* cons = close_get_waiting_consumer();
    if (cons != nullptr) {
      cons->continuation.resume();
    }
  }

  /// Returns true if the queue appears to be empty.
  /// This is an unsynchronized read (like `try_pull()`), so it is only a hint.
  /// Only safe to call from the single consumer.
  bool empty() {
    element* elem = &values[read_offset % capacity];
    return (elem->flags.load(std::memory_order_acquire) & DATA_BIT) == 0;
  }

  /// Await to dequeue. Returns a `pull_zc_scope` which provides a scoped
  /// zero-copy reference to a value in the queue storage. When the scope is
  /// destroyed, the referenced value will be destroyed and the queue slot freed
  /// for reuse. Only safe to call from the single consumer.
  ///
  /// The returned scope's `has_value()` / `operator bool()` returns true if a
  /// value was dequeued, or false if the queue was closed and drained.
  ///
  /// This scope must be released before the next call to `try_pull()` or
  /// `pull()`. It must also be released before the queue is destroyed.
  ///
  /// May suspend until a value is available, or until `close()` is called.
  [[nodiscard(
    "You must co_await pull(). To poll from a non-coroutine function, use "
    "try_pull()."
  )]] aw_pull
  pull() noexcept
    requires(ConsumerCanSuspend)
  {
    return aw_pull(*this);
  }

  /// Attempts to immediately dequeue, returning a `try_pull_zc_scope`
  /// which provides a scoped zero-copy reference to a value in the queue
  /// storage. When the scope is destroyed, the referenced value will be
  /// destroyed and the queue slot freed for reuse. Only safe to call from
  /// the single consumer.
  ///
  /// The returned scope's `status()` returns:
  ///   - qu_mpsc_bounded_err::OK     - a value was dequeued
  ///   - qu_mpsc_bounded_err::EMPTY  - no value is currently available
  ///   - qu_mpsc_bounded_err::CLOSED - the queue has been closed and drained
  ///
  /// The returned scope's `has_value()` / `operator bool()` returns true if a
  /// value was dequeued, or false if the queue was empty or closed.
  ///
  /// This scope must be released before the next call to `try_pull()` or
  /// `pull()`. It must also be released before the queue is destroyed.
  try_pull_zc_scope try_pull() {
    while (true) {
      size_t idx = read_offset;
      element* elem = &values[idx % capacity];
      uintptr_t f = elem->flags.load(std::memory_order_acquire);
      if ((f & DATA_BIT) != 0) {
        return try_pull_zc_scope(this, elem, idx);
      }
      if (closed.load(std::memory_order_acquire)) {
        while (!closed_ready.load(std::memory_order_acquire)) {
          TMC_CPU_PAUSE();
        }
        size_t cutoff = write_closed_at.load(std::memory_order_acquire);
        if (!circular_less_than(idx, cutoff)) {
          return try_pull_zc_scope(tmc::qu_mpsc_bounded_err::CLOSED);
        }
        if (has_close_sentinel(f)) {
          read_offset = idx + 1;
          continue;
        }
      }
      return try_pull_zc_scope(tmc::qu_mpsc_bounded_err::EMPTY);
    }
  }

  /// Destroys the queue and any contained values that have not yet been
  /// consumed.
  ///
  /// Before destroying this, you must ensure:
  /// - No producer is currently calling or suspended in push().
  /// - No consumer is calling or suspended in pull() / try_pull().
  /// - No pull_zc_scope / try_pull_zc_scope from this queue is alive.
  /// - No other thread is calling any other member function.
  ///
  /// The recommended teardown sequence is:
  /// 1. Stop submitting new push() calls.
  /// 2. close() the queue.
  /// 3. Drain via pull() / try_pull() until CLOSED. This naturally wakes
  ///    every pre-close parked producer.
  /// 4. Ensure no further queue method calls will occur (e.g. by joining all
  ///    producer and consumer coroutines).
  /// 5. Destroy the queue.
  ~qu_mpsc_bounded() {
    close();
    size_t end = write_closed_at.load(std::memory_order_relaxed);
    size_t idx = read_offset;
    while (circular_less_than(idx, end)) {
      element* elem = &values[idx % capacity];
      uintptr_t f = elem->flags.load(std::memory_order_relaxed);
      if ((f & DATA_BIT) != 0) {
        elem->data.destroy();
      }
      ++idx;
    }
    delete[] values;
  }

  qu_mpsc_bounded(const qu_mpsc_bounded&) = delete;
  qu_mpsc_bounded& operator=(const qu_mpsc_bounded&) = delete;
  qu_mpsc_bounded(qu_mpsc_bounded&&) = delete;
  qu_mpsc_bounded& operator=(qu_mpsc_bounded&&) = delete;

  /// Returns a `bool` when awaited: `true` on successful enqueue, `false`
  /// if the queue was closed.
  template <typename... Args>
  class aw_push final : private tmc::detail::AwaitTagNoGroupCoAwait {
    friend qu_mpsc_bounded<T, Config>;

    qu_mpsc_bounded& queue;
    // Construction args are stored as references (T& for lvalue inputs, T&&
    // for rvalue / temporary inputs) so we never copy or move them into the
    // awaitable. T itself need not be movable; it will be emplace-constructed
    // directly into the queue slot from these forwarded args.
    //
    // The caller must co_await this awaitable in the same full-expression
    // (see push()'s docs) so any referenced temporary's lifetime is extended
    // across both the suspension and the resumption.
    std::tuple<Args&&...> args;

    aw_push(qu_mpsc_bounded& Queue, Args&&... ConstructArgs) noexcept
        : queue(Queue), args(static_cast<Args&&>(ConstructArgs)...) {}

    struct aw_push_impl final {
      producer_base base;
      qu_mpsc_bounded& queue;
      std::tuple<Args&&...>& args;
      element* elem;
      bool closed_before_enqueue;

      aw_push_impl(aw_push& Parent) noexcept
          : base{nullptr, tmc::detail::this_thread::executor(), nullptr,
                 tmc::detail::this_thread::this_task().prio},
            queue(Parent.queue), args(Parent.args), elem(nullptr),
            closed_before_enqueue(false) {}

      bool await_ready() noexcept {
        elem = queue.get_write_ticket();
        if (elem == nullptr) [[unlikely]] {
          closed_before_enqueue = true;
          return true;
        }
        // Fast path: claim an empty slot.
        producer_wait_result r = queue.producer_try_claim(elem);
        if (r == producer_wait_result::CLOSED) [[unlikely]] {
          closed_before_enqueue = true;
          return true;
        }
        return r == producer_wait_result::CLAIMED;
      }

      bool await_suspend(std::coroutine_handle<> Outer) noexcept {
        base.continuation = Outer;
        while (true) {
          producer_wait_result r = queue.producer_park(elem, &base);
          if (r == producer_wait_result::PARKED) {
            // Parked; will be woken by the consumer once the slot drains
            // and is handed off to us with PROD_BIT already set.
            return true;
          }
          if (r == producer_wait_result::CLOSED) [[unlikely]] {
            closed_before_enqueue = true;
            return false;
          }
          // Slot raced empty between park-attempt's load and CAS. Try to
          // claim it directly instead.
          r = queue.producer_try_claim(elem);
          if (r == producer_wait_result::CLAIMED) {
            return false;
          }
          if (r == producer_wait_result::CLOSED) [[unlikely]] {
            closed_before_enqueue = true;
            return false;
          }
          // Lost the claim race to a fresh producer; loop and try to park
          // again.
        }
      }

      bool await_resume() noexcept {
        if (closed_before_enqueue) [[unlikely]] {
          return false;
        }
        // We hold PROD_BIT on `elem` - either we claimed it ourselves, or
        // the consumer popped us from the chain and set PROD_BIT on our
        // behalf. Either way, emplace and publish. Using std::move on the tuple
        // causes std::apply to invoke get<>() on an rvalue tuple, which
        // preserves T&& elements as rvalues; T& elements remain lvalues.
        // std::forward<decltype(a)> then forwards each arg with its original
        // value category.
        std::apply(
          [this](auto&&... a) {
            elem->data.emplace(static_cast<decltype(a)&&>(a)...);
          },
          std::move(args)
        );
        consumer_base* cons = queue.producer_publish(elem);
        if (cons != nullptr) {
          tmc::detail::post_checked(
            cons->continuation_executor, std::move(cons->continuation),
            cons->prio
          );
        }
        return true;
      }
    };

  public:
    aw_push_impl operator co_await() && noexcept { return aw_push_impl(*this); }
  };

  /// Returns a `pull_zc_scope` when awaited.
  class aw_pull final : private tmc::detail::AwaitTagNoGroupCoAwait {
    friend qu_mpsc_bounded<T, Config>;

    qu_mpsc_bounded& queue;

    aw_pull(qu_mpsc_bounded& Queue) noexcept : queue(Queue) {}

    struct aw_pull_impl final {
      consumer_base base;
      qu_mpsc_bounded& queue;

      aw_pull_impl(aw_pull& Parent) noexcept
          : base{tmc::detail::this_thread::executor(), nullptr,
                 tmc::detail::this_thread::this_task().prio, nullptr, 0},
            queue{Parent.queue} {}

      bool await_ready() noexcept {
        while (true) {
          base.idx = queue.read_offset;
          base.elem = &queue.values[base.idx % queue.capacity];
          uintptr_t f = base.elem->flags.load(std::memory_order_acquire);
          if ((f & DATA_BIT) != 0) {
            return true;
          }
          if (queue.closed.load(std::memory_order_acquire)) {
            while (!queue.closed_ready.load(std::memory_order_acquire)) {
              TMC_CPU_PAUSE();
            }
            size_t cutoff =
              queue.write_closed_at.load(std::memory_order_acquire);
            if (!circular_less_than(base.idx, cutoff)) {
              base.elem = nullptr; // signal CLOSED scope
              return true;
            }
            if (has_close_sentinel(f)) {
              queue.read_offset = base.idx + 1;
              continue;
            }
          }
          return false;
        }
      }

      bool await_suspend(std::coroutine_handle<> Outer) noexcept {
        consumer_base* const self = &base;
        qu_mpsc_bounded* const q = &queue;
        self->continuation = Outer;

        while (true) {
          self->idx = q->read_offset;
          self->elem = &q->values[self->idx % q->capacity];
          element* elem = self->elem;
          size_t idx = self->idx;

          // Installing the consumer requires setting 3 values in order:
          // waiting_consumer_idx, waiting_consumer, and flags | CONS_BIT.
          // Producers and close() only consume waiting_consumer after they
          // observe/clear CONS_BIT, so a successful CONS_BIT CAS is the final
          // await_suspend operation. This prevents use-after-resume races
          // because this cannot be resumed until after the final CAS.

          // Install the waiting_consumer first
          elem->waiting_consumer_idx.store(idx, std::memory_order_relaxed);
          elem->waiting_consumer.store(self, std::memory_order_release);

          // Double-check
          uintptr_t f = elem->flags.load(std::memory_order_acquire);
          while (true) {
            if ((f & DATA_BIT) != 0) {
              consumer_base* expected = self;
              if (elem->waiting_consumer.compare_exchange_strong(
                    expected, nullptr, std::memory_order_acq_rel,
                    std::memory_order_acquire
                  )) {
                return false;
              }
              return true;
            }
            if (has_close_sentinel(f)) {
              consumer_base* expected = self;
              if (elem->waiting_consumer.compare_exchange_strong(
                    expected, nullptr, std::memory_order_acq_rel,
                    std::memory_order_acquire
                  )) {
                while (!q->closed_ready.load(std::memory_order_acquire)) {
                  TMC_CPU_PAUSE();
                }
                size_t cutoff =
                  q->write_closed_at.load(std::memory_order_acquire);
                if (!circular_less_than(idx, cutoff)) {
                  self->elem = nullptr; // signal CLOSED
                  return false;
                }
                q->read_offset = idx + 1;
                break;
              }
              return true;
            }
            assert((f & CONS_BIT) == 0);
            if (elem->flags.compare_exchange_weak(
                  f, f | CONS_BIT, std::memory_order_acq_rel,
                  std::memory_order_acquire
                )) {
              return true;
            }
          }
        }
      }

      TMC_AWAIT_RESUME pull_zc_scope await_resume() noexcept {
        // If closed and drained, base.elem was set to nullptr by either our
        // own check or by close_get_waiting_consumer.
        return pull_zc_scope(&queue, base.elem, base.idx);
      }
    };

  public:
    aw_pull_impl operator co_await() && noexcept { return aw_pull_impl(*this); }
  };
};

} // namespace tmc
