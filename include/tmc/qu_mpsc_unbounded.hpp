// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

// Provides tmc::qu_mpsc_unbounded, an async MPSC unbounded linearizable queue.
// All enqueue and dequeue operations are zero-copy.

// Uses a similar fetch-add slot acquisition scheme + linked list of blocks like
// tmc::channel, but optimized for a single consumer.

// Instead of hazard pointers, uses a quiescent-state based reclamation scheme:
// 1. Producers reserve tickets with write_offset, then load write_block_hint.
//    If the hint is past their reservation, they fall back to write_block.
// 2. Producers may advance write_block_hint forward after finding their block.
// 3. The consumer enters a new block and publishes it as the new write_block.
// 4. The consumer snapshots write_offset as the reclaim cutoff.
// 5. Once read_offset reaches that cutoff, producers that may have observed the
//    old write_block or write_block_hint are done, so old blocks can be
//    recycled.

#include "tmc/detail/compat.hpp"
#include "tmc/detail/concepts_awaitable.hpp"
#include "tmc/detail/qu_storage.hpp"
#include "tmc/detail/thread_locals.hpp"
#include "tmc/ex_any.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <coroutine>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace tmc {
struct qu_mpsc_unbounded_default_config {
  /// If true, enables the suspending `pull()` operation. This costs each
  /// producer an additional locked operation to check for a waiting consumer.
  static inline constexpr bool ConsumerCanSuspend = true;

  /// The number of elements that can be stored in each block in the
  /// qu_mpsc_unbounded linked list.
  static inline constexpr size_t BlockSize = 4096;

  /// At level 0, queue elements will be padded up to the next increment of 64
  /// bytes. This reduces false sharing between neighboring elements.
  /// At level 1, no padding will be applied.
  static inline constexpr size_t PackingLevel = 0;

  /// If true, the first storage block will be a member of the qu_mpsc_unbounded
  /// object (instead of dynamically allocated). Subsequent storage blocks are
  /// always dynamically allocated.
  static inline constexpr bool EmbedFirstBlock = false;
};

/// Status code returned by qu_mpsc_unbounded.try_pull().status()
enum class qu_mpsc_unbounded_err { OK, EMPTY, CLOSED };

template <typename T, typename Config = tmc::qu_mpsc_unbounded_default_config>
class qu_mpsc_unbounded {
  static_assert(std::is_nothrow_destructible_v<T>);

  static inline constexpr size_t BlockSize = Config::BlockSize;
  static inline constexpr size_t BlockSizeMask = BlockSize - 1;
  static inline constexpr bool ConsumerCanSuspend = Config::ConsumerCanSuspend;
  static_assert(
    BlockSize && ((BlockSize & (BlockSize - 1)) == 0),
    "BlockSize must be a power of 2"
  );

  // Ensure that the subtraction of unsigned offsets always results in a value
  // that can be represented as a signed integer.
  static_assert(
    BlockSize <= (TMC_ONE_BIT << (TMC_PLATFORM_BITS - 1)),
    "BlockSize must not be larger than half the max value that can be "
    "represented by a platform word"
  );

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

  static_assert(alignof(consumer_base) >= 4);
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

    // Attempts to install Cons as a waiting consumer.
    // Returns the previous flags value: 0 (nullptr) means Cons is now
    // installed and the consumer should suspend; DATA_BIT means a producer
    // already published data here; CLOSED_BIT means close() already published
    // a CLOSED sentinel here.
    uintptr_t try_wait(consumer_base* Cons) noexcept {
      return reinterpret_cast<uintptr_t>(
        flags.exchange(static_cast<void*>(Cons), std::memory_order_acq_rel)
      );
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

    // Publishes a CLOSED sentinel at this slot. If a consumer was already
    // waiting, its consumer_base pointer is returned so the caller can wake it.
    // Used only by close() to mark the cutoff slot.
    consumer_base* set_closed_or_get_waiting_consumer() noexcept {
      void* prev = flags.exchange(
        reinterpret_cast<void*>(CLOSED_BIT), std::memory_order_acq_rel
      );
      if (reinterpret_cast<uintptr_t>(prev) < 4) {
        return nullptr;
      }
      return static_cast<consumer_base*>(prev);
    }

    bool is_data_waiting() noexcept {
      void* f = flags.load(std::memory_order_acquire);
      return DATA_BIT == reinterpret_cast<uintptr_t>(f);
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

  struct data_block {
    std::atomic<size_t> offset;
    std::atomic<data_block*> next;
    std::array<element, BlockSize> values;

    void reset_values() noexcept {
      for (size_t i = 0; i < BlockSize; ++i) {
        values[i].reset();
      }
    }

    data_block(size_t Offset) noexcept {
      offset.store(Offset, std::memory_order_relaxed);
      next.store(nullptr, std::memory_order_relaxed);
      reset_values();
    }

    data_block() noexcept : data_block(0) {}
  };

  char pad0[TMC_CACHE_LINE_SIZE];
  // Producer hot fields
  std::atomic<size_t> write_offset;
  // closed is read by producers on every post() (acquire load). It sits with
  // write_offset because producers RMW write_offset and immediately check
  // closed; both are on the same cacheline so the load is essentially free.
  std::atomic<bool> closed;
  std::atomic<data_block*> write_block_hint;
  std::atomic<data_block*> write_block;
  // Cold close-related fields: only read by producers on the close slow path,
  // and only written once by close() itself.
  std::atomic<size_t> write_closed_at;
  std::atomic<bool> closed_ready;
  char pad1[TMC_CACHE_LINE_SIZE - sizeof(size_t)];
  // Read and written only by consumer
  size_t read_offset;
  data_block* read_block;
  data_block* pending_reclaim_old_head;
  data_block* pending_reclaim_new_head;
  size_t pending_reclaim_cutoff;
  data_block* head_block;
  data_block* tail_block;
  char pad2[TMC_CACHE_LINE_SIZE - sizeof(void*)];

  struct empty {};
  using EmbeddedBlock =
    std::conditional_t<Config::EmbedFirstBlock, data_block, empty>;
  TMC_NO_UNIQUE_ADDRESS EmbeddedBlock embedded_block;

public:
  class aw_pull;

  /// A zero-copy handle to an object in the queue's storage. The object is
  /// exclusively available to this handle. When this handle is destroyed, the
  /// queued object will be destroyed and the queue slot will be freed for
  /// reuse. Returned by `try_pull()`.
  ///
  /// The status of the pull is exposed via `status()`:
  /// `qu_mpsc_unbounded_err::OK` if a value is held, `EMPTY` if no value was
  /// available, or `CLOSED` if the queue has been closed and drained.
  class try_pull_zc_scope {
    friend qu_mpsc_unbounded;
    qu_mpsc_unbounded* queue;
    element* elem;
    data_block* block;
    size_t idx;
    tmc::qu_mpsc_unbounded_err err;

    try_pull_zc_scope(
      qu_mpsc_unbounded* Queue, element* Elem, data_block* Block, size_t Idx
    ) noexcept
        : queue{Queue}, elem{Elem}, block{Block}, idx{Idx},
          err{tmc::qu_mpsc_unbounded_err::OK} {}

    explicit try_pull_zc_scope(tmc::qu_mpsc_unbounded_err Err) noexcept
        : queue{nullptr}, elem{nullptr}, block{nullptr}, idx{0}, err{Err} {}

  public:
    /// Constructs an empty scope (status EMPTY). Evaluates to false when
    /// converted to bool.
    try_pull_zc_scope() noexcept
        : queue{nullptr}, elem{nullptr}, block{nullptr}, idx{0},
          err{tmc::qu_mpsc_unbounded_err::EMPTY} {}

    try_pull_zc_scope(const try_pull_zc_scope&) = delete;
    try_pull_zc_scope& operator=(const try_pull_zc_scope&) = delete;

    try_pull_zc_scope(try_pull_zc_scope&& Other) noexcept
        : queue{Other.queue}, elem{Other.elem}, block{Other.block},
          idx{Other.idx}, err{Other.err} {
      Other.elem = nullptr;
      Other.err = tmc::qu_mpsc_unbounded_err::EMPTY;
    }

    try_pull_zc_scope& operator=(try_pull_zc_scope&& Other) noexcept {
      if (this != &Other) {
        if (elem != nullptr) {
          queue->finish_read(elem, block, idx);
          elem = nullptr;
        }
        queue = Other.queue;
        elem = Other.elem;
        block = Other.block;
        idx = Other.idx;
        err = Other.err;
        Other.elem = nullptr;
        Other.err = tmc::qu_mpsc_unbounded_err::EMPTY;
      }
      return *this;
    }

    /// Returns true if this scope holds a value from the queue (status == OK).
    explicit operator bool() const noexcept { return elem != nullptr; }

    /// Returns true if this scope holds a value from the queue (status == OK).
    bool has_value() const noexcept { return elem != nullptr; }

    /// Returns the status of this pull: OK, EMPTY, or CLOSED.
    tmc::qu_mpsc_unbounded_err status() const noexcept { return err; }

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
        queue->finish_read(elem, block, idx);
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
    friend qu_mpsc_unbounded;
    qu_mpsc_unbounded* queue;
    element* elem;
    data_block* block;
    size_t idx;

    pull_zc_scope(
      qu_mpsc_unbounded* Queue, element* Elem, data_block* Block, size_t Idx
    ) noexcept
        : queue{Queue}, elem{Elem}, block{Block}, idx{Idx} {}

  public:
    /// Constructs an empty scope. Evaluates to false when converted to bool.
    pull_zc_scope() noexcept
        : queue{nullptr}, elem{nullptr}, block{nullptr}, idx{0} {}

    pull_zc_scope(const pull_zc_scope&) = delete;
    pull_zc_scope& operator=(const pull_zc_scope&) = delete;

    pull_zc_scope(pull_zc_scope&& Other) noexcept
        : queue{Other.queue}, elem{Other.elem}, block{Other.block},
          idx{Other.idx} {
      Other.elem = nullptr;
    }

    /// Returns true if this scope holds a value from the queue.
    explicit operator bool() const noexcept { return elem != nullptr; }

    /// Returns true if this scope holds a value from the queue.
    bool has_value() const noexcept { return elem != nullptr; }

    pull_zc_scope& operator=(pull_zc_scope&& Other) noexcept {
      if (this != &Other) {
        if (elem != nullptr) {
          queue->finish_read(elem, block, idx);
          elem = nullptr;
        }
        queue = Other.queue;
        elem = Other.elem;
        block = Other.block;
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
        queue->finish_read(elem, block, idx);
        elem = nullptr;
      }
    }
  };

  qu_mpsc_unbounded() noexcept {
    data_block* block;
    if constexpr (Config::EmbedFirstBlock) {
      block = &embedded_block;
    } else {
      block = new data_block(0);
    }
    head_block = block;
    write_block.store(block, std::memory_order_relaxed);
    write_block_hint.store(block, std::memory_order_relaxed);
    tail_block = block;
    write_offset.store(0, std::memory_order_relaxed);
    closed.store(false, std::memory_order_relaxed);
    closed_ready.store(false, std::memory_order_relaxed);
    write_closed_at.store(0, std::memory_order_relaxed);
    read_offset = 0;
    read_block = block;
    pending_reclaim_old_head = nullptr;
    pending_reclaim_new_head = nullptr;
    pending_reclaim_cutoff = 0;
    tmc::detail::memory_barrier();
  }

private:
  static inline bool circular_less_than(size_t a, size_t b) noexcept {
    return a - b > (TMC_ONE_BIT << (TMC_PLATFORM_BITS - 1));
  }

  void reclaim_blocks(data_block* OldHead, data_block* NewHead) noexcept {
    // Reset blocks and move them to the tail of the list in groups of 4.
    while (true) {
      std::array<data_block*, 4> unlinked;
      size_t unlinkedCount = 0;
      for (; unlinkedCount < unlinked.size(); ++unlinkedCount) {
        if (OldHead == NewHead) {
          break;
        }
        unlinked[unlinkedCount] = OldHead;
        OldHead = OldHead->next.load(std::memory_order_acquire);
      }
      if (unlinkedCount == 0) {
        break;
      }

      for (size_t i = 0; i < unlinkedCount; ++i) {
        unlinked[i]->reset_values();
      }

      data_block* tailBlock = tail_block;
      data_block* next = tailBlock->next.load(std::memory_order_acquire);

      // Iterate forward in case tailBlock is part of unlinked.
      while (next != nullptr) {
        tailBlock = next;
        next = tailBlock->next.load(std::memory_order_acquire);
      }
      // Actually unlink the blocks from the head of the queue.
      // They stay linked to each other.
      unlinked[unlinkedCount - 1]->next.store(
        nullptr, std::memory_order_release
      );

      while (true) {
        // Update their offsets to the end of the queue.
        size_t boff =
          tailBlock->offset.load(std::memory_order_relaxed) + BlockSize;
        for (size_t i = 0; i < unlinkedCount; ++i) {
          unlinked[i]->offset.store(boff, std::memory_order_relaxed);
          boff += BlockSize;
        }

        // Re-link the tail of the queue to the head of the unlinked blocks.
        if (tailBlock->next.compare_exchange_strong(
              next, unlinked[0], std::memory_order_acq_rel,
              std::memory_order_acquire
            )) {
          break;
        }

        // Tail was out of date, find the new tail.
        while (next != nullptr) {
          tailBlock = next;
          next = tailBlock->next.load(std::memory_order_acquire);
        }
      }

      tail_block = unlinked[unlinkedCount - 1];
    }
  }

  // Given idx and a starting block, advance it until the block containing idx
  // is found.
  static inline data_block* find_block(data_block* Block, size_t Idx) noexcept {
    size_t offset = Block->offset.load(std::memory_order_relaxed);
    size_t targetOffset = Idx & ~BlockSizeMask;
    // Find or allocate the associated block
    while (offset != targetOffset) {
      data_block* next = Block->next.load(std::memory_order_acquire);
      if (next == nullptr) {
        data_block* newBlock = new data_block(offset + BlockSize);
        if (Block->next.compare_exchange_strong(
              next, newBlock, std::memory_order_acq_rel,
              std::memory_order_acquire
            )) {
          next = newBlock;
        } else {
          delete newBlock;
        }
      }
      Block = next;
      offset += BlockSize;
      assert(Block->offset.load(std::memory_order_relaxed) == offset);
    }

    assert(
      Idx >= Block->offset.load(std::memory_order_relaxed) &&
      Idx <= Block->offset.load(std::memory_order_relaxed) + BlockSize - 1
    );
    return Block;
  }

  static inline bool block_before(data_block* A, data_block* B) noexcept {
    return circular_less_than(
      A->offset.load(std::memory_order_relaxed),
      B->offset.load(std::memory_order_relaxed)
    );
  }

  void advance_write_block_hint_at_least(
    data_block* Current, data_block* Target
  ) noexcept {
    while (block_before(Current, Target)) {
      if (write_block_hint.compare_exchange_weak(
            Current, Target, std::memory_order_seq_cst,
            std::memory_order_seq_cst
          )) {
        return;
      }
    }
  }

  void advance_write_block_hint_at_least(data_block* Target) noexcept {
    advance_write_block_hint_at_least(
      write_block_hint.load(std::memory_order_seq_cst), Target
    );
  }

  data_block* get_mpsc_write_start_block(size_t Idx) noexcept {
    data_block* block = write_block_hint.load(std::memory_order_seq_cst);
    if (!circular_less_than(
          block->offset.load(std::memory_order_relaxed), 1 + Idx
        )) {
      // A later producer may have advanced the hint past this producer's
      // earlier reservation. Fall back to the consumer-managed reclaim
      // frontier, which cannot advance past an unproduced reservation.
      block = write_block.load(std::memory_order_seq_cst);
      assert(circular_less_than(
        block->offset.load(std::memory_order_relaxed), 1 + Idx
      ));
    }
    return block;
  }

  bool try_finish_pending_reclaim() noexcept {
    if (pending_reclaim_old_head == nullptr) {
      return false;
    }

    // The pending blocks were removed from the producer-visible write head
    // before pending_reclaim_cutoff was read from write_offset. Any producer
    // that can still be walking from the old write head must therefore have a
    // reservation before this cutoff. Once the single consumer reaches the
    // cutoff, those producers have published their elements and no longer touch
    // the old blocks.
    if (circular_less_than(read_offset, pending_reclaim_cutoff)) {
      return false;
    }

    data_block* oldHead = pending_reclaim_old_head;
    data_block* newHead = pending_reclaim_new_head;
    head_block = newHead;
    pending_reclaim_old_head = nullptr;
    pending_reclaim_new_head = nullptr;
    reclaim_blocks(oldHead, newHead);
    return true;
  }

  void try_start_reclaim(data_block* NewHead) noexcept {
    if (pending_reclaim_old_head != nullptr) {
      return;
    }

    data_block* oldHead = head_block;
    size_t newHeadOffset = read_offset & ~BlockSizeMask;
    assert(NewHead->offset.load(std::memory_order_relaxed) == newHeadOffset);
    size_t oldOff = oldHead->offset.load(std::memory_order_relaxed);
    if (!circular_less_than(oldOff, newHeadOffset)) {
      return;
    }

    // This seq_cst write_block store, seq_cst write_block_hint advancement, and
    // the following seq_cst write_offset load form the cutoff protocol with
    // producers, which do a seq_cst fetch_add before a seq_cst load of either
    // write_block_hint or write_block. A producer that observes the old
    // write_block or write_block_hint must have a reservation included in the
    // cutoff.
    write_block.store(NewHead, std::memory_order_seq_cst);
    advance_write_block_hint_at_least(NewHead);
    pending_reclaim_cutoff = write_offset.load(std::memory_order_seq_cst);
    pending_reclaim_old_head = oldHead;
    pending_reclaim_new_head = NewHead;
  }

  void try_reclaim_blocks(data_block* NewHead) noexcept {
    try_finish_pending_reclaim();
    try_start_reclaim(NewHead);
    try_finish_pending_reclaim();
  }

  // Idx will be initialized by this function.
  // Returns nullptr if the queue is closed and Idx is past the close cutoff;
  // the caller must not write to the slot in that case.
  element* get_write_ticket(size_t& Idx) noexcept {
    // seq_cst is needed here so the reader can order its write_block update
    // and subsequent write_offset load against the producer's reservation and
    // write_block load. It also forms the producer side of the close protocol:
    // close()'s release store to `closed` is sequenced-before its own seq_cst
    // fetch_add on write_offset; if our fetch_add is mod-order after close's,
    // the RMW chain makes the release store of `closed` happens-before our
    // subsequent acquire load below.
    Idx = write_offset.fetch_add(1, std::memory_order_seq_cst);

    if (closed.load(std::memory_order_acquire)) [[unlikely]] {
      // Wait for write_closed_at to be published by close().
      while (!closed_ready.load(std::memory_order_acquire)) {
        TMC_CPU_PAUSE();
      }
      if (circular_less_than(
            write_closed_at.load(std::memory_order_acquire), 1 + Idx
          )) {
        return nullptr;
      }
    }

    data_block* observed = get_mpsc_write_start_block(Idx);

    assert(circular_less_than(
      observed->offset.load(std::memory_order_relaxed), 1 + Idx
    ));

    data_block* block = find_block(observed, Idx);
    advance_write_block_hint_at_least(observed, block);
    element* elem = &block->values[Idx & BlockSizeMask];
    return elem;
  }

  element* get_read_ticket(size_t& Idx, data_block*& Block) noexcept {
    Idx = read_offset;
    Block = read_block;

    assert(
      circular_less_than(Block->offset.load(std::memory_order_relaxed), 1 + Idx)
    );

    Block = find_block(Block, Idx);
    return &Block->values[Idx & BlockSizeMask];
  }

  void finish_read(element* Elem, data_block* Block, size_t Idx) noexcept {
    Elem->data.destroy();
    read_offset = Idx + 1;
    // Only try to reclaim once the consumer has entered a new block. In MPSC
    // mode this is where the producer-visible write head may advance; in SPSC
    // mode the producer already advanced before making this block-start element
    // visible, so old blocks can be reclaimed immediately.
    if ((Idx & BlockSizeMask) == 0) {
      read_block = Block;
      try_reclaim_blocks(Block);
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

  void notify_consumer(consumer_base* Cons) noexcept {
    if (Cons != nullptr) {
      tmc::detail::post_checked(
        Cons->continuation_executor, std::move(Cons->continuation), Cons->prio
      );
    }
  }

  // StartIdx and EndIdx will be initialized by this function.
  // Count must be non-zero (enforced by the caller).
  // Returns nullptr if the queue is closed and the reservation is entirely
  // past the close cutoff; the caller must not write any of the slots in
  // that case. Because close() takes a single fetch_add cutoff, a bulk
  // reservation cannot straddle the cutoff: it is either all pre-close or
  // all post-close in the seq_cst total order on write_offset.
  data_block* get_write_ticket_bulk(
    size_t Count, size_t& StartIdx, size_t& EndIdx
  ) noexcept {
    // seq_cst here serves the same purpose as in get_write_ticket: it orders
    // the reader's reclaim cutoff AND forms the producer side of the close
    // protocol (RMW-chain from close()'s release store to `closed`).
    StartIdx = write_offset.fetch_add(Count, std::memory_order_seq_cst);
    EndIdx = StartIdx + Count;

    if (closed.load(std::memory_order_acquire)) [[unlikely]] {
      while (!closed_ready.load(std::memory_order_acquire)) {
        TMC_CPU_PAUSE();
      }
      if (circular_less_than(
            write_closed_at.load(std::memory_order_acquire), 1 + StartIdx
          )) {
        return nullptr;
      }
    }

    data_block* observed = get_mpsc_write_start_block(StartIdx);

    assert(circular_less_than(
      observed->offset.load(std::memory_order_relaxed), 1 + StartIdx
    ));

    // Ensure all blocks for the operation are allocated and available.
    data_block* startBlock = find_block(observed, StartIdx);
    data_block* endBlock = find_block(startBlock, EndIdx - 1);
    advance_write_block_hint_at_least(observed, endBlock);
    return startBlock;
  }

public:
  /// If the queue is open, this will always return true, indicating that an
  /// object of type T was enqueued by in-place construction, forwarding
  /// `ConstructArgs` to T's constructor.
  ///
  /// If the queue is closed, this will return false, and the object will not
  /// be enqueued.
  ///
  /// If a consumer is currently suspended waiting for a value, it will be
  /// resumed once the the value is enqueued.
  template <typename... Args> bool post(Args&&... ConstructArgs) noexcept {
    // Implementing handling for throwing construction is not possible with the
    // current design. This assert will also fire if no matching constructor can
    // be found for the provided arguments.
    static_assert(std::is_nothrow_constructible_v<T, Args&&...>);

    // Get write ticket and associated block.
    size_t idx;
    element* elem = get_write_ticket(idx);
    if (elem == nullptr) [[unlikely]] {
      return false;
    }

    consumer_base* cons =
      write_element(elem, static_cast<Args&&>(ConstructArgs)...);
    notify_consumer(cons);
    return true;
  }

  /// If the queue is open, this will always return true, indicating that
  /// Count elements, starting from the Begin iterator, were enqueued.
  ///
  /// If the queue is closed, this will return false, and no items
  /// will be enqueued.
  ///
  /// If a consumer is currently suspended waiting for a value, it will be
  /// resumed once the the values are enqueued.
  ///
  /// Each item is moved (not copied) from the iterator into the queue.
  ///
  /// The closed check is performed first, then space is pre-allocated, then all
  /// Count items are moved into the queue. Thus, there cannot be a partial
  /// success - either all or none of the items will be moved.
  template <typename It> bool post_bulk(It&& Items, size_t Count) noexcept {
    // Implementing handling for throwing construction is not possible with the
    // current design. This assert will also fire if no matching constructor can
    // be found for the iterator's dereferenced value.
    static_assert(
      std::is_nothrow_constructible_v<T, decltype(std::move(*Items))>
    );

    if (Count == 0) [[unlikely]] {
      return true;
    }

    // Get write ticket and associated block.
    size_t startIdx, endIdx;
    data_block* block = get_write_ticket_bulk(Count, startIdx, endIdx);
    if (block == nullptr) [[unlikely]] {
      return false;
    }

    size_t idx = startIdx;
    while (idx < endIdx) {
      element* elem = &block->values[idx & BlockSizeMask];

      TMC_DISABLE_WARNING_PESSIMIZING_MOVE_BEGIN
      consumer_base* waiting = write_element(elem, std::move(*Items));
      TMC_DISABLE_WARNING_PESSIMIZING_MOVE_END
      notify_consumer(waiting);

      ++Items;
      ++idx;
      if ((idx & BlockSizeMask) == 0) {
        block = block->next.load(std::memory_order_acquire);
        // all blocks should have been preallocated for [startIdx, endIdx)
        assert(block != nullptr || idx >= endIdx);
      }
    }
    return true;
  }

  /// Calculates the number of elements via `size_t Count = End - Begin;`
  ///
  /// If the queue is open, this will always return true, indicating that
  /// Count elements, starting from the Begin iterator, were enqueued.
  ///
  /// If the queue is closed, this will return false, and no items
  /// will be enqueued.
  ///
  /// If a consumer is currently suspended waiting for a value, it will be
  /// resumed once the the values are enqueued.
  ///
  /// Each item is moved (not copied) from the iterator into the queue.
  ///
  /// The closed check is performed first, then space is pre-allocated, then all
  /// Count items are moved into the queue. Thus, there cannot be a partial
  /// success - either all or none of the items will be moved.
  template <typename It> bool post_bulk(It&& Begin, It&& End) noexcept {
    // Implementing handling for throwing construction is not possible with the
    // current design. This assert will also fire if no matching constructor can
    // be found for the iterator's dereferenced value.
    static_assert(
      std::is_nothrow_constructible_v<T, decltype(std::move(*Begin))>
    );
    return post_bulk(
      static_cast<It&&>(Begin), static_cast<size_t>(End - Begin)
    );
  }

  /// Calculates the number of elements via
  /// `size_t Count = Range.end() - Range.begin();`
  ///
  /// If the queue is open, this will always return true, indicating that
  /// Count elements from the beginning of the range were enqueued.
  ///
  /// If the queue is closed, this will return false, and no items
  /// will be enqueued.
  ///
  /// If a consumer is currently suspended waiting for a value, it will be
  /// resumed once the the values are enqueued.
  ///
  /// Each item is moved (not copied) from the iterator into the queue.
  ///
  /// The closed check is performed first, then space is pre-allocated, then all
  /// Count items are moved into the queue. Thus, there cannot be a partial
  /// success - either all or none of the items will be moved.
  template <typename Range> bool post_bulk(Range&& R) noexcept {
    // Implementing handling for throwing construction is not possible with the
    // current design. This assert will also fire if no matching constructor can
    // be found for the iterator's dereferenced value.
    static_assert(std::is_nothrow_constructible_v<
                  T, decltype(std::move(*static_cast<Range&&>(R).begin()))>);
    auto begin = static_cast<Range&&>(R).begin();
    auto end = static_cast<Range&&>(R).end();
    return post_bulk(begin, static_cast<size_t>(end - begin));
  }

private:
  // Performs the common close work and returns the waiting consumer (if any)
  // that needs to be woken. Returns nullptr if the queue was already closed
  // by another task, or if no consumer was waiting at the cutoff slot.
  consumer_base* close_get_waiting_consumer() noexcept {
    bool expected = false;
    if (!closed.compare_exchange_strong(
          expected, true, std::memory_order_release, std::memory_order_acquire
        )) {
      // Already closed by another task.
      return nullptr;
    }

    // We are the unique closer. The release store of `closed` above is
    // sequenced-before the following seq_cst fetch_add; any producer whose
    // own seq_cst fetch_add on write_offset is mod-order after ours will
    // synchronize-with us via the RMW chain and see `closed == true` on
    // its subsequent acquire load.
    size_t woff = write_offset.fetch_add(1, std::memory_order_seq_cst);
    write_closed_at.store(woff, std::memory_order_release);
    closed_ready.store(true, std::memory_order_release);

    // Publish the CLOSED sentinel at slot woff. The single consumer is
    // bounded to slot <= woff (slot > woff is unreachable since no producer
    // ever fills slot woff), so this is the only slot the consumer can be
    // stuck on. The exchange races with the consumer's try_wait(): exactly
    // one of the two RMWs on this element's flags goes first.
    //   - If the consumer's exchange goes first, it installed its
    //     consumer_base pointer; our exchange returns that pointer and we
    //     post the resumption.
    //   - If our exchange goes first, the slot now contains CLOSED_BIT;
    //     when the consumer later runs try_wait() it observes CLOSED_BIT
    //     and returns CLOSED without suspending.
    data_block* observed = get_mpsc_write_start_block(woff);
    data_block* block = find_block(observed, woff);
    element* elem = &block->values[woff & BlockSizeMask];
    consumer_base* cons = elem->set_closed_or_get_waiting_consumer();
    if (cons != nullptr) {
      // Setting elem to nullptr marks it as closed on the consumer side
      cons->elem = nullptr;
    }
    return cons;
  }

public:
  /// All future calls to `post()` and `post_bulk()` will immediately return
  /// false. Calls to `pull()` and `try_pull()` will continue to read data until
  /// all messages have been consumed, at which point all subsequent calls will
  /// immediately return an empty scope. If the queue was already empty, any
  /// waiting consumers will be awoken immediately and return an empty scope.
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
    size_t Idx = read_offset;
    data_block* block = find_block(read_block, Idx);
    element* elem = &block->values[Idx & BlockSizeMask];

    bool isEmpty = !elem->is_data_waiting();
    return isEmpty;
  }

  /// Returns a `pull_zc_scope` when awaited.
  class aw_pull final : private tmc::detail::AwaitTagNoGroupCoAwait {
    friend qu_mpsc_unbounded<T, Config>;

    qu_mpsc_unbounded& queue;

    aw_pull(qu_mpsc_unbounded& Queue) noexcept : queue(Queue) {}

    struct aw_pull_impl final {
      consumer_base base;
      qu_mpsc_unbounded& queue;
      data_block* block;
      size_t idx;

      aw_pull_impl(aw_pull& Parent) noexcept
          : base{tmc::detail::this_thread::executor(), nullptr,
                 tmc::detail::this_thread::this_task().prio, nullptr},
            queue{Parent.queue}, block{nullptr}, idx{0} {}

      bool await_ready() noexcept {
        element* myElem = queue.get_read_ticket(idx, block);
        base.elem = myElem;
        return myElem->poll() == DATA_BIT;
      }

      bool await_suspend(std::coroutine_handle<> Outer) noexcept {
        base.continuation = Outer;
        uintptr_t prev = base.elem->try_wait(&base);
        if (prev == CLOSED_BIT) [[unlikely]] {
          // Set the flags back to CLOSED_BIT so that future calls to pull() or
          // try_pull() see that it is still closed.
          base.elem->flags.store(
            reinterpret_cast<void*>(CLOSED_BIT), std::memory_order_release
          );
          base.elem = nullptr;
        }
        return prev == 0;
      }

      TMC_AWAIT_RESUME pull_zc_scope await_resume() noexcept {
        // If closed, base.elem was already set to nullptr in await_suspend or
        // close(). This marks the zc_scope as empty.
        return pull_zc_scope(&queue, base.elem, block, idx);
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
  ///   - qu_mpsc_unbounded_err::OK     - a value was dequeued
  ///   - qu_mpsc_unbounded_err::EMPTY  - no value is currently available
  ///   - qu_mpsc_unbounded_err::CLOSED - the queue has been closed and drained
  ///
  /// The returned scope's `has_value()` / `operator bool()` returns true if a
  /// value was dequeued, or false if the queue was empty or closed.
  ///
  /// This scope must be released before the next call to `try_pull()` or
  /// `pull()`. It must also be released before the queue is destroyed.
  try_pull_zc_scope try_pull() {
    size_t Idx;
    data_block* block;
    element* elem = get_read_ticket(Idx, block);

    auto s = elem->poll();
    if (s == DATA_BIT) {
      return try_pull_zc_scope(this, elem, block, Idx);
    }
    if (s == CLOSED_BIT) {
      return try_pull_zc_scope(tmc::qu_mpsc_unbounded_err::CLOSED);
    }
    return try_pull_zc_scope(tmc::qu_mpsc_unbounded_err::EMPTY);
  }

  /// Destroys the queue and any contained values that have not yet been
  /// consumed.
  ///
  /// Before destroying this, you must ensure:
  /// - No producer is currently calling post() or post_bulk().
  /// - No consumer is calling or suspended in pull() / try_pull().
  /// - No pull_zc_scope / try_pull_zc_scope from this queue is alive.
  /// - No other thread is calling any other member function.
  ///
  /// The recommended teardown sequence is:
  /// 1. Stop submitting new post() calls.
  /// 2. close() the queue.
  /// 3. Drain via pull() / try_pull() until CLOSED.
  /// 4. Ensure no further queue method calls will occur (e.g. by joining all
  ///    producer and consumer coroutines).
  /// 5. Destroy the queue.
  ~qu_mpsc_unbounded() {
    close();
    {
      // close() published a CLOSED sentinel at write_closed_at; that slot
      // holds no data, and no producer can fill any slot at or beyond it.
      size_t end = write_closed_at.load(std::memory_order_relaxed);
      size_t idx = read_offset;
      data_block* block = head_block;
      // If the consumer stopped consuming before the queue was drained, there
      // may be leftover data in the queue. Destroy it.
      while (circular_less_than(idx, end)) {
        block = find_block(block, idx);
        element* elem = &block->values[idx & BlockSizeMask];
        if (elem->is_data_waiting()) {
          elem->data.destroy();
        }
        ++idx;
      }
    }
    {
      data_block* block = head_block;
      while (block != nullptr) {
        data_block* next = block->next.load(std::memory_order_acquire);
        if constexpr (Config::EmbedFirstBlock) {
          if (block != &embedded_block) {
            delete block;
          }
        } else {
          delete block;
        }
        block = next;
      }
    }
  }

  qu_mpsc_unbounded(const qu_mpsc_unbounded&) = delete;
  qu_mpsc_unbounded& operator=(const qu_mpsc_unbounded&) = delete;
  qu_mpsc_unbounded(qu_mpsc_unbounded&&) = delete;
  qu_mpsc_unbounded& operator=(qu_mpsc_unbounded&&) = delete;
};

} // namespace tmc
