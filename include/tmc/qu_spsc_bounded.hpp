// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

// Provides tmc::qu_spsc_bounded, an async SPSC bounded linearizable queue.
// All enqueue and dequeue operations are zero-copy.
// A single allocation is created in the constructor. Subsequent operations are
// allocation-free.

// Uses a similar fetch-add slot acquisition scheme to tmc::channel, with these
// changes:
// - producer suspends when queue is full
// - single producer can publish offset after writing data instead of before
// - single consumer read offset does not need to be atomic
// - close() may only be called by the single producer

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
struct qu_spsc_bounded_default_config {
  /// If true, enables the suspending `pull()` operation. This costs the
  /// producer an additional locked operation to check for a waiting consumer.
  static inline constexpr bool ConsumerCanSuspend = true;

  /// At level 0, queue elements will be padded up to the next increment of 64
  /// bytes. This reduces false sharing between neighboring elements.
  /// At level 1, no padding will be applied.
  /// The SPSC queue is packed by default to improve cache coherency for the
  /// single producer.
  static inline constexpr size_t PackingLevel = 1;
};

/// Status code returned by qu_spsc_bounded.try_pull().status()
enum class qu_spsc_bounded_err { OK, EMPTY, CLOSED };

template <typename T, typename Config = tmc::qu_spsc_bounded_default_config>
class qu_spsc_bounded {
  static inline constexpr bool ConsumerCanSuspend = Config::ConsumerCanSuspend;

  // Flag bits in element::flags. Upper bits encode the consumer_base* (low 2
  // bits guaranteed 0 by alignment).
  static inline constexpr uintptr_t DATA_BIT = TMC_ONE_BIT;
  static inline constexpr uintptr_t CLOSED_BIT = TMC_ONE_BIT << 1;

  struct element;

  struct consumer_base {
    tmc::ex_any* continuation_executor;
    std::coroutine_handle<> continuation;
    size_t prio;
    element* elem;
  };

  struct producer_base {
    tmc::ex_any* continuation_executor;
    std::coroutine_handle<> continuation;
    size_t prio;
  };

  // We assert that nullptr == 0 (in compat.hpp), and that the lower 2 bits of a
  // waiter pointer are 0, so that values 1-3 can be used to represent flags.
  static_assert(alignof(consumer_base) >= 4);
  static_assert(alignof(producer_base) >= 4);
  static_assert(Config::PackingLevel < 2);

  struct element {
    std::atomic<void*> flags;
    tmc::detail::qu_storage<T> data;

    static constexpr size_t UNPADLEN =
      sizeof(std::atomic<void*>) + sizeof(tmc::detail::qu_storage<T>);
    static constexpr size_t WANTLEN = (UNPADLEN + TMC_CACHE_LINE_SIZE - 1) &
                                      static_cast<size_t>(
                                        0 - TMC_CACHE_LINE_SIZE
                                      ); // round up to TMC_CACHE_LINE_SIZE
    static constexpr size_t PADLEN =
      UNPADLEN < WANTLEN ? (WANTLEN - UNPADLEN) : 999;

    struct empty {};
    using Padding = std::conditional_t<
      Config::PackingLevel == 0 && PADLEN != 999, char[PADLEN], empty>;
    TMC_NO_UNIQUE_ADDRESS Padding pad;

    // Attempts to install Cons as a waiting consumer via CAS(nullptr → Cons).
    // Returns the previous flags value:
    //   - 0 (nullptr) means Cons is now installed and the consumer should
    //     suspend;
    //   - DATA_BIT means a producer already published data here;
    //   - CLOSED_BIT means close() already published a CLOSED sentinel here;
    //   - a value >= 4 is a producer_base* (queue is full and a producer is
    //     suspended on this slot waiting for the consumer to drain it; the
    //     data from the producer's previous round IS present in this slot).
    // On any non-zero return value Cons was NOT installed; the slot remains
    // unmodified.
    uintptr_t try_wait(consumer_base* Cons) noexcept {
      void* expected = nullptr;
      flags.compare_exchange_strong(
        expected, static_cast<void*>(Cons), std::memory_order_acq_rel,
        std::memory_order_acquire
      );
      return reinterpret_cast<uintptr_t>(expected);
    }

    // Sets the data ready flag,
    // or returns a consumer pointer if that consumer was already waiting.
    consumer_base* set_data_ready_or_get_waiting_consumer() noexcept
      requires(ConsumerCanSuspend)
    {
      void* prev = flags.exchange(
        reinterpret_cast<void*>(DATA_BIT), std::memory_order_acq_rel
      );
      return static_cast<consumer_base*>(prev);
    }

    void set_data_ready() noexcept
      requires(!ConsumerCanSuspend)
    {
      flags.store(reinterpret_cast<void*>(DATA_BIT), std::memory_order_release);
    }

    // Publishes a CLOSED sentinel. Used only by close() to mark the cutoff
    // slot.
    //
    // The published value depends on the prior value:
    //   - 0 (empty)         -> CLOSED_BIT
    //   - DATA_BIT          -> DATA_BIT|CLOSED_BIT (data is preserved; the
    //                          consumer will still read it, and finish_read
    //                          will re-publish CLOSED_BIT for the wraparound)
    //   - CLOSED_BIT        -> CLOSED_BIT (idempotent; should never happen)
    //   - consumer_base*    -> CLOSED_BIT (the pointer is consumed by the
    //                          return value so the caller can wake the
    //                          suspended consumer; subsequent attempts to pull
    //                          from this slot will see it as closed)
    //
    // Returns the previously-installed consumer_base* if a consumer was
    // waiting on this slot at close time (so the caller can wake it), or
    // nullptr otherwise.
    //
    // Note: producer_base* cannot appear here. close() runs on the single
    // producer, which cannot simultaneously be suspended on a slot.
    consumer_base* set_closed_get_waiting_consumer() noexcept {
      void* expected = flags.load(std::memory_order_relaxed);
      while (true) {
        uintptr_t cur = reinterpret_cast<uintptr_t>(expected);
        uintptr_t desired =
          (cur == DATA_BIT) ? (DATA_BIT | CLOSED_BIT) : CLOSED_BIT;
        if (flags.compare_exchange_weak(
              expected, reinterpret_cast<void*>(desired),
              std::memory_order_acq_rel, std::memory_order_relaxed
            )) {
          if (cur >= 4) {
            return static_cast<consumer_base*>(expected);
          }
          return nullptr;
        }
      }
    }

    bool is_data_waiting() noexcept {
      uintptr_t v =
        reinterpret_cast<uintptr_t>(flags.load(std::memory_order_acquire));
      // Data is present when DATA_BIT is set (possibly together with
      // CLOSED_BIT if close() ran while the slot still held data), or when
      // flags is a producer_base* (>= 4) meaning a producer is suspended
      // because the queue was full but the previous round's data is still
      // here. We assume that it is a producer pointer and not a consumer
      // pointer because of the documented invariants of the 2 call sites that
      // this is used from: empty() can only be called by the consumer, so it
      // cannot be waiting, and the destructor doesn't wake producers (it would
      // be unsafe for the destructor to race with a producer).
      return (v & DATA_BIT) != 0 || v >= 4;
    }

    bool is_closed_sentinel() noexcept {
      void* f = flags.load(std::memory_order_acquire);
      return CLOSED_BIT == reinterpret_cast<uintptr_t>(f);
    }

    // Returns the raw flags value: DATA_BIT, CLOSED_BIT, or 0 (meaning empty).
    uintptr_t poll() noexcept {
      return reinterpret_cast<uintptr_t>(flags.load(std::memory_order_acquire));
    }

    void reset() noexcept { flags.store(nullptr, std::memory_order_relaxed); }
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

  // Read and written only by close(). Producer reads with NDEBUG only.
  std::atomic<bool> closed;
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
  /// `qu_spsc_bounded_err::OK` if a value is held, `EMPTY` if no value was
  /// available, or `CLOSED` if the queue has been closed and drained.
  class try_pull_zc_scope {
    friend qu_spsc_bounded;
    qu_spsc_bounded* queue;
    element* elem;
    size_t idx;
    tmc::qu_spsc_bounded_err err;

    try_pull_zc_scope(
      qu_spsc_bounded* Queue, element* Elem, size_t Idx
    ) noexcept
        : queue{Queue}, elem{Elem}, idx{Idx},
          err{tmc::qu_spsc_bounded_err::OK} {}

    explicit try_pull_zc_scope(tmc::qu_spsc_bounded_err Err) noexcept
        : queue{nullptr}, elem{nullptr}, idx{0}, err{Err} {}

  public:
    /// Constructs an empty scope (status EMPTY). Evaluates to false when
    /// converted to bool.
    try_pull_zc_scope() noexcept
        : queue{nullptr}, elem{nullptr}, idx{0},
          err{tmc::qu_spsc_bounded_err::EMPTY} {}

    try_pull_zc_scope(const try_pull_zc_scope&) = delete;
    try_pull_zc_scope& operator=(const try_pull_zc_scope&) = delete;

    try_pull_zc_scope(try_pull_zc_scope&& Other) noexcept
        : queue{Other.queue}, elem{Other.elem}, idx{Other.idx}, err{Other.err} {
      Other.elem = nullptr;
      Other.err = tmc::qu_spsc_bounded_err::EMPTY;
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
        Other.err = tmc::qu_spsc_bounded_err::EMPTY;
      }
      return *this;
    }

    /// Returns true if this scope holds a value from the queue (status == OK).
    explicit operator bool() const noexcept { return elem != nullptr; }

    /// Returns true if this scope holds a value from the queue (status == OK).
    bool has_value() const noexcept { return elem != nullptr; }

    /// Returns the status of this pull: OK, EMPTY, or CLOSED.
    tmc::qu_spsc_bounded_err status() const noexcept { return err; }

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
    friend qu_spsc_bounded;
    qu_spsc_bounded* queue;
    element* elem;
    size_t idx;

    pull_zc_scope(qu_spsc_bounded* Queue, element* Elem, size_t Idx) noexcept
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

  /// Constructs a qu_spsc_bounded with the given capacity.
  explicit qu_spsc_bounded(size_t Capacity) noexcept
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
    read_offset = 0;
    for (size_t i = 0; i < capacity; ++i) {
      values[i].reset();
    }
    tmc::detail::memory_barrier();
  }

private:
  static inline bool circular_less_than(size_t a, size_t b) noexcept {
    return a - b > (TMC_ONE_BIT << (TMC_PLATFORM_BITS - 1));
  }

  // Idx will be initialized by this function
  element* get_write_ticket(size_t& Idx) noexcept {
    // For SPSC, the element is written before writing updated write_offset
    Idx = write_offset.load(std::memory_order_relaxed);
    return &values[Idx % capacity];
  }

  // Idx will be initialized by this function
  element* get_read_ticket(size_t& Idx) noexcept {
    // For SPSC, the element is read before writing updated read_offset
    Idx = read_offset;
    return &values[Idx % capacity];
  }

  void finish_read(element* Elem, size_t Idx) noexcept {
    Elem->data.destroy();

    // Exchange to nullptr to free the slot.
    void* prev = Elem->flags.exchange(nullptr, std::memory_order_acq_rel);
    read_offset = Idx + 1;

    uintptr_t pv = reinterpret_cast<uintptr_t>(prev);
    if (pv >= 4) {
      // The queue was full, and a producer is waiting. Wake it up.
      auto* prod = static_cast<producer_base*>(prev);
      tmc::detail::post_checked(
        prod->continuation_executor, std::move(prod->continuation), prod->prio
      );
    } else if ((pv & CLOSED_BIT) != 0) {
      // The slot we just drained was the close cutoff (queue was full at
      // close time, so close() set DATA_BIT|CLOSED_BIT). Re-publish
      // CLOSED_BIT alone so that when the consumer wraps around to this
      // same physical slot, its next pull/try_pull observes the closed
      // sentinel directly instead of suspending forever. No producer can be
      // waiting or race us here because close() forbids any further pushes.
      Elem->flags.store(
        reinterpret_cast<void*>(CLOSED_BIT), std::memory_order_release
      );
    }
  }

  template <typename... Args>
  consumer_base*
  write_element(element* Elem, Args&&... ConstructArgs) noexcept {
    Elem->data.emplace(static_cast<Args&&>(ConstructArgs)...);
    if constexpr (ConsumerCanSuspend) {
      return Elem->set_data_ready_or_get_waiting_consumer();
    } else {
      Elem->set_data_ready();
      return nullptr;
    }
  }

public:
  template <typename... Args> class aw_push;
  template <typename It> class aw_push_bulk;

  /// Enqueues a new value in the queue by in-place construction, forwarding
  /// `ConstructArgs` to T's constructor. Only safe to call from the single
  /// producer.
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
  /// You must not call this after calling close().
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
    // close() must only be called from the single producer, so push()
    // and close() are sequenced on the same task. Pushing after close() is
    // a programming error.
    assert(!closed.load(std::memory_order_relaxed));
    return aw_push<Args...>(*this, static_cast<Args&&>(ConstructArgs)...);
  }

  /// Non-suspending counterpart to `push()`. Constructs a new value in the
  /// queue, forwarding `ConstructArgs` to T's constructor. Only safe to call
  /// from the single producer.
  ///
  /// Returns `true` if the value was successfully pushed, or `false` if the
  /// queue was full (in which case no value is enqueued, ConstructArgs are
  /// not used, and the queue is not modified).
  ///
  /// If a consumer is currently suspended waiting for a value, it will be
  /// resumed if a value was enqueued.
  ///
  /// You must not call this after calling close().
  template <typename... Args>
  [[nodiscard(
    "try_push() returns false if the queue was full; check the "
    "return value to know whether the value was enqueued."
  )]] bool
  try_push(Args&&... ConstructArgs) noexcept {
    // close() must only be called from the single producer, so try_push()
    // and close() are sequenced on the same task. Pushing after close() is
    // a programming error.
    assert(!closed.load(std::memory_order_relaxed));
    size_t idx;
    element* elem = get_write_ticket(idx);
    // If the slot still holds DATA_BIT, the consumer has not yet drained the
    // previous value at this slot - the queue is full. Any other value
    // (nullptr meaning free, or a consumer_base* meaning a consumer is
    // waiting) means we can write immediately. A producer_base* cannot appear
    // here since this is the single producer.
    uintptr_t f = elem->poll();
    if (f == DATA_BIT) {
      return false;
    }
    consumer_base* cons =
      write_element(elem, static_cast<Args&&>(ConstructArgs)...);
    write_offset.store(idx + 1, std::memory_order_release);
    if (cons != nullptr) {
      tmc::detail::post_checked(
        cons->continuation_executor, std::move(cons->continuation), cons->prio
      );
    }
    return true;
  }

  /// Moves `Count` values from the iterator `Items` into the queue. Only safe
  /// to call from the single producer. `Count` must be no greater than
  /// the queue capacity passed to the constructor; if more elements need to be
  /// submitted, call this function in a loop.
  ///
  /// Returns an awaitable that must be co_awaited. If the queue does not have
  /// `Count` free slots, the producer suspends until enough slots have been
  /// freed by the consumer to fit all `Count` elements at once. Otherwise it
  /// completes synchronously.
  ///
  /// If a consumer is currently suspended waiting for a value, it will be
  /// resumed once the the values are enqueued.
  ///
  /// You must not call this after calling close().
  template <typename It>
  [[nodiscard(
    "You must co_await push_bulk(). The values will not be enqueued "
    "until co_await."
  )]]
  aw_push_bulk<std::remove_cvref_t<It>>
  push_bulk(It&& Items, size_t Count) noexcept {
    static_assert(
      std::is_nothrow_move_constructible_v<T>,
      "push_bulk moves values from the iterator into the queue; T must be "
      "nothrow move constructible"
    );
    // close() must only be called from the single producer, so
    // push_bulk() and close() are sequenced on the same task. Pushing after
    // close() is a programming error.
    assert(!closed.load(std::memory_order_relaxed));
    assert(Count <= capacity);
    return aw_push_bulk<std::remove_cvref_t<It>>(
      *this, static_cast<It&&>(Items), Count
    );
  }

  /// Calculates the number of elements via `size_t Count = End - Begin;`
  /// and moves them from the iterator `Begin` into the queue. Only safe to
  /// call from the single producer. The number of elements must be no
  /// greater than the queue capacity passed to the constructor; if more
  /// elements need to be submitted, call this function in a loop.
  ///
  /// Returns an awaitable that must be co_awaited. See the (Items, Count)
  /// overload for suspension behavior.
  ///
  /// If a consumer is currently suspended waiting for a value, it will be
  /// resumed once the the values are enqueued.
  ///
  /// You must not call this after calling close().
  template <typename It>
  [[nodiscard(
    "You must co_await push_bulk(). The values will not be enqueued "
    "until co_await."
  )]]
  aw_push_bulk<std::remove_cvref_t<It>>
  push_bulk(It&& Begin, It&& End) noexcept {
    static_assert(
      std::is_nothrow_move_constructible_v<T>,
      "push_bulk moves values from the iterator into the queue; T must be "
      "nothrow move constructible"
    );
    return push_bulk(
      static_cast<It&&>(Begin), static_cast<size_t>(End - Begin)
    );
  }

  /// Calculates the number of elements via
  /// `size_t Count = Range.end() - Range.begin();` and moves them from the
  /// beginning of the range into the queue. Only safe to call from the single
  /// producer. The number of elements must be no greater than
  /// the queue capacity passed to the constructor; if more elements need to be
  /// submitted, call this function in a loop.
  ///
  /// Returns an awaitable that must be co_awaited. See the (Items, Count)
  /// overload for suspension behavior.
  ///
  /// If a consumer is currently suspended waiting for a value, it will be
  /// resumed once the the values are enqueued.
  ///
  /// You must not call this after calling close().
  template <typename Range>
  [[nodiscard(
    "You must co_await push_bulk(). The values will not be enqueued "
    "until co_await."
  )]]
  auto push_bulk(Range&& R) noexcept {
    static_assert(
      std::is_nothrow_move_constructible_v<T>,
      "push_bulk moves values from the iterator into the queue; T must be "
      "nothrow move constructible"
    );
    auto begin = static_cast<Range&&>(R).begin();
    auto end = static_cast<Range&&>(R).end();
    return push_bulk(std::move(begin), static_cast<size_t>(end - begin));
  }

private:
  // Performs the common close work and returns the waiting consumer (if any)
  // that needs to be woken. Returns nullptr if the queue was already closed,
  // or if no consumer was waiting at the cutoff slot.
  consumer_base* close_get_waiting_consumer() noexcept {
    bool expected = false;
    if (!closed.compare_exchange_strong(
          expected, true, std::memory_order_acq_rel, std::memory_order_acquire
        )) {
      // Already closed.
      return nullptr;
    }

    // Because close() is only called from the single producer, there is
    // no concurrent producer that could reserve a slot past the close cutoff.
    // The next slot the producer would have used is write_offset (writes are
    // published by storing write_offset *after* the data); this is the only
    // slot the consumer could still be waiting on.
    size_t woff = write_offset.load(std::memory_order_relaxed);
    write_closed_at.store(woff, std::memory_order_release);

    // Set CLOSED_BIT into the cutoff slot's flags. This races with the
    // consumer's try_wait() on the same element; the two RMWs linearize.
    element* elem = &values[woff % capacity];
    consumer_base* cons = elem->set_closed_get_waiting_consumer();
    if (cons != nullptr) {
      // Setting elem to nullptr notifies consumer's await_resume() -> user code
      // that the queue is closed.
      cons->elem = nullptr;
    }
    return cons;
  }

public:
  /// Closes the queue. May only be called from the single producer.
  /// After `close()` returns, the producer must not call `push()`,
  /// `try_push()`, or `push_bulk()` again. Calls to `pull()` and `try_pull()`
  /// will continue to read data until all messages have been consumed, at which
  /// point all subsequent calls will immediately return an empty scope. If the
  /// queue was already empty, any waiting consumers will be awoken immediately
  /// and return an empty scope.
  ///
  /// `close()` is idempotent.
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
  /// idempotent. May only be called from the single producer.
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
    size_t Idx = read_offset;
    element* elem = &values[Idx % capacity];

    bool isEmpty = !elem->is_data_waiting();
    return isEmpty;
  }

  /// Returns `void` when awaited.
  template <typename... Args>
  class aw_push final : private tmc::detail::AwaitTagNoGroupCoAwait {
    friend qu_spsc_bounded<T, Config>;

    qu_spsc_bounded& queue;
    // Construction args are stored as references (T& for lvalue inputs, T&&
    // for rvalue / temporary inputs) so that we never copy or move them into
    // the awaitable. T itself need not be movable; it will be emplace-
    // constructed directly into the queue slot from these forwarded args.
    //
    // The caller must co_await this awaitable in the same full-expression
    // (see push()'s docs) so that any referenced temporary's lifetime is
    // extended across both the suspension and the resumption.
    std::tuple<Args&&...> args;

    aw_push(qu_spsc_bounded& Queue, Args&&... ConstructArgs) noexcept
        : queue(Queue), args(static_cast<Args&&>(ConstructArgs)...) {}

    struct aw_push_impl final {
      producer_base base;
      qu_spsc_bounded& queue;
      std::tuple<Args&&...>& args;
      element* elem;
      size_t idx;

      aw_push_impl(aw_push& Parent) noexcept
          : base{tmc::detail::this_thread::executor(), nullptr,
                 tmc::detail::this_thread::this_task().prio},
            queue(Parent.queue), args(Parent.args), elem(nullptr), idx(0) {}

      bool await_ready() noexcept {
        elem = queue.get_write_ticket(idx);
        // If the slot is anything other than DATA_BIT (i.e. nullptr meaning
        // free, or a consumer_base* meaning a consumer is waiting), the
        // producer may write immediately. DATA_BIT means the consumer has
        // not yet consumed the previous value at this slot - queue is full.
        uintptr_t f = elem->poll();
        return f != DATA_BIT;
      }

      bool await_suspend(std::coroutine_handle<> Outer) noexcept {
        base.continuation = Outer;
        // Attempt to install ourselves as a waiting producer. We expect the
        // slot to still hold DATA_BIT (queue still full). If so, the
        // consumer's finish_read will observe our pointer and wake us.
        void* expected = reinterpret_cast<void*>(DATA_BIT);
        if (elem->flags.compare_exchange_strong(
              expected, static_cast<void*>(&base), std::memory_order_acq_rel,
              std::memory_order_acquire
            )) {
          return true;
        }
        // If the CAS fails, the consumer raced ahead and freed the slot. In
        // that case we proceed to write the data synchronously without
        // suspending.
        //
        // Valid states at this point are nullptr (consumer freed the slot), or
        // a consumer_base* (consumer freed the slot, wrapped around, and is
        // now waiting here). It cannot be CLOSED_BIT (close() is sequenced on
        // the producer) nor a producer_base* (there is only one producer).
        assert(
          expected == nullptr ||
          (ConsumerCanSuspend && reinterpret_cast<uintptr_t>(expected) >= 4)
        );
        return false;
      }

      void await_resume() noexcept {
        // The slot is now free (nullptr). Emplace-construct T directly into
        // the slot by forwarding the stored references (zero-copy), then
        // publish. Using std::move on the tuple causes std::apply to invoke
        // get<>() on an rvalue tuple, which preserves T&& elements as
        // rvalues; T& elements remain lvalues. std::forward<decltype(a)>
        // then forwards each arg with its original value category.
        consumer_base* cons = std::apply(
          [this](auto&&... a) {
            return queue.write_element(elem, static_cast<decltype(a)&&>(a)...);
          },
          std::move(args)
        );
        queue.write_offset.store(idx + 1, std::memory_order_release);
        if (cons != nullptr) {
          tmc::detail::post_checked(
            cons->continuation_executor, std::move(cons->continuation),
            cons->prio
          );
        }
      }
    };

  public:
    aw_push_impl operator co_await() && noexcept { return aw_push_impl(*this); }
  };

  /// Returns `void` when awaited.
  template <typename It>
  class aw_push_bulk final : private tmc::detail::AwaitTagNoGroupCoAwait {
    friend qu_spsc_bounded<T, Config>;

    qu_spsc_bounded& queue;
    It items;
    size_t count;

    aw_push_bulk(qu_spsc_bounded& Queue, It Items, size_t Count) noexcept
        : queue(Queue), items(std::move(Items)), count(Count) {}

    struct aw_push_bulk_impl final {
      producer_base base;
      qu_spsc_bounded& queue;
      It& items;
      size_t count;
      size_t startIdx;
      element* lastElem;

      aw_push_bulk_impl(aw_push_bulk& Parent) noexcept
          : base{tmc::detail::this_thread::executor(), nullptr,
                 tmc::detail::this_thread::this_task().prio},
            queue(Parent.queue), items(Parent.items), count(Parent.count),
            startIdx(0), lastElem(nullptr) {}

      bool await_ready() noexcept {
        if (count == 0) [[unlikely]] {
          return true;
        }
        startIdx = queue.write_offset.load(std::memory_order_relaxed);
        // The last slot we need free in order to write all `count` elements.
        // When single consumer frees this slot, all prior slots are also free.
        lastElem = &queue.values[(startIdx + count - 1) % queue.capacity];
        // 'Free' means anything other than DATA_BIT. If count == 1 and the
        // queue is empty with the consumer suspended on the single start / end
        // slot, flags may hold a consumer_base*; that is also 'free' from the
        // producer's perspective and is correctly handled by write_element
        // in await_resume, which will wake the waiting consumer.
        uintptr_t f = lastElem->poll();
        return f != DATA_BIT;
      }

      bool await_suspend(std::coroutine_handle<> Outer) noexcept {
        base.continuation = Outer;
        // Attempt to install ourselves as a waiting producer. We expect the
        // slot to still hold DATA_BIT (queue still full). If so, the
        // consumer's finish_read will observe our pointer and wake us.
        void* expected = reinterpret_cast<void*>(DATA_BIT);
        if (lastElem->flags.compare_exchange_strong(
              expected, static_cast<void*>(&base), std::memory_order_acq_rel,
              std::memory_order_acquire
            )) {
          return true;
        }
        // If the CAS fails, the consumer raced ahead and freed the slot. In
        // that case we proceed to write the data synchronously without
        // suspending.
        //
        // Valid states at this point are nullptr (consumer freed the slot), or
        // a consumer_base* (consumer freed the slot, wrapped around, and is
        // now waiting here). It cannot be CLOSED_BIT (close() is sequenced on
        // the producer) nor a producer_base* (there is only one producer).
        assert(
          expected == nullptr ||
          (ConsumerCanSuspend && reinterpret_cast<uintptr_t>(expected) >= 4)
        );
        return false;
      }

      void await_resume() noexcept {
        if (count == 0) [[unlikely]] {
          return;
        }
        // All `count` slots are now free. The consumer can only be waiting at
        // the first slot, so as an optimization, we publish all the other slots
        // using relaxed stores (instead of exchanges), then do an exchange on
        // the first slot to wake any waiting consumer.

        // Write first slot's data so we can advance the iterator normally
        element* firstElem = &queue.values[startIdx % queue.capacity];
        firstElem->data.emplace(std::move(*items));
        ++items;

        // Publish the rest of the slots' data + flags with relaxed stores
        for (size_t i = 1; i < count; ++i) {
          element* elem = &queue.values[(startIdx + i) % queue.capacity];
          elem->data.emplace(std::move(*items));
          elem->flags.store(
            reinterpret_cast<void*>(DATA_BIT), std::memory_order_relaxed
          );
          ++items;
        }

        // Release the first slot's flags and check for consumer
        consumer_base* cons;
        if constexpr (ConsumerCanSuspend) {
          cons = firstElem->set_data_ready_or_get_waiting_consumer();
        } else {
          firstElem->set_data_ready();
          cons = nullptr;
        }

        queue.write_offset.store(startIdx + count, std::memory_order_release);
        if (cons != nullptr) {
          tmc::detail::post_checked(
            cons->continuation_executor, std::move(cons->continuation),
            cons->prio
          );
        }
      }
    };

  public:
    aw_push_bulk_impl operator co_await() && noexcept {
      return aw_push_bulk_impl(*this);
    }
  };

  /// Returns a `pull_zc_scope` when awaited.
  class aw_pull final : private tmc::detail::AwaitTagNoGroupCoAwait {
    friend qu_spsc_bounded<T, Config>;

    qu_spsc_bounded& queue;

    aw_pull(qu_spsc_bounded& Queue) noexcept : queue(Queue) {}

    struct aw_pull_impl final {
      consumer_base base;
      qu_spsc_bounded& queue;
      size_t idx;

      aw_pull_impl(aw_pull& Parent) noexcept
          : base{tmc::detail::this_thread::executor(), nullptr,
                 tmc::detail::this_thread::this_task().prio, nullptr},
            queue{Parent.queue}, idx{0} {}

      bool await_ready() noexcept {
        element* myElem = queue.get_read_ticket(idx);
        base.elem = myElem;
        // Data is ready when DATA_BIT is set, possibly together with
        // CLOSED_BIT if close() ran while data was still in the slot.
        return (myElem->poll() & DATA_BIT) != 0;
      }

      bool await_suspend(std::coroutine_handle<> Outer) noexcept {
        base.continuation = Outer;
        uintptr_t prev = base.elem->try_wait(&base);
        if (prev == 0) {
          // We installed our consumer_base* into the slot. Either a
          // producer will publish data and wake us, or close() will swap
          // CLOSED_BIT into the slot, observe our pointer in its RMW, and
          // wake us. The slot-level RMW between close() and try_wait
          // linearizes these two cases without any need to consult the
          // queue-level `closed` flag.
          return true;
        }
        if (prev == CLOSED_BIT) [[unlikely]] {
          // Mark scope as empty (closed and drained).
          base.elem = nullptr;
        }
        // Prev contains DATA_BIT and/or CLOSED_BIT, or a producer_base* (queue
        // was full; data is still present in this slot). Either way, try_wait
        // left the slot unmodified and we proceed to read.
        return false;
      }

      TMC_AWAIT_RESUME pull_zc_scope await_resume() noexcept {
        // If closed, base.elem was already set to nullptr in await_suspend or
        // close(). This marks the zc_scope as empty.
        return pull_zc_scope(&queue, base.elem, idx);
      }
    };

  public:
    aw_pull_impl operator co_await() && noexcept { return aw_pull_impl(*this); }
  };

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
  /// destroyed and the queue slot freed for reuse. Only safe to call from the
  /// single consumer.
  ///
  /// The returned scope's `status()` returns:
  ///   - qu_spsc_bounded_err::OK     - a value was dequeued
  ///   - qu_spsc_bounded_err::EMPTY  - no value is currently available
  ///   - qu_spsc_bounded_err::CLOSED - the queue has been closed and drained
  ///
  /// The returned scope's `has_value()` / `operator bool()` returns true if a
  /// value was dequeued, or false if the queue was empty or closed.
  ///
  /// This scope must be released before the next call to `try_pull()` or
  /// `pull()`. It must also be released before the queue is destroyed.
  try_pull_zc_scope try_pull() {
    size_t Idx;
    element* elem = get_read_ticket(Idx);

    auto s = elem->poll();
    // (s & DATA_BIT) != 0: normal data ready, possibly also closed (queue
    //   was full at close time; finish_read will re-publish CLOSED_BIT for
    //   the wraparound).
    // s >= 4: a producer is suspended on this slot because the queue was
    //   full; the previous round's data is still present. finish_read will
    //   wake the producer when the scope is released.
    if ((s & DATA_BIT) != 0 || s >= 4) {
      return try_pull_zc_scope(this, elem, Idx);
    }
    if (s == CLOSED_BIT) {
      return try_pull_zc_scope(tmc::qu_spsc_bounded_err::CLOSED);
    }
    return try_pull_zc_scope(tmc::qu_spsc_bounded_err::EMPTY);
  }

  /// Destroys the queue and any contained values that have not yet been
  /// consumed.
  ///
  /// Before destroying this, you must ensure:
  /// - No producer is currently calling or suspended in push(), try_push(), or
  ///   push_bulk().
  /// - No consumer is calling or suspended in pull() / try_pull().
  /// - No pull_zc_scope / try_pull_zc_scope from this queue is alive.
  /// - No other thread is calling any other member function.
  ///
  /// The recommended teardown sequence is:
  /// 1. Stop submitting new push() calls.
  /// 2. close() the queue.
  /// 3. Drain via pull() / try_pull() until CLOSED.
  /// 4. Ensure no further queue method calls will occur (e.g. by joining all
  ///    producer and consumer coroutines).
  /// 5. Destroy the queue.
  ~qu_spsc_bounded() {
    close();
    // close() published a CLOSED sentinel at write_closed_at; that slot
    // holds no data, and no producer can fill any slot at or beyond it.
    size_t end = write_closed_at.load(std::memory_order_relaxed);
    size_t idx = read_offset;
    // If the consumer stopped consuming before the queue was drained, there
    // may be leftover data in the queue. Destroy it.
    while (circular_less_than(idx, end)) {
      element* elem = &values[idx % capacity];
      if (elem->is_data_waiting()) {
        elem->data.destroy();
      }
      ++idx;
    }
    delete[] values;
  }

  qu_spsc_bounded(const qu_spsc_bounded&) = delete;
  qu_spsc_bounded& operator=(const qu_spsc_bounded&) = delete;
  qu_spsc_bounded(qu_spsc_bounded&&) = delete;
  qu_spsc_bounded& operator=(qu_spsc_bounded&&) = delete;
};

} // namespace tmc
