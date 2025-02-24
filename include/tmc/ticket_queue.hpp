// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

// Provides tmc::ticket_queue, an async MPMC queue of unbounded size.

// Producers may suspend when calling co_await push() if the queue is full.
// Consumers retrieve values in FIFO order with pull().
// Consumers retrieve values in LIFO order with pop();

#include "tmc/detail/compat.hpp"
#include "tmc/detail/concepts.hpp"
#include "tmc/detail/thread_locals.hpp"
#include "tmc/detail/tiny_lock.hpp"

#include <array>
#include <atomic>
#include <coroutine>
#include <cstdio>
#include <variant>

// TODO mask with highest bit set to 0 when comparing indexes

namespace tmc {
enum queue_error { OK = 0, CLOSED = 1, EMPTY = 2, FULL = 3 };

template <typename T, size_t Size = 256> class ticket_queue {
  // TODO allow size == 1 (empty queue that always directly transfers)
  static_assert(Size > 1);

public:
  // Round Capacity up to next power of 2
  static constexpr size_t Capacity = [](size_t x) consteval -> size_t {
    --x;
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    for (std::size_t i = 1; i < sizeof(size_t); i <<= 1) {
      x |= x >> (i << 3);
    }
    ++x;
    return x;
  }(Size);
  static constexpr size_t CapacityMask = Capacity - 1;

  struct aw_ticket_queue_waiter_base {
    T t; // by value for now, possibly zero-copy / by ref in future
    ticket_queue& queue;
    queue_error err;
    tmc::detail::type_erased_executor* continuation_executor;
    std::coroutine_handle<> continuation;
    size_t prio;
    template <typename U>
    aw_ticket_queue_waiter_base(U&& u, ticket_queue& q)
        : t(std::forward<U>(u)), queue(q), err{OK},
          continuation_executor{tmc::detail::this_thread::executor},
          continuation{nullptr},
          prio(tmc::detail::this_thread::this_task.prio) {}

    aw_ticket_queue_waiter_base(ticket_queue& q)
        : queue(q), err{OK},
          continuation_executor{tmc::detail::this_thread::executor},
          continuation{nullptr},
          prio(tmc::detail::this_thread::this_task.prio) {}
  };

  template <bool Must> class aw_ticket_queue_push;
  template <bool Must> class aw_ticket_queue_pull;

  // May return a value or CLOSED
  aw_ticket_queue_pull<false> pull() {
    return aw_ticket_queue_pull<false>(*this);
  }

  struct element {
    std::atomic<size_t> flags;
    aw_ticket_queue_waiter_base* consumer;
    T data;
    element() { flags.store(0, std::memory_order_release); }
  };

  struct data_block;

  struct alignas(16) block_list {
    data_block* block;
    size_t offset;
  };

  struct data_block {
    std::atomic<block_list> next;
    std::array<element, Capacity> values;

    data_block(size_t offset) : values{} {
      next.store({nullptr, offset}, std::memory_order_release);
    }
  };

  std::atomic<bool> closed;
  std::atomic<size_t> closed_at;
  alignas(16) std::atomic<block_list> all_blocks;
  tmc::tiny_lock blocks_lock;
  char pad0[64 - 3 * (sizeof(size_t))];
  // If write_offset == read_offset, queue is empty
  // If write_offset == read_offset - 1, queue is full
  // 1 element of capacity is wasted to make this work
  alignas(16) std::atomic<block_list> write_blocks;
  char pad1[64 - 2 * (sizeof(size_t))];
  alignas(16) std::atomic<block_list> read_blocks;
  char pad2[64 - 2 * (sizeof(size_t))];

  ticket_queue() : closed{false}, closed_at{TMC_ALL_ONES} {
    auto block = new data_block(0);
    all_blocks = {block, 0};
    write_blocks = {block, 0};
    read_blocks = {block, 0};
  }

  static_assert(std::atomic<block_list>::is_always_lock_free);

  // TODO add bool template argument to push this work to a background task
  // Access to this function (the blocks pointer specifically) must be
  // externally synchronized (via blocks_lock).
  template <bool Wait> void try_free_block() {
    // TODO think about the order / kind of these atomic loads
    block_list blocks = all_blocks.load(std::memory_order_acquire);
    while (true) {
      block_list wblocks = write_blocks.load(std::memory_order_acquire);
      block_list rblocks = read_blocks.load(std::memory_order_acquire);
      data_block* block = blocks.block;
      data_block* wblock = wblocks.block;
      data_block* rblock = rblocks.block;
      // TODO RACE:
      // A: Load Rblock/Wblock here
      // B: Update block->next
      // A: Iterate block->next
      // A: Load Rblock/Wblock here (sees old value / no match, ok)
      // B: Update Rblock/Wblock
      // A: Deleted block->next which is currently active Rblock/Wblock

      size_t nextOffset = block->next.load(std::memory_order_acquire).offset;
      if (nextOffset == wblock->next.load(std::memory_order_acquire).offset ||
          nextOffset == rblock->next.load(std::memory_order_acquire).offset) {
        // Cannot free currently active block
        return;
      }

      for (size_t idx = 0; idx < Capacity; ++idx) {
        auto v = &block->values[idx];
        if constexpr (Wait) {
          // Data is present at these elements; wait for the queue to drain
          while (v->flags.load(std::memory_order_acquire) != 3) {
            TMC_CPU_PAUSE();
          }
        } else {
          // All elements in this block must be done before it can be deleted.
          size_t flags = v->flags.load(std::memory_order_acquire);
          if (flags != 3) {
            return;
          }
        }
      }

      block_list next = block->next.load(std::memory_order_acquire);
      assert(next.block != nullptr);
      block_list expected = blocks;
      blocks.offset = blocks.offset + Capacity;
      blocks.block = next.block;
      [[maybe_unused]] bool ok = all_blocks.compare_exchange_strong(
        expected, blocks, std::memory_order_acq_rel, std::memory_order_acquire
      );
      assert(ok);

      delete block;
    }
  }

  // TODO: Single-width CAS Conversion
  // Load index
  // Load block - if block at breakpoint, call AllocBlock
  // XCHG index + 1
  // If failed, retry
  // If index is more than Capacity breakpoint, reload block

  // AllocBlock:
  // If block->next, use that
  // Else XCHG nullptr / new block with block->next
  // If failed, use loaded value
  // XCHG with block head

  // Returns the currently active block that can be accessed via idx.
  // Returns nullptr if the queue is closed.
  template <bool IsPush>
  element* get_ticket(std::atomic<block_list>& block_head) {
    block_list blocks = block_head.load(std::memory_order_acquire);
    while (true) {
      // Check closed flag
      if constexpr (IsPush) {
        // close() will set `closed` before incrementing write offset
        // thus we are guaranteed to see it if we acquire write offset first
        if (closed.load(std::memory_order_acquire)) {
          return nullptr;
        }
      } else { // Pull
        // If closed, continue draining until the queue is empty
        // As indicated by the closed_at index
        if (closed.load(std::memory_order_acquire)) {
          if (blocks.offset >= closed_at.load(std::memory_order_acquire)) {
            return nullptr;
          }
        }
      }

      // Increment block offset by RMW
      // Allocate or update next block pointer as necessary
      block_list updated{blocks.block, blocks.offset + 1};
      if ((updated.offset & CapacityMask) == 0) {
        block_list next = blocks.block->next.load(std::memory_order_acquire);
        if (next.block != nullptr) {
          updated.block = next.block;
        } else {
          // This counter is needed to detect ABA when
          // block is deallocated and replaced while we are trying to add next.
          block_list expected{nullptr, next.offset};
          next.block = new data_block(updated.offset);
          // Created new block, update block next first
          if (blocks.block->next.compare_exchange_strong(
                expected, next, std::memory_order_acq_rel,
                std::memory_order_acquire
              )) {
            updated.block = next.block;
          } else {
            // Another thread created the block first
            delete next.block;
            updated.block = expected.block;
          }
        }
        if (block_head.compare_exchange_strong(
              blocks, updated, std::memory_order_acq_rel,
              std::memory_order_acquire
            )) {
          if (blocks_lock.try_lock()) {
            try_free_block<false>();
            blocks_lock.unlock();
          }
          break;
        }
      } else {
        if (block_head.compare_exchange_strong(
              blocks, updated, std::memory_order_acq_rel,
              std::memory_order_acquire
            )) {
          break;
        }
      }
    }

    // Get a pointer to the element we got a ticket for
    data_block* block = blocks.block;
    size_t idx = blocks.offset;
    assert(idx >= block->offset && idx < block->offset + Capacity);
    element* elem = &block->values[idx & CapacityMask];
    return elem;
  }

  template <typename U> queue_error push(U&& u) {
    // Get write ticket and associated block.
    element* elem = get_ticket<true>(write_blocks);
    if (elem == nullptr) {
      return CLOSED;
    }
    size_t flags = elem->flags.load(std::memory_order_relaxed);
    assert((flags & 1) == 0);

    // Check if consumer is waiting
    if (flags & 2) {
      // There was a consumer waiting for this data
      auto cons = elem->consumer;
      // Still need to store so block can be freed
      elem->flags.store(3, std::memory_order_release);
      cons->t = std::forward<U>(u);
      tmc::detail::post_checked(
        cons->continuation_executor, std::move(cons->continuation), cons->prio
      );
      return OK;
    }

    // No consumer waiting, store the data
    elem->data = std::forward<U>(u);

    // Finalize transaction
    size_t expected = 0;
    if (!elem->flags.compare_exchange_strong(
          expected, 1, std::memory_order_acq_rel, std::memory_order_acquire
        )) {
      // Consumer started waiting for this data during our RMW cycle
      assert(expected == 2);
      auto cons = elem->consumer;
      cons->t = std::move(elem->data);
      tmc::detail::post_checked(
        cons->continuation_executor, std::move(cons->continuation), cons->prio
      );
      elem->flags.store(3, std::memory_order_release);
    }
    return OK;
  }

  template <bool Must>
  class aw_ticket_queue_pull : protected aw_ticket_queue_waiter_base,
                               private tmc::detail::AwaitTagNoGroupAsIs {
    using aw_ticket_queue_waiter_base::continuation;
    using aw_ticket_queue_waiter_base::continuation_executor;
    using aw_ticket_queue_waiter_base::err;
    using aw_ticket_queue_waiter_base::queue;
    using aw_ticket_queue_waiter_base::t;
    element* elem;

    aw_ticket_queue_pull(ticket_queue& q) : aw_ticket_queue_waiter_base(q) {}

    // May return a value or CLOSED
    friend aw_ticket_queue_pull ticket_queue::pull();

  public:
    bool await_ready() {
      // Get read ticket and associated block.
      elem = queue.template get_ticket<false>(queue.read_blocks);
      if (elem == nullptr) {
        err = CLOSED;
        return true;
      }

      // Check if data is ready
      size_t flags = elem->flags.load(std::memory_order_relaxed);
      assert((flags & 2) == 0);

      if (flags & 1) {
        // Data is already ready here.
        t = std::move(elem->data);
        // Still need to store so block can be freed
        elem->flags.store(3, std::memory_order_release);
        return true;
      }
      return false;
    }
    bool await_suspend(std::coroutine_handle<> Outer) {
      // Data wasn't ready, prepare to suspend
      continuation = Outer;
      elem->consumer = this;

      // Finalize transaction
      size_t expected = 0;
      if (!elem->flags.compare_exchange_strong(
            expected, 2, std::memory_order_acq_rel, std::memory_order_acquire
          )) {
        // data became ready during our RMW cycle
        assert(expected == 1);
        t = std::move(elem->data);
        elem->flags.store(3, std::memory_order_release);
        return false;
      }
      return true;
    }
    std::variant<T, queue_error> await_resume()
      requires(!Must)
    {
      if (err == OK) {
        return std::move(t);
      } else {
        return err;
      }
    }
    T&& await_resume()
      requires(Must)
    {
      return std::move(t);
    }
  };

  // template <bool Must> class aw_ticket_queue_pop {
  //   ticket_queue& queue;
  //   bool await_ready() {}
  //   bool await_suspend() {}
  //   std::variant<T, queue_error> await_resume()
  //     requires(!Must)
  //   {}
  //   T await_resume()
  //     requires(Must)
  //   {}
  // };

public:
  // May return OK, FULL, or CLOSED
  queue_error try_push();
  // May return a value, or EMPTY or CLOSED
  std::variant<T, queue_error> try_pull();
  // May return a value, or EMPTY OR CLOSED
  std::variant<T, queue_error> try_pop();

  // // May return a value or CLOSED
  // aw_ticket_queue_pop<false> pop() {}

  // Returns void. If the queue is closed, std::terminate will be called.
  aw_ticket_queue_push<true> must_push();
  // Returns a value. If the queue is closed, std::terminate will be called.
  aw_ticket_queue_pull<true> must_pull();
  // // Returns a value. If the queue is closed, std::terminate will be
  // called. aw_ticket_queue_pop<true> must_pop() {}

  // TODO separate close() and drain() operations
  // TODO separate drain() (async) and drain_sync()
  // will need a single location on the queue to store the drain async
  // continuation all consumers participate in checking this to awaken

  // All currently waiting producers will return CLOSED.
  // Consumers will continue to read data until the queue is drained,
  // at which point all consumers will return CLOSED.
  void close() {
    blocks_lock.spin_lock();
    closed.store(true, std::memory_order_seq_cst);
    try_free_block<true>();
    block_list wblock = write_blocks.load(std::memory_order_seq_cst);
    while (true) {
      block_list updated{wblock.block, wblock.offset + 1};
      if (write_blocks.compare_exchange_strong(
            wblock, updated, std::memory_order_seq_cst,
            std::memory_order_seq_cst
          )) {
        break;
      }
    }
    size_t woff = wblock.offset;
    closed_at.store(woff, std::memory_order_seq_cst);
    block_list rblock = read_blocks.load(std::memory_order_seq_cst);
    size_t roff = rblock.offset;
    block_list blocks = all_blocks.load(std::memory_order_seq_cst);
    data_block* block = blocks.block;
    size_t i = block->next.load(std::memory_order_seq_cst).offset;

    // Wait for the queue to drain (elements prior to closed_at write index)
    while (true) {
      while (i < roff) {
        if (i < woff) {
          size_t idx = i & CapacityMask;
          auto v = &block->values[idx];
          // Data is present at these elements; wait for the queue to drain
          while (v->flags.load(std::memory_order_acquire) != 3) {
            TMC_CPU_PAUSE();
          }
        } else { // i >= woff
          break;
        }
        ++i;
        if ((i & CapacityMask) == 0) {
          block_list next = block->next.load(std::memory_order_acquire);
          while (next.block == nullptr) {
            // A block is being constructed. Wait for it.
            TMC_CPU_PAUSE();
            next = block->next.load(std::memory_order_acquire);
          }
          block = next.block;
        }
      }
      if (roff < woff) {
        TMC_CPU_PAUSE();
        rblock = read_blocks.load(std::memory_order_seq_cst);
        roff = rblock.offset;
      } else {
        break;
      }
    }

    // `closed` is accessed by relaxed load in consumer.
    // In order to ensure that it is seen in a timely fashion, this
    // creates a release sequence with the acquire load in consumer.

    rblock = read_blocks.load(std::memory_order_seq_cst);
    while (true) {
      block_list updated{rblock.block, rblock.offset + 1};
      if (read_blocks.compare_exchange_strong(
            rblock, updated, std::memory_order_seq_cst,
            std::memory_order_seq_cst
          )) {
        break;
      }
    }
    roff = rblock.offset;

    // No data will be written to these elements. They are past the closed_at
    // write index. `roff` is now the closed-at read index. Consumers will be
    // waiting at indexes prior to `roff`.
    while (i < roff) {
      size_t idx = i & CapacityMask;
      auto v = &block->values[idx];

      // Wait for consumer to appear
      size_t flags = v->flags.load(std::memory_order_acquire);
      while ((flags & 2) == 0) {
        // std::printf("waiting\n");
        TMC_CPU_PAUSE();
        flags = v->flags.load(std::memory_order_acquire);
      }

      // If flags & 1 is set, it indicates the consumer saw the closed flag
      // and did not wait. Otherwise, wakeup the waiting consumer.
      if ((flags & 1) == 0) {
        auto cons = v->consumer;
        cons->err = CLOSED;
        tmc::detail::post_checked(
          cons->continuation_executor, std::move(cons->continuation), cons->prio
        );
      }

      ++i;
      if (i >= roff) {
        break;
      }
      if ((i & CapacityMask) == 0) {
        block_list next = block->next.load(std::memory_order_acquire);
        while (next.block == nullptr) {
          // A block is being constructed. Wait for it.
          TMC_CPU_PAUSE();
          next = block->next.load(std::memory_order_acquire);
        }
        block = next.block;
      }
    }
    blocks_lock.unlock();
  }

  ~ticket_queue() {
    block_list blocks = all_blocks.load(std::memory_order_acquire);
    data_block* block = blocks.block;
    while (block != nullptr) {
      block_list next = block->next.load(std::memory_order_acquire);
      delete block;
      block = next.block;
    }
  }

  ticket_queue(const ticket_queue&) = delete;
  ticket_queue& operator=(const ticket_queue&) = delete;
  // TODO implement move constructor
};

} // namespace tmc
