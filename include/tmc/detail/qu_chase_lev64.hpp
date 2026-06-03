// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/compat.hpp"
#include "tmc/detail/tsan.hpp"

#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>

namespace tmc {
namespace detail {

// A Chase-Lev work-stealing deque.
//
// The owner (single producer/consumer) calls push() and try_pop() at the tail.
// Any other thread may call steal() to take work from the head.
//
// Buffers can grow. When the owner detects its buffer is full, it allocates a
// new buffer of double the size, copies all elements over, and atomically
// publishes the new buffer. Old buffers are retained in a "leftovers" vector
// so that stealers that loaded the old buffer pointer can still access it
// safely. Leftovers are destroyed only when the deque itself is destroyed.
//
// T must be trivially copyable (and trivially destructible). This is sufficient
// for tmc::work_item (either std::coroutine_handle<> or tmc::coro_functor),
// both of which are non-owning handles whose underlying allocations are freed
// only by the unique copy that is actually executed.
//
// This variant packs the top (head) and bottom (tail) indices into a single
// 8-byte-aligned region:
//   - bytes 0-3 (low 32 bits in a 64-bit load) = top
//   - bytes 4-7 (high 32 bits in a 64-bit load) = bottom (owner writes this)
// Indices are 32 bits, so the deque capacity maxes out below 2^31 elements.
// Comparisons use signed 32-bit differences so the algorithm is correct
// across 32-bit wraparound as long as |b - t| < 2^31.
//
// Because the owner is the sole writer of bottom, push() and pop() update
// the bottom half via a plain aligned 32-bit store (no LOCK prefix). On x86
// this preserves the cheap relaxed-store hot path of classic Chase-Lev while
// still letting stealers grab a consistent (top, bottom) snapshot in a single
// aligned 64-bit load.
//
// Last-element race: rather than have pop() CAS the top, steal() CAS-es the
// full 64-bit (top, bottom) word (incrementing only top). pop() decrements
// bottom before reading top, so any in-flight stealer with a snapshot of
// (t, t+1) will observe the decremented bottom and fail its full-word CAS.
// This means pop() always wins the last-element race once its bottom
// decrement is visible - it does not need its own CAS, only a plain store
// to publish the new empty (top, bottom) state.

template <typename T> class chase_lev_deque {
  static_assert(
    std::is_trivially_copyable_v<T>,
    "chase_lev_deque requires a trivially copyable element type"
  );
  static_assert(
    std::is_trivially_destructible_v<T>,
    "chase_lev_deque requires a trivially destructible element type"
  );
  static_assert(
    std::endian::native == std::endian::little,
    "chase_lev_deque assumes little-endian"
  );

  // Data buffers are over-aligned to 64 bytes so the low 6 bits of the data
  // pointer are free for use as a tag. We use 5 of those bits to encode
  // log2(capacity) (capacity is always a power of two and is bounded by 2^31
  // due to the 32-bit indices), which lets us pack (data_ptr, mask) into a
  // single 8-byte word that stealers can read with one atomic load.
  static constexpr size_t BUFFER_DATA_ALIGN = 64;
  static constexpr uintptr_t TAG_MASK = BUFFER_DATA_ALIGN - 1;

  struct buffer {
    // Owning pointer to the data block. The capacity / mask are NOT stored
    // here; they are recovered from the tagged active cell in active_data_
    // when needed. This field exists only so that ~buffer can free the block.
    T* data;

    buffer() : data(nullptr) {}

    void init(size_t cap) {
      assert((cap & (cap - 1)) == 0 && "capacity must be a power of two");
      data = static_cast<T*>(
        ::operator new(sizeof(T) * cap, std::align_val_t{BUFFER_DATA_ALIGN})
      );
      assert(
        (reinterpret_cast<uintptr_t>(data) & TAG_MASK) == 0 &&
        "data pointer must be aligned to BUFFER_DATA_ALIGN"
      );
    }

    ~buffer() {
      if (data != nullptr) {
        ::operator delete(data, std::align_val_t{BUFFER_DATA_ALIGN});
      }
    }

    buffer(const buffer&) = delete;
    buffer& operator=(const buffer&) = delete;
  };

  // Packed (top, bottom) state. Logically two 32-bit fields packed into a
  // single 8-byte word:
  //   bytes 0..3 (low 32 bits)  = bottom (owner-written via push/pop)
  //   bytes 4..7 (high 32 bits) = top    (CAS'd by stealers; some pop paths)
  // Stealers must read both halves in a single 64-bit atomic load to get a
  // consistent snapshot, so the canonical storage type is uint64_t. All
  // full-word loads / stores / CAS / RMW go through state_full() as
  // atomic_ref<uint64_t> over this object directly - no aliasing.
  alignas(TMC_CACHE_LINE_SIZE) uint64_t state_storage_;

  // The owner's fast-path bottom update is a single 32-bit aligned atomic
  // store. This avoids the LOCK prefix a 64-bit RMW would incur on x86 and
  // is safe to race with a stealer's full-word CAS (the CAS observes the
  // changed bottom and fails).
  //
  // Expressing "atomic 32-bit store to the low half of a 64-bit atomic" is
  // not possible in strictly standards-compliant C++:
  //   - std::atomic_ref<T> requires the referenced object to be of type T,
  //     so atomic_ref<uint32_t> over half of a uint64_t violates its
  //     preconditions.
  //   - Strict aliasing ([basic.lval]) does not list uint32_t as a type
  //     that may alias a uint64_t object.
  //   - A union of atomic<uint64_t>/atomic<uint32_t> trips the active-member
  //     rule (atomic<T> has no guaranteed common initial sequence).
  //   - Replacing the 32-bit store with a 64-bit RMW would cost a LOCK and
  //     defeat the optimization (and would corrupt the top half when
  //     bottom == UINT32_MAX).
  // We use the GCC/Clang __may_alias__ attribute to permit the access. This
  // is the ONLY place in this file that accesses state_storage_ via a type
  // other than its declared uint64_t.
  using aliased_u32 __attribute__((__may_alias__)) = uint32_t;

  // Owner-private "active buffer*" - used only by grow() to find the previous
  // buffer slot and advance to oldBuf + 1. No synchronization with stealers
  // needed (relaxed accesses are fine).
  alignas(TMC_CACHE_LINE_SIZE) buffer* active_buffer_;

  // Tagged (data_ptr | log2(capacity)) cell that push/pop/steal read with a
  // single atomic load to recover both the active data pointer and its mask
  // without an extra indirection. Owner updates this with a release-store in
  // the constructor and grow(); stealers acquire-load.
  std::atomic<uintptr_t> active_data_;

  // Buffers retained until destruction so stealers always have a valid pointer.
  // Each slot's data pointer starts as nullptr and is allocated when the slot
  // becomes the active buffer (initial construction or grow()). The doubling-
  // on-grow behavior guarantees we never need more than 62 slots in practice.
  std::array<buffer, 62> buffers_;

  static constexpr size_t DEFAULT_INITIAL_CAPACITY = 128;

  // Atomic accessors over the 8-byte state word.
  //
  // state_full() returns an atomic_ref over the canonical uint64_t storage -
  // no aliasing, this is the natural type. Used for every load, store, CAS,
  // and RMW in the file.
  //
  // state_bottom() returns an atomic_ref over the low 32 bits of the same
  // word via the __may_alias__ uint32_t alias declared above. Used only for
  // owner-only stores (push/post_bulk/pop's restore path); never for loads.
  std::atomic_ref<uint64_t> state_full() noexcept {
    return std::atomic_ref<uint64_t>(state_storage_);
  }
  std::atomic_ref<uint64_t> state_full() const noexcept {
    return std::atomic_ref<uint64_t>(const_cast<uint64_t&>(state_storage_));
  }
  std::atomic_ref<uint32_t> state_bottom() noexcept {
    // Little-endian (asserted above): bottom occupies bytes 0..3. The
    // pointer is laundered through void* to avoid clang's
    // -Wundefined-reinterpret-cast (the may_alias attribute permits the
    // access at runtime but the direct uint64_t*->uint32_t* cast still
    // trips the warning).
    return std::atomic_ref<uint32_t>(
      *static_cast<aliased_u32*>(static_cast<void*>(&state_storage_))
    );
  }

  // Return the top/bottom halves as uint32_t. All arithmetic on indices is
  // performed in unsigned (defined-wrap) form; callers reinterpret the
  // difference as int32_t only at relational comparisons (the Chase-Lev
  // signed-difference trick).
  static uint32_t unpack_top(uint64_t s) {
    return static_cast<uint32_t>(s >> 32);
  }
  static uint32_t unpack_bottom(uint64_t s) { return static_cast<uint32_t>(s); }

  // Unpack the data pointer from the tagged (data | log2(capacity)) cell
  // stored in active_data_.
  static T* unpack_data(uintptr_t w) {
    return reinterpret_cast<T*>(w & ~static_cast<uintptr_t>(TAG_MASK));
  }

  // Unpack the mask (capacity - 1) from the tagged
  // (data | log2(capacity)) cell stored in active_data_.
  static size_t unpack_mask(uintptr_t w) {
    size_t lc = static_cast<size_t>(w & TAG_MASK);
    return (TMC_ONE_BIT << lc) - 1;
  }

  // Acquire-load of the tagged active cell. Used by stealers, which need to
  // synchronize-with grow()'s release-store to see the copied elements in
  // the new buffer. The caller must unpack the result via unpack_data() and
  // unpack_mask().
  uintptr_t load_active_acquire() const {
    return active_data_.load(std::memory_order_acquire);
  }

  // Relaxed load - safe for the owner, since the owner is the only writer.
  // The caller must unpack the result via unpack_data() and unpack_mask().
  uintptr_t load_active_relaxed() const {
    return active_data_.load(std::memory_order_relaxed);
  }

  // Owner-only. Publish a new (data, mask) pair for stealers. The
  // release-store synchronizes-with stealers' acquire-load above, ensuring
  // any element copies performed by grow() into the new buffer become
  // visible.
  void publish_active(T* data, size_t cap) {
    assert((cap & (cap - 1)) == 0 && "capacity must be a power of two");
    size_t lc = static_cast<size_t>(std::countr_zero(cap));
    assert(lc <= TAG_MASK && "log2(capacity) does not fit in tag bits");
    uintptr_t tagged = reinterpret_cast<uintptr_t>(data) | lc;
    active_data_.store(tagged, std::memory_order_release);
  }

  // For T sizes that fit in a single hardware atomic word, we use
  // std::atomic_ref so that push/pop/steal slot accesses are well-defined under
  // the C++ memory model and TSAN-clean. The ordering of these atomics is
  // relaxed; the actual happens-before edge for slot visibility is provided by
  // the release-store / acquire-load on the packed state word.
  //
  // For larger T (notably the 2-pointer coro_functor used when
  // TMC_WORK_ITEM=FUNCORO), std::atomic_ref<T> would either generate a
  // multi-word CAS or fall back to an internal lock. We instead use the classic
  // Chase-Lev "benign racy" approach which ignores invalid reads afterward, and
  // simply disable TSan for the helper.
  template <typename U>
  TMC_INLINE_OR_TSAN static void store_item(T* slot, U&& item) {
    if constexpr (sizeof(T) <= sizeof(size_t)) {
      T tmp(static_cast<U&&>(item));
      std::atomic_ref<T>(*slot).store(tmp, std::memory_order_relaxed);
    } else {
      new (slot) T(static_cast<U&&>(item));
    }
  }

  TMC_INLINE_OR_TSAN static void load_item(T& out, T* slot) {
    if constexpr (sizeof(T) <= sizeof(size_t)) {
      out = std::atomic_ref<T>(*slot).load(std::memory_order_relaxed);
    } else {
      std::memcpy(
        static_cast<void*>(&out), static_cast<const void*>(slot), sizeof(T)
      );
    }
  }

public:
  chase_lev_deque() : chase_lev_deque(DEFAULT_INITIAL_CAPACITY) {}

  explicit chase_lev_deque(size_t initialCapacity) {
    size_t cap = 1;
    while (cap < initialCapacity) {
      cap <<= 1;
    }
    buffer* buf = &buffers_[0];
    buf->init(cap);
    active_buffer_ = buf;
    publish_active(buf->data, cap);
    // start at 1 so we don't underflow when subtracting bottom right away
    state_full().store(
      (TMC_ONE_BIT << 32) | TMC_ONE_BIT, std::memory_order_relaxed
    );
  }

  ~chase_lev_deque() = default;

  chase_lev_deque(const chase_lev_deque&) = delete;
  chase_lev_deque& operator=(const chase_lev_deque&) = delete;
  chase_lev_deque(chase_lev_deque&&) = delete;
  chase_lev_deque& operator=(chase_lev_deque&&) = delete;

  void grow(size_t cap, uint32_t b, uint32_t t, uint32_t count) {
    // Buffer too small - grow to fit.
    cap *= 2;
    // All index arithmetic is done in uint32_t so that wraparound is defined
    // behavior (signed overflow would be UB). The (b - t) difference is the
    // queue's current size in two's-complement representation and adding
    // count likewise wraps cleanly.
    size_t needed = static_cast<size_t>(b - t + count);
    while (cap < needed) {
      cap <<= 1;
    }

    // Recover the old buffer's data pointer and mask from the tagged active
    // cell (owner-only access - relaxed load is safe).
    uintptr_t old_aw = load_active_relaxed();
    T* old_data = unpack_data(old_aw);
    size_t old_mask = unpack_mask(old_aw);

    buffer* oldBuf = active_buffer_;
    buffer* nb = oldBuf + 1;
    nb->init(cap);
    size_t new_mask = cap - 1;
    for (uint32_t i = t; i != b; ++i) {
      std::memcpy(
        static_cast<void*>(nb->data + (static_cast<size_t>(i) & new_mask)),
        static_cast<const void*>(
          old_data + (static_cast<size_t>(i) & old_mask)
        ),
        sizeof(T)
      );
    }
    active_buffer_ = nb;
    // Release-store synchronizes-with stealers' acquire-load of the tagged
    // cell. The element memcpys above are sequenced-before this store, so
    // any stealer that observes the new (data, mask) pair will also observe
    // the copied elements.
    publish_active(nb->data, cap);
  }

  // Owner-only. Push an item at the bottom (tail) of the deque.
  // Grows the buffer if it is full.
  template <typename U> TMC_FORCE_INLINE void push(U&& item) {
    auto state = state_full().load(std::memory_order_acquire);
    uint32_t b = unpack_bottom(state);
    uint32_t t = unpack_top(state);
    // Owner is the only writer of the tagged active cell, so a relaxed
    // load is sufficient here.
    uintptr_t aw = load_active_relaxed();
    T* data = unpack_data(aw);
    size_t mask = unpack_mask(aw);
    // (b - t) is computed in uint32_t (defined wraparound). Reinterpret as
    // int32_t for the signed comparison against mask (the Chase-Lev trick).
    if (static_cast<int32_t>(b - t) > static_cast<int32_t>(mask)) [[unlikely]] {
      grow(mask + 1, b, t, 1);
      // grow() just published a new active cell; re-read it (owner-only,
      // relaxed) to pick up the new data pointer and mask.
      aw = load_active_relaxed();
      data = unpack_data(aw);
      mask = unpack_mask(aw);
    }

    store_item(data + (static_cast<size_t>(b) & mask), static_cast<U&&>(item));
    // Plain 32-bit aligned store to the bottom half. release publishes the
    // slot store to any stealer that subsequently observes the new bottom.
    state_bottom().store(b + 1u, std::memory_order_release);
  }

  // Owner-only. Push Count items at the bottom (tail) of the deque, taken
  // from the iterator It (incremented Count times). Grows the buffer if it
  // is full. Issues only a single bottom update regardless of Count.
  template <typename It>
  TMC_FORCE_INLINE void post_bulk(It&& Items, size_t Count) {
    if (Count == 0) [[unlikely]] {
      return;
    }
    auto state = state_full().load(std::memory_order_acquire);
    uint32_t b = unpack_bottom(state);
    uint32_t t = unpack_top(state);
    uintptr_t aw = load_active_relaxed();
    T* data = unpack_data(aw);
    size_t mask = unpack_mask(aw);
    size_t capacity = mask + 1;
    // Compute (queue_size + Count) entirely in uint32_t (defined wrap), then
    // reinterpret as int32_t for the signed comparison.
    uint32_t needed = (b - t) + static_cast<uint32_t>(Count);
    if (static_cast<int32_t>(needed) > static_cast<int32_t>(capacity))
      [[unlikely]] {
      grow(capacity, b, t, static_cast<uint32_t>(Count));
      // grow() just published a new active cell; re-read it (owner-only,
      // relaxed) to pick up the new data pointer and mask.
      aw = load_active_relaxed();
      data = unpack_data(aw);
      mask = unpack_mask(aw);
    }

    It it = static_cast<It&&>(Items);
    for (size_t i = 0; i < Count; ++i) {
      store_item(
        data + (static_cast<size_t>(b + static_cast<uint32_t>(i)) & mask),
        static_cast<T&&>(*it)
      );
      ++it;
    }
    uint32_t newBottom = b + static_cast<uint32_t>(Count);
    state_bottom().store(newBottom, std::memory_order_release);
  }

  // Owner-only. Pop an item from the bottom (tail) of the deque (LIFO).
  // Returns true if an item was popped, false if the deque was empty.
  //
  // Last-element race resolution: pop() does not CAS. It decrements bottom
  // before the seq_cst fence, then reads top. Any concurrent stealer's
  // full-word CAS will observe the decremented bottom and fail, so pop()
  // wins as long as no stealer's CAS completed before the bottom decrement
  // became visible (in which case pop sees top advanced and returns false).
  TMC_FORCE_INLINE bool try_pop(T& out) {
    uintptr_t aw = load_active_relaxed();
    T* data = unpack_data(aw);
    size_t mask = unpack_mask(aw);
    auto state = state_full().load(std::memory_order_acquire);
    uint32_t b = unpack_bottom(state);
    uint32_t t = unpack_top(state);
  RETRY:
    // (b - t) is computed in uint32_t (defined wraparound). Reinterpret as
    // int32_t for the signed sign check (the Chase-Lev trick).
    int32_t diff = static_cast<int32_t>(b - t);
    if (diff <= 0) {
      // Queue was empty
      return false;
    }
    if (diff == 0) {
      // Queue has one element. Try to claim it by advancing top via CAS.
      uint64_t newState =
        static_cast<uint64_t>(b) | (static_cast<uint64_t>(t + 1u) << 32);
      if (state_full().compare_exchange_strong(
            state, newState, std::memory_order_seq_cst,
            std::memory_order_relaxed
          )) {
        load_item(out, data + (static_cast<size_t>(b) & mask));
      }
      return false;
    }
    // Queue has more than one element. Decrement bottom to claim it.
    if (b != 0) [[likely]] {
      // Happiest path - try to complete the entire operation in a single FAA
      state = state_full().fetch_sub(1, std::memory_order_acq_rel);
      --b;
      t = unpack_top(state);
    } else {
      // Underflow case - bottom is at 0 and we need to decrement it.
      // Use CAS to set bottom without affecting top.
      // (b - 1u) wraps to UINT32_MAX, which is the intended bit pattern.
      uint64_t newState = (static_cast<uint64_t>(t) << 32) | (b - 1u);
      if (state_full().compare_exchange_strong(
            state, newState, std::memory_order_acq_rel,
            std::memory_order_relaxed
          )) {
        --b;
      } else {
        // Stealer modified top
        t = unpack_top(state);
        goto RETRY;
      }
    }
    diff = static_cast<int32_t>(b - t);
    if (diff < 0) [[unlikely]] {
      // Empty. Restore bottom.
      state_bottom().store(b + 1u, std::memory_order_relaxed);
      return false;
    }

    load_item(out, data + (static_cast<size_t>(b) & mask));
    if (diff > 0) {
      // More than one element - safe to take without racing stealers.
      return true;
    }
    // diff == 0
    // Last element - pop always wins. Take the element and publish the
    // empty state (top=t+1=b+1, bottom=b+1) in a single full-word store.
    // Any in-flight stealer with snapshot (t, t+1) will fail its full-word
    // CAS because both halves have changed (and bottom was already
    // decremented to t before this store). Advancing top here is essential
    // to prevent ABA: without it a subsequent push would restore the
    // packed state to (t, t+1), matching the old stealer's expected value.
    uint64_t empty_state =
      static_cast<uint64_t>(b + 1u) | (static_cast<uint64_t>(b + 1u) << 32);
    state_full().store(empty_state, std::memory_order_relaxed);
    return true;
  }

  // Any thread. Steal an item from the top (head) of the deque (FIFO).
  // Returns true if an item was stolen, false otherwise (deque empty or lost
  // a race).
  TMC_FORCE_INLINE bool steal(T& out) {
    size_t retryCount = 0;
    // Single 64-bit acquire load gives a consistent (top, bottom) snapshot.
    uint64_t s = state_full().load(std::memory_order_acquire);
  RETRY:
    uint32_t t = unpack_top(s);
    uint32_t b = unpack_bottom(s);
    // (b - t) is computed in uint32_t (defined wraparound). Reinterpret as
    // int32_t for the signed sign check.
    if (static_cast<int32_t>(b - t) <= 0) {
      return false;
    }
    // Single acquire-load of the tagged (data | log2(capacity)) cell.
    // Synchronizes-with grow()'s release-store, ensuring any element copies
    // performed into the new buffer are visible if we observe the new tag.
    // If we observe the old tag, the old buffer is still alive (retained in
    // buffers_) so the memcpy below is safe.
    uintptr_t aw = load_active_acquire();
    T* data = unpack_data(aw);
    size_t mask = unpack_mask(aw);
    // Racy read - safe because T is trivially copyable and destructible. If
    // we lose the CAS below, the caller will ignore the out value.
    load_item(out, data + (static_cast<size_t>(t) & mask));
    // Full-word CAS: increment top, leave bottom unchanged. This will fail
    // if the owner has modified bottom (push or pop) since our snapshot,
    // which is what lets pop() always win the last-element race without
    // having to CAS itself - pop's bottom decrement is enough to invalidate
    // any in-flight stealer's expected value here. The cost is that steal()
    // may now spuriously fail when concurrent push()/pop() are happening.
    uint64_t desired = static_cast<uint64_t>(static_cast<uint32_t>(s)) |
                       (static_cast<uint64_t>(t + 1u) << 32);
    if (!state_full().compare_exchange_strong(
          s, desired, std::memory_order_seq_cst, std::memory_order_relaxed
        )) {
      if (retryCount == 3) {
        return false;
      }
      for (size_t i = 0; i < retryCount; ++i) {
        TMC_CPU_PAUSE();
      }
      ++retryCount;
      goto RETRY;
    }
    return true;
  }

  // Approximate size. Safe to call from any thread.
  size_t size_approx() const {
    uint64_t s = state_full().load(std::memory_order_relaxed);
    // (bottom - top) is computed in uint32_t (defined wraparound) and
    // reinterpreted as int32_t for the signed sign check.
    int32_t diff = static_cast<int32_t>(unpack_bottom(s) - unpack_top(s));
    return diff > 0 ? static_cast<size_t>(diff) : 0;
  }

  // Approximate emptiness. Safe to call from any thread.
  bool empty() const {
    uint64_t s = state_full().load(std::memory_order_relaxed);
    // (bottom - top) is computed in uint32_t (defined wraparound) and
    // reinterpreted as int32_t for the signed sign check.
    return static_cast<int32_t>(unpack_bottom(s) - unpack_top(s)) <= 0;
  }
};

} // namespace detail
} // namespace tmc
