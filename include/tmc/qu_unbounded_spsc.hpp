// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

// Unbounded SPSC queue using linked list of blocks. Uses a similar fetch-add
// slot acquisition scheme to tmc::channel, but with various changes:
// - consumers are single-threaded, so read offset does not need to be atomic
// - queue cannot be closed
// - single consumer's offset is non-atomic
// - single producer can publish offset after writing data instead of before
// - single consumer can recycle blocks immediately after finishing them

#include "tmc/detail/compat.hpp"
#include "tmc/detail/concepts_awaitable.hpp"
#include "tmc/detail/thread_locals.hpp"
#include "tmc/ex_any.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <coroutine>
#include <type_traits>
#include <utility>

namespace tmc {
namespace detail {

// Allocates elements without constructing them, to be constructed later using
// placement new. T need not be default, copy, or move constructible.
// The caller must track whether the element exists, and manually invoke the
// destructor if necessary.
template <typename T> struct qu_unbounded_spsc_storage {
  union alignas(alignof(T)) {
    T value;
  };
#ifndef NDEBUG
  bool exists = false;
#endif

  qu_unbounded_spsc_storage() noexcept {}

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

  // Precondition: Other.value must exist
  qu_unbounded_spsc_storage(qu_unbounded_spsc_storage&& Other) noexcept {
    emplace(static_cast<T&&>(Other.value));
    Other.destroy();
  }
  qu_unbounded_spsc_storage&
  operator=(qu_unbounded_spsc_storage&& Other) noexcept {
    emplace(static_cast<T&&>(Other.value));
    Other.destroy();
    return *this;
  }

  // If data was present, the caller is responsible for destroying it.
#ifndef NDEBUG
  ~qu_unbounded_spsc_storage() { assert(!exists); }
#else
  ~qu_unbounded_spsc_storage()
    requires(std::is_trivially_destructible_v<T>)
  = default;
  ~qu_unbounded_spsc_storage()
    requires(!std::is_trivially_destructible_v<T>)
  {}
#endif

  qu_unbounded_spsc_storage(const qu_unbounded_spsc_storage&) = delete;
  qu_unbounded_spsc_storage&
  operator=(const qu_unbounded_spsc_storage&) = delete;
};

struct qu_unbounded_spsc_default_config {
  /// The number of elements that can be stored in each block in the
  /// qu_unbounded_spsc linked list.
  static inline constexpr size_t BlockSize = 4096;

  /// At level 0, queue elements will be padded up to the next increment of 64
  /// bytes. This reduces false sharing between neighboring elements.
  /// At level 1, no padding will be applied.
  /// SPSC defaults to packed since there is no cross-producer sharing.
  static inline constexpr size_t PackingLevel = 1;

  /// If true, the first storage block will be a member of the qu_unbounded_spsc
  /// object (instead of dynamically allocated). Subsequent storage blocks are
  /// always dynamically allocated.
  static inline constexpr bool EmbedFirstBlock = false;

  /// If true, enables the suspending pull() operation. This adds a CAS to the
  /// producer path to check for a waiting consumer.
  static inline constexpr bool ConsumerCanSuspend = true;
};

template <
  typename T, typename Config = tmc::detail::qu_unbounded_spsc_default_config>
class qu_unbounded_spsc {
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

  // Implementing handling for throwing construction is not possible with the
  // current design.
  static_assert(std::is_nothrow_move_constructible_v<T>);

private:
  struct consumer_base {
    tmc::ex_any* continuation_executor;
    std::coroutine_handle<> continuation;
    size_t prio;
  };

  class element_t {
    static inline constexpr uintptr_t DATA_BIT = TMC_ONE_BIT;
    std::atomic<void*> flags;

  public:
    tmc::detail::qu_unbounded_spsc_storage<T> data;

    static constexpr size_t UNPADLEN =
      sizeof(std::atomic<void*>) +
      sizeof(tmc::detail::qu_unbounded_spsc_storage<T>);
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

    // If this returns false, data is ready and consumer should not wait.
    bool try_wait(consumer_base* Cons) noexcept {
      void* prev =
        flags.exchange(static_cast<void*>(Cons), std::memory_order_acq_rel);
      return prev == nullptr;
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

    bool is_data_waiting() noexcept {
      void* f = flags.load(std::memory_order_acquire);
      return DATA_BIT == reinterpret_cast<uintptr_t>(f);
    }

    void reset() noexcept { flags.store(nullptr, std::memory_order_relaxed); }
  };

  using element = element_t;
  static_assert(Config::PackingLevel < 2);

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

  static_assert(std::atomic<size_t>::is_always_lock_free);
  static_assert(std::atomic<data_block*>::is_always_lock_free);
  static_assert(std::atomic<void*>::is_always_lock_free);

  char pad0[TMC_CACHE_LINE_SIZE - sizeof(size_t)];
  std::atomic<size_t> write_offset;
  char pad1[TMC_CACHE_LINE_SIZE - sizeof(size_t)];
  size_t read_offset;
  data_block* read_block;
  char pad2[TMC_CACHE_LINE_SIZE - sizeof(size_t) - sizeof(data_block*)];

  std::atomic<data_block*> write_block;
  data_block* head_block;
  data_block* tail_block;

  struct empty {};
  using EmbeddedBlock =
    std::conditional_t<Config::EmbedFirstBlock, data_block, empty>;
  TMC_NO_UNIQUE_ADDRESS EmbeddedBlock embedded_block;

public:
  class aw_pull;

  qu_unbounded_spsc() noexcept {
    data_block* block;
    if constexpr (Config::EmbedFirstBlock) {
      block = &embedded_block;
    } else {
      block = new data_block(0);
    }
    head_block = block;
    write_block.store(block, std::memory_order_relaxed);
    tail_block = block;
    write_offset.store(0, std::memory_order_relaxed);
    read_offset = 0;
    read_block = block;
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

  void try_reclaim_blocks(data_block* NewHead) noexcept {
    data_block* oldHead = head_block;
    size_t newHeadOffset = read_offset & ~BlockSizeMask;
    assert(NewHead->offset.load(std::memory_order_relaxed) == newHeadOffset);
    size_t oldOff = oldHead->offset.load(std::memory_order_relaxed);
    if (!circular_less_than(oldOff, newHeadOffset)) {
      return;
    }

    head_block = NewHead;
    reclaim_blocks(oldHead, NewHead);
  }

  // Idx will be initialized by this function
  element* get_write_ticket(size_t& Idx) noexcept {
    // In SPSC mode, write_offset is the committed write offset. The producer
    // takes the next index from it, advances its block cursor before making a
    // block-start element visible, and publishes write_offset after writing.
    Idx = write_offset.load(std::memory_order_relaxed);
    data_block* block = write_block.load(std::memory_order_relaxed);

    assert(
      circular_less_than(block->offset.load(std::memory_order_relaxed), 1 + Idx)
    );

    block = find_block(block, Idx);
    write_block.store(block, std::memory_order_relaxed);
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
    Elem->data.emplace(std::forward<Args>(ConstructArgs)...);
    if constexpr (ConsumerCanSuspend) {
      return Elem->set_data_ready_or_get_waiting_consumer();
    } else {
      Elem->set_data_ready();
      return nullptr;
    }
  }

  // StartIdx and EndIdx will be initialized by this function.
  // Count must be non-zero (enforced by the caller).
  data_block* get_write_ticket_bulk(
    size_t Count, size_t& StartIdx, size_t& EndIdx
  ) noexcept {
    // In SPSC mode, write_offset is published after all elements in the bulk
    // operation have been written.
    StartIdx = write_offset.load(std::memory_order_relaxed);
    EndIdx = StartIdx + Count;
    data_block* block = write_block.load(std::memory_order_relaxed);

    assert(circular_less_than(
      block->offset.load(std::memory_order_relaxed), 1 + StartIdx
    ));

    // Ensure all blocks for the operation are allocated and available.
    data_block* startBlock = find_block(block, StartIdx);
    find_block(startBlock, EndIdx - 1);
    write_block.store(startBlock, std::memory_order_relaxed);
    return startBlock;
  }

public:
  template <typename U> void post(U&& Val) noexcept {
    // Get write ticket and associated block.
    size_t idx;
    element* elem = get_write_ticket(idx);

    consumer_base* cons = write_element(elem, static_cast<U&&>(Val));
    write_offset.store(idx + 1, std::memory_order_release);
    if (cons != nullptr) {
      tmc::detail::post_checked(
        cons->continuation_executor, std::move(cons->continuation), cons->prio
      );
    }
  }

  template <typename It> void post_bulk(It&& Items, size_t Count) noexcept {
    if (Count == 0) [[unlikely]] {
      return;
    }

    // Get write ticket and associated block.
    size_t startIdx, endIdx;
    data_block* block = get_write_ticket_bulk(Count, startIdx, endIdx);

    size_t idx = startIdx;
    consumer_base* cons = nullptr;
    while (idx < endIdx) {
      element* elem = &block->values[idx & BlockSizeMask];

      TMC_DISABLE_WARNING_PESSIMIZING_MOVE_BEGIN
      consumer_base* waiting = write_element(elem, std::move(*Items));
      TMC_DISABLE_WARNING_PESSIMIZING_MOVE_END
      if (waiting != nullptr) {
        assert(cons == nullptr);
        cons = waiting;
      }

      ++Items;
      ++idx;
      if ((idx & BlockSizeMask) == 0) {
        block = block->next.load(std::memory_order_acquire);
        // all blocks should have been preallocated for [startIdx, endIdx)
        assert(block != nullptr || idx >= endIdx);
        if (idx < endIdx) {
          write_block.store(block, std::memory_order_relaxed);
        }
      }
    }
    write_offset.store(endIdx, std::memory_order_release);
    if (cons != nullptr) {
      tmc::detail::post_checked(
        cons->continuation_executor, std::move(cons->continuation), cons->prio
      );
    }
  }

  // Only safe to call from the single consumer.
  bool empty() {
    size_t Idx = read_offset;
    data_block* block = find_block(read_block, Idx);
    element* elem = &block->values[Idx & BlockSizeMask];

    bool isEmpty = !elem->is_data_waiting();
    return isEmpty;
  }

  class aw_pull final : private tmc::detail::AwaitTagNoGroupCoAwait {
    friend qu_unbounded_spsc<T, Config>;

    qu_unbounded_spsc& queue;

    aw_pull(qu_unbounded_spsc& Queue) noexcept : queue(Queue) {}

    struct aw_pull_impl final {
      consumer_base base;
      qu_unbounded_spsc& queue;
      element* elem;
      data_block* block;
      size_t idx;

      aw_pull_impl(aw_pull& Parent) noexcept
          : base{tmc::detail::this_thread::executor(), nullptr,
                 tmc::detail::this_thread::this_task().prio},
            queue{Parent.queue}, elem{nullptr}, block{nullptr}, idx{0} {}

      bool await_ready() noexcept {
        elem = queue.get_read_ticket(idx, block);
        return elem->is_data_waiting();
      }

      bool await_suspend(std::coroutine_handle<> Outer) noexcept {
        base.continuation = Outer;
        if (!elem->try_wait(&base)) {
          // data became ready during our RMW cycle
          return false;
        }
        return true;
      }

      TMC_AWAIT_RESUME T await_resume() noexcept {
        T result(std::move(elem->data.value));
        queue.finish_read(elem, block, idx);
        return result;
      }
    };

  public:
    aw_pull_impl operator co_await() && noexcept { return aw_pull_impl(*this); }
  };

  /// Returns a T.
  ///
  /// May suspend until a value is available. qu_unbounded_spsc has no close
  /// operation, so callers that need to terminate a consumer loop should post a
  /// sentinel value.
  [[nodiscard(
    "You must co_await pull(). To poll from a non-coroutine function, use "
    "try_pull()."
  )]] aw_pull
  pull() noexcept
    requires(ConsumerCanSuspend)
  {
    static_assert(std::is_nothrow_move_constructible_v<T>);
    return aw_pull(*this);
  }

  bool try_pull(T& output) {
    size_t Idx;
    data_block* block;
    element* elem = get_read_ticket(Idx, block);

    if (elem->is_data_waiting()) {
      // Data is already ready here.
      output = std::move(elem->data.value);
      finish_read(elem, block, Idx);
      return true;
    }
    return false;
  }

  ~qu_unbounded_spsc() {
    {
      size_t woff = write_offset.load(std::memory_order_relaxed);
      size_t idx = read_offset;
      data_block* block = head_block;
      while (circular_less_than(idx, woff)) {
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

  qu_unbounded_spsc(const qu_unbounded_spsc&) = delete;
  qu_unbounded_spsc& operator=(const qu_unbounded_spsc&) = delete;
  qu_unbounded_spsc(qu_unbounded_spsc&&) = delete;
  qu_unbounded_spsc& operator=(qu_unbounded_spsc&&) = delete;
};

} // namespace detail
} // namespace tmc
