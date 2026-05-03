// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

// Provides tmc::bounded_queue, an async MPMC bounded queue.

// Writers claim write tickets with fetch_add on a monotonically increasing
// write counter. Readers claim read tickets the same way from a separate read
// counter. Each ticket maps to a slot in a fixed-size circular buffer.
//
// If a reader reaches a slot before its writer has published data for that
// ticket, the reader suspends in that slot until the writer completes.
// If a writer reaches a slot before the previous reader has released it, the
// writer suspends in that slot until the reader completes.
//
// Since waiters are stored in the slots themselves, the total number of active
// readers and the total number of active writers must each never exceed the
// queue capacity. This guarantees that a side never wraps around and tries to
// install a second waiter into a slot that is still owned by an older waiter of
// the same kind.
//
// The queue does not use shared ownership. It is a regular class and must
// outlive any in-flight operations and any zero-copy scopes returned from it.

#include "tmc/detail/compat.hpp"
#include "tmc/detail/concepts_awaitable.hpp"
#include "tmc/detail/thread_locals.hpp"

#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <type_traits>
#include <utility>

namespace tmc {
namespace detail {
template <typename T> struct bounded_queue_storage {
  union alignas(alignof(T)) {
    T value;
  };
#ifndef NDEBUG
  bool exists = false;
#endif

  bounded_queue_storage() noexcept {}

  template <typename... ConstructArgs>
  void emplace(ConstructArgs&&... Args) noexcept {
#ifndef NDEBUG
    assert(!exists);
    exists = true;
#endif
    ::new (static_cast<void*>(&value)) T(static_cast<ConstructArgs&&>(Args)...);
  }

  void destroy() noexcept {
#ifndef NDEBUG
    assert(exists);
    exists = false;
#endif
    value.~T();
  }

  bounded_queue_storage(const bounded_queue_storage&) = delete;
  bounded_queue_storage& operator=(const bounded_queue_storage&) = delete;

  bounded_queue_storage(bounded_queue_storage&& Other) noexcept {
    emplace(static_cast<T&&>(Other.value));
    Other.destroy();
  }
  bounded_queue_storage& operator=(bounded_queue_storage&& Other) noexcept {
    emplace(static_cast<T&&>(Other.value));
    Other.destroy();
    return *this;
  }

#ifndef NDEBUG
  ~bounded_queue_storage() { assert(!exists); }
#else
  ~bounded_queue_storage()
    requires(std::is_trivially_destructible_v<T>)
  = default;
  ~bounded_queue_storage()
    requires(!std::is_trivially_destructible_v<T>)
  {}
#endif
};
} // namespace detail

struct bounded_queue_default_config {
  /// The number of slots in the circular buffer.
  /// Must be a power of 2.
  static inline constexpr size_t Capacity = 1024;

  /// At level 0, each slot is padded up to the next cache-line boundary.
  /// At level 1, no padding is applied.
  static inline constexpr size_t PackingLevel = 0;
};

template <typename T, typename Config = tmc::bounded_queue_default_config>
class bounded_queue {
  static_assert(std::is_nothrow_destructible_v<T>);

  static inline constexpr size_t Capacity = Config::Capacity;
  static inline constexpr size_t CapacityMask = Capacity - 1;
  static inline constexpr size_t CapacityShift = std::countr_zero(Capacity);
  static_assert(
    Capacity && ((Capacity & (Capacity - 1)) == 0),
    "Capacity must be a power of 2"
  );
  static_assert(Config::PackingLevel <= 1);
  static_assert(std::atomic<size_t>::is_always_lock_free);
  static_assert(std::atomic<void*>::is_always_lock_free);

  struct slot_waiter {
    tmc::ex_any* continuation_executor;
    std::coroutine_handle<> continuation;
    size_t prio;
  };

  static inline constexpr uintptr_t WriterWaiterBit = 1;
  static_assert(alignof(slot_waiter) >= 2);

  class slot {
  public:
    std::atomic<size_t> turn;
    std::atomic<void*> waiter;
    tmc::detail::bounded_queue_storage<T> data;

    static constexpr size_t UNPADLEN =
      sizeof(std::atomic<size_t>) + sizeof(std::atomic<void*>) +
      sizeof(tmc::detail::bounded_queue_storage<T>);
    static constexpr size_t WANTLEN = (UNPADLEN + TMC_CACHE_LINE_SIZE - 1) &
                                      static_cast<size_t>(
                                        0 - TMC_CACHE_LINE_SIZE
                                      );
    static constexpr size_t PADLEN =
      UNPADLEN < WANTLEN ? (WANTLEN - UNPADLEN) : 999;

    struct empty {};
    using Padding = std::conditional_t<
      Config::PackingLevel == 0 && PADLEN != 999, char[PADLEN], empty>;
    TMC_NO_UNIQUE_ADDRESS Padding pad;

    slot() noexcept : turn(0), waiter(nullptr) {}
  };

  struct push_state {
    slot* elem;
    size_t ready_turn;
    size_t publish_turn;
  };

  struct pull_state {
    slot* elem;
    size_t ready_turn;
    size_t release_turn;
  };

public:
  class zc_scope;
  class started_pull_zc;
  template <typename... StoredArgs> class aw_push;
  class aw_pull;
  class aw_pull_zc;
  class aw_pull_zc_started;

private:

  alignas(TMC_CACHE_LINE_SIZE) std::atomic<size_t> write_count;
  char pad0[TMC_CACHE_LINE_SIZE - sizeof(std::atomic<size_t>)];
  std::atomic<size_t> read_count;
  char pad1[TMC_CACHE_LINE_SIZE - sizeof(std::atomic<size_t>)];
  std::array<slot, Capacity> elements;

  static inline size_t slot_index(size_t Ticket) noexcept {
    return Ticket & CapacityMask;
  }

  static inline size_t empty_turn(size_t Ticket) noexcept {
    return (Ticket >> CapacityShift) << 1;
  }

  static inline size_t full_turn(size_t Ticket) noexcept {
    return empty_turn(Ticket) + 1;
  }

  static inline size_t next_empty_turn(size_t Ticket) noexcept {
    return empty_turn(Ticket) + 2;
  }

  static inline bool slot_ready(slot const& Elem, size_t ReadyTurn) noexcept {
    return ReadyTurn == Elem.turn.load(std::memory_order_acquire);
  }

  static inline void observe_ready_turn(
    slot const& Elem, size_t ReadyTurn
  ) noexcept {
    size_t observed = Elem.turn.load(std::memory_order_acquire);
    assert(observed == ReadyTurn);
    (void)ReadyTurn;
    (void)observed;
  }

  static inline void* tag_waiter(slot_waiter* Waiter, bool Writer) noexcept {
    uintptr_t addr = reinterpret_cast<uintptr_t>(Waiter);
    assert((addr & WriterWaiterBit) == 0);
    if (Writer) {
      addr |= WriterWaiterBit;
    }
    return reinterpret_cast<void*>(addr);
  }

  static inline slot_waiter* untag_waiter(void* TaggedWaiter) noexcept {
    auto addr = reinterpret_cast<uintptr_t>(TaggedWaiter);
    addr &= ~WriterWaiterBit;
    return reinterpret_cast<slot_waiter*>(addr);
  }

  static inline bool is_writer_waiter(void* TaggedWaiter) noexcept {
    return 0 != (reinterpret_cast<uintptr_t>(TaggedWaiter) & WriterWaiterBit);
  }

  static inline void resume_waiter(void* TaggedWaiter) noexcept {
    auto* waiter = untag_waiter(TaggedWaiter);
    tmc::detail::post_checked(
      waiter->continuation_executor, std::move(waiter->continuation),
      waiter->prio
    );
  }

  bool suspend_for_turn(
    slot& Elem, slot_waiter& Waiter, size_t ReadyTurn, bool Writer,
    std::coroutine_handle<> Outer
  ) noexcept {
    Waiter.continuation_executor = tmc::detail::this_thread::executor();
    Waiter.continuation = Outer;
    Waiter.prio = tmc::detail::this_thread::this_task().prio;

    void* tagged = tag_waiter(&Waiter, Writer);
    while (true) {
      if (slot_ready(Elem, ReadyTurn)) {
        return false;
      }

      void* expected = nullptr;
      if (Elem.waiter.compare_exchange_weak(
            expected, tagged, std::memory_order_acq_rel,
            std::memory_order_acquire
          )) {
        // Install the waiter before re-checking the turn. If the slot became
        // ready during the CAS window, either we retract the waiter and proceed
        // inline, or the other side already consumed it and will resume us.
        if (slot_ready(Elem, ReadyTurn)) {
          void* mine = tagged;
          if (Elem.waiter.compare_exchange_strong(
                mine, nullptr, std::memory_order_acq_rel,
                std::memory_order_acquire
              )) {
            return false;
          }
        }
        return true;
      }

      TMC_CPU_PAUSE();
    }
  }

  static void wake_waiter(slot& Elem, bool WakeWriter) noexcept {
    void* tagged = Elem.waiter.exchange(nullptr, std::memory_order_acq_rel);
    if (tagged == nullptr) {
      return;
    }
    assert(WakeWriter == is_writer_waiter(tagged));
    (void)WakeWriter;
    resume_waiter(tagged);
  }

  push_state begin_push() noexcept {
    size_t ticket = write_count.fetch_add(1, std::memory_order_relaxed);
    return push_state{
      &elements[slot_index(ticket)], empty_turn(ticket), full_turn(ticket)};
  }

  pull_state begin_pull() noexcept {
    size_t ticket = read_count.fetch_add(1, std::memory_order_relaxed);
    return pull_state{
      &elements[slot_index(ticket)], full_turn(ticket), next_empty_turn(ticket)};
  }

  template <typename Tuple>
  void complete_push(
    slot& Elem, size_t ReadyTurn, size_t PublishTurn, Tuple&& Args
  ) noexcept {
    observe_ready_turn(Elem, ReadyTurn);
    std::apply(
      [&](auto&&... StoredArgs) {
        Elem.data.emplace(static_cast<decltype(StoredArgs)&&>(StoredArgs)...);
      },
      static_cast<Tuple&&>(Args)
    );
    Elem.turn.store(PublishTurn, std::memory_order_release);
    wake_waiter(Elem, false);
  }

  T complete_pull(
    slot& Elem, size_t ReadyTurn, size_t ReleaseTurn
  ) noexcept {
    observe_ready_turn(Elem, ReadyTurn);
    T value(std::move(Elem.data.value));
    Elem.data.destroy();
    Elem.turn.store(ReleaseTurn, std::memory_order_release);
    wake_waiter(Elem, true);
    return value;
  }

public:
  class zc_scope {
    slot* elem;
    size_t release_turn;

    friend class bounded_queue;

    zc_scope(slot* Elem, size_t ReleaseTurn) noexcept
        : elem{Elem}, release_turn{ReleaseTurn} {}

    void release() noexcept {
      if (elem != nullptr) {
        elem->data.destroy();
        elem->turn.store(release_turn, std::memory_order_release);
        wake_waiter(*elem, true);
        elem = nullptr;
      }
    }

  public:
    zc_scope(const zc_scope&) = delete;
    zc_scope& operator=(const zc_scope&) = delete;

    zc_scope(zc_scope&& Other) noexcept
        : elem{Other.elem}, release_turn{Other.release_turn} {
      Other.elem = nullptr;
    }

    zc_scope& operator=(zc_scope&& Other) noexcept {
      if (this != &Other) {
        release();
        elem = Other.elem;
        release_turn = Other.release_turn;
        Other.elem = nullptr;
      }
      return *this;
    }

    T& get() noexcept { return elem->data.value; }
    T& operator*() noexcept { return elem->data.value; }
    T* operator->() noexcept { return &elem->data.value; }

    ~zc_scope() { release(); }
  };

  class [[nodiscard(
    "You must continue the result of start_pull_zc() with "
    "std::move(started).pull_zc()."
  )]] started_pull_zc {
    bounded_queue* queue;
    pull_state state;
    bool ready;

    friend class bounded_queue;

    started_pull_zc(bounded_queue* Queue, pull_state State) noexcept
        : queue{Queue}, state{State}, ready{slot_ready(*State.elem, State.ready_turn)} {}

    pull_state release_state() noexcept {
      pull_state result = state;
      state.elem = nullptr;
      ready = false;
      return result;
    }

  public:
    started_pull_zc(const started_pull_zc&) = delete;
    started_pull_zc& operator=(const started_pull_zc&) = delete;

    started_pull_zc(started_pull_zc&& Other) noexcept
        : queue{Other.queue}, state{Other.state}, ready{Other.ready} {
      Other.state.elem = nullptr;
      Other.ready = false;
    }

    started_pull_zc& operator=(started_pull_zc&&) noexcept = delete;

    explicit operator bool() const noexcept { return ready; }

    [[nodiscard]] bool refresh_ready() noexcept {
      assert(state.elem != nullptr);
      if (!ready) {
        ready = slot_ready(*state.elem, state.ready_turn);
      }
      return ready;
    }

    aw_pull_zc_started pull_zc() && noexcept;

    ~started_pull_zc() {
#ifndef NDEBUG
      assert(
        state.elem == nullptr &&
        "You must continue the result of start_pull_zc() with "
        "std::move(started).pull_zc()."
      );
#endif
    }
  };

  template <typename... StoredArgs>
  class aw_push final : private tmc::detail::AwaitTagNoGroupAsIs {
    bounded_queue* queue;
    push_state state;
    slot_waiter waiter;
    std::tuple<StoredArgs...> args;

    friend class bounded_queue;

    template <typename... ConstructArgs>
    aw_push(
      bounded_queue& Queue, push_state State, ConstructArgs&&... ConstructArgsIn
    ) noexcept
        : queue{&Queue}, state{State}, waiter{nullptr, nullptr, 0},
          args(static_cast<ConstructArgs&&>(ConstructArgsIn)...) {}

  public:
    aw_push(const aw_push&) = delete;
    aw_push& operator=(const aw_push&) = delete;
    aw_push(aw_push&&) = default;
    aw_push& operator=(aw_push&&) = default;

    bool await_ready() noexcept {
      return slot_ready(*state.elem, state.ready_turn);
    }

    bool await_suspend(std::coroutine_handle<> Outer) noexcept {
      return queue->suspend_for_turn(
        *state.elem, waiter, state.ready_turn, true, Outer
      );
    }

    TMC_AWAIT_RESUME void await_resume() noexcept {
      queue->complete_push(
        *state.elem, state.ready_turn, state.publish_turn, std::move(args)
      );
    }
  };

  class aw_pull final : private tmc::detail::AwaitTagNoGroupAsIs {
    bounded_queue* queue;
    pull_state state;
    slot_waiter waiter;

    friend class bounded_queue;

    aw_pull(bounded_queue& Queue, pull_state State) noexcept
        : queue{&Queue}, state{State}, waiter{nullptr, nullptr, 0} {}

  public:
    aw_pull(const aw_pull&) = delete;
    aw_pull& operator=(const aw_pull&) = delete;
    aw_pull(aw_pull&&) = default;
    aw_pull& operator=(aw_pull&&) = default;

    bool await_ready() noexcept {
      return slot_ready(*state.elem, state.ready_turn);
    }

    bool await_suspend(std::coroutine_handle<> Outer) noexcept {
      return queue->suspend_for_turn(
        *state.elem, waiter, state.ready_turn, false, Outer
      );
    }

    TMC_AWAIT_RESUME T await_resume() noexcept {
      return queue->complete_pull(*state.elem, state.ready_turn, state.release_turn);
    }
  };

  class aw_pull_zc final : private tmc::detail::AwaitTagNoGroupAsIs {
    bounded_queue* queue;
    pull_state state;
    slot_waiter waiter;

    friend class bounded_queue;

    aw_pull_zc(bounded_queue& Queue, pull_state State) noexcept
        : queue{&Queue}, state{State}, waiter{nullptr, nullptr, 0} {}

  public:
    aw_pull_zc(const aw_pull_zc&) = delete;
    aw_pull_zc& operator=(const aw_pull_zc&) = delete;
    aw_pull_zc(aw_pull_zc&&) = default;
    aw_pull_zc& operator=(aw_pull_zc&&) = default;

    bool await_ready() noexcept {
      return slot_ready(*state.elem, state.ready_turn);
    }

    bool await_suspend(std::coroutine_handle<> Outer) noexcept {
      return queue->suspend_for_turn(
        *state.elem, waiter, state.ready_turn, false, Outer
      );
    }

    TMC_AWAIT_RESUME zc_scope await_resume() noexcept {
      observe_ready_turn(*state.elem, state.ready_turn);
      return zc_scope(state.elem, state.release_turn);
    }
  };

  class aw_pull_zc_started final : private tmc::detail::AwaitTagNoGroupAsIs {
    bounded_queue* queue;
    pull_state state;
    slot_waiter waiter;

    friend class started_pull_zc;

    aw_pull_zc_started(bounded_queue& Queue, pull_state State) noexcept
        : queue{&Queue}, state{State}, waiter{nullptr, nullptr, 0} {}

  public:
    aw_pull_zc_started(const aw_pull_zc_started&) = delete;
    aw_pull_zc_started& operator=(const aw_pull_zc_started&) = delete;
    aw_pull_zc_started(aw_pull_zc_started&&) = default;
    aw_pull_zc_started& operator=(aw_pull_zc_started&&) = default;

    bool await_ready() noexcept {
      return slot_ready(*state.elem, state.ready_turn);
    }

    bool await_suspend(std::coroutine_handle<> Outer) noexcept {
      return queue->suspend_for_turn(
        *state.elem, waiter, state.ready_turn, false, Outer
      );
    }

    TMC_AWAIT_RESUME zc_scope await_resume() noexcept {
      observe_ready_turn(*state.elem, state.ready_turn);
      return zc_scope(state.elem, state.release_turn);
    }
  };

  bounded_queue() noexcept : write_count{0}, read_count{0} {}

  static constexpr size_t capacity() noexcept { return Capacity; }

  template <typename... Args>
  [[nodiscard("You must co_await push().")]] aw_push<std::decay_t<Args>...>
  push(Args&&... ConstructArgs) noexcept {
    static_assert(std::is_nothrow_constructible_v<T, Args&&...>);
    return aw_push<std::decay_t<Args>...>(
      *this, begin_push(), static_cast<Args&&>(ConstructArgs)...
    );
  }

  [[nodiscard("You must co_await pull().")]] aw_pull pull() noexcept {
    static_assert(std::is_nothrow_move_constructible_v<T>);
    return aw_pull(*this, begin_pull());
  }

  [[nodiscard(
    "You must continue the result of start_pull_zc() with "
    "std::move(started).pull_zc()."
  )]] started_pull_zc
  start_pull_zc() noexcept {
    return started_pull_zc(this, begin_pull());
  }

  [[nodiscard("You must co_await pull_zc().")]] aw_pull_zc pull_zc() noexcept {
    return aw_pull_zc(*this, begin_pull());
  }

  ~bounded_queue() {
    size_t readTicket = read_count.load(std::memory_order_relaxed);
    size_t writeTicket = write_count.load(std::memory_order_relaxed);
    while (readTicket != writeTicket) {
      slot& elem = elements[slot_index(readTicket)];
      if (slot_ready(elem, full_turn(readTicket))) {
        elem.data.destroy();
      }
      ++readTicket;
    }

#ifndef NDEBUG
    for (auto& elem : elements) {
      assert(elem.waiter.load(std::memory_order_relaxed) == nullptr);
    }
#endif
  }

  bounded_queue(const bounded_queue&) = delete;
  bounded_queue& operator=(const bounded_queue&) = delete;
  bounded_queue(bounded_queue&&) = delete;
  bounded_queue& operator=(bounded_queue&&) = delete;
};

template <typename T, typename Config>
inline typename bounded_queue<T, Config>::aw_pull_zc_started
bounded_queue<T, Config>::started_pull_zc::pull_zc() && noexcept {
  assert(state.elem != nullptr);
  return aw_pull_zc_started(*queue, release_state());
}

} // namespace tmc
