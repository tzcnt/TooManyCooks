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
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

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
    data_block* next;
    size_t offset;
  };

  struct data_block {
    // Making this DWCAS slowed things down quite a bit.
    // Maybe loads from it are being implemented as CAS?
    // Maybe you can use a union / type pun / atomic_ref to avoid that
    std::atomic<block_list> info;
    std::array<element, Capacity> values;

    data_block(size_t offset) : values{} {
      info.store({nullptr, offset}, std::memory_order_release);
    }
  };

  data_block* ALL_BLOCKS = reinterpret_cast<data_block*>(1);
  data_block* WRITE_BLOCKS = reinterpret_cast<data_block*>(2);
  data_block* READ_BLOCKS = reinterpret_cast<data_block*>(3);

  enum op_name { ALLOC, FREE, LINK_TO, LINK_FROM, UNLINK_TO, UNLINK_FROM };
  struct operation {
    data_block* target;
    size_t offset;
    op_name op;
    std::thread::id tid;
  };
  struct tracker {
    tmc::tiny_lock lock;
    std::vector<operation> ops;
    tracker() {}
  };

  struct tracker_scope {
    tracker* t;
    tmc::tiny_lock_guard lg;
    tracker_scope(tracker* tr) : t{tr}, lg{t->lock} {}

    inline void
    push_op(size_t off, op_name op, data_block* tar, std::thread::id tid) {
      t->ops.emplace_back(tar, off, op, tid);
    }
    tracker_scope(tracker_scope const&) = delete;
    tracker_scope& operator=(tracker_scope const&) = delete;
  };

  static inline thread_local std::vector<tracker*> tracker_pool;

  tmc::tiny_lock trackers_lock;
  std::unordered_map<data_block*, tracker*> blocks_map;
  tracker_scope get_tracker(data_block* b) {
    assert(b != nullptr);
    assert(!tracker_pool.empty());
    tracker* tr = tracker_pool.back();
    trackers_lock.spin_lock();
    auto p = blocks_map.insert({b, tr});
    tracker* t;
    if (p.second) {
      trackers_lock.unlock();
      t = tr;
      tracker_pool.pop_back();
    } else {
      t = p.first->second;
      trackers_lock.unlock();
    }
    assert(t != nullptr);
    return tracker_scope(t);
  }

  std::pair<tracker_scope, tracker_scope>
  get_two_trackers(data_block* b1, data_block* b2) {
    assert(b1 != nullptr);
    assert(b2 != nullptr);
    assert(!tracker_pool.empty());
    tracker* t1;
    tracker* t2;
    tracker* tr = tracker_pool.back();
    trackers_lock.spin_lock();
    auto p = blocks_map.insert({b1, tr});
    if (p.second) {
      t1 = tr;
      tracker_pool.pop_back();
    } else {
      t1 = p.first->second;
    }

    tr = tracker_pool.back();
    p = blocks_map.insert({b2, tr});
    if (p.second) {
      trackers_lock.unlock();
      t2 = tr;
      tracker_pool.pop_back();
    } else {
      t2 = p.first->second;
      trackers_lock.unlock();
    }
    assert(t1 != nullptr);
    assert(t2 != nullptr);
    return std::make_pair(t1, t2);
  }

  std::vector<data_block*> freelist;
  tmc::tiny_lock freelist_lock;

  std::atomic<size_t> closed;
  std::atomic<size_t> write_closed_at;
  std::atomic<size_t> read_closed_at;
  std::atomic<size_t> drained_to; // exclusive
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

  ticket_queue()
      : blocks_map{1000}, closed{0}, write_closed_at{TMC_ALL_ONES},
        read_closed_at{TMC_ALL_ONES}, drained_to{0} {
    freelist.reserve(1024);
    auto block = new data_block(0);
    all_blocks = {block, 0};
    write_blocks = {block, 0};
    read_blocks = {block, 0};
  }

  static_assert(std::atomic<block_list>::is_always_lock_free);

  // TODO add bool template argument to push this work to a background task
  // Access to this function (the blocks pointer specifically) must be
  // externally synchronized (via blocks_lock).
  void try_free_block() {
    auto tid = std::this_thread::get_id();
    // TODO think about the order / kind of these atomic loads
    block_list blocks = all_blocks.load(std::memory_order_acquire);
    size_t drain_offset = drained_to.load(std::memory_order_acquire);
    while (true) {
      block_list wblocks = write_blocks.load(std::memory_order_acquire);
      block_list rblocks = read_blocks.load(std::memory_order_acquire);
      data_block* block = blocks.next;
      data_block* wblock = wblocks.next;
      data_block* rblock = rblocks.next;
      // TODO RACE:
      // A: Load Rblock/Wblock here
      // B: Update block->next
      // A: Iterate block->next
      // A: Load Rblock/Wblock here (sees old value / no match, ok)
      // B: Update Rblock/Wblock
      // A: Deleted block->next which is currently active Rblock/Wblock

      block_list info = block->info.load(std::memory_order_acquire);
      size_t nextOffset = info.offset;
      // if (info.next == nullptr ||
      //     nextOffset >= wblock->info.load(std::memory_order_acquire).offset
      //     || nextOffset >=
      //     rblock->info.load(std::memory_order_acquire).offset) {
      //   // Cannot free currently active block
      //   drained_to.store(nextOffset, std::memory_order_release);
      //   return;
      // }

      for (size_t idx = 0; idx < Capacity; ++idx) {
        auto v = &block->values[idx];
        // All elements in this block must be done before it can be deleted.
        size_t flags = v->flags.load(std::memory_order_acquire);
        if (flags != 3) {
          drained_to.store(nextOffset + idx, std::memory_order_release);
          return;
        }
      }
#ifndef NDEBUG
      assert(info.offset == blocks.offset);
#else
      if (info.offset != blocks.offset) {
        std::printf("FAIL off\n");
        std::fflush(stdout);
      }
#endif
      drain_offset = nextOffset + Capacity;

      block_list expected = blocks;
      blocks.offset = blocks.offset + Capacity;
      blocks.next = info.next;
      [[maybe_unused]] bool ok = all_blocks.compare_exchange_strong(
        expected, blocks, std::memory_order_acq_rel, std::memory_order_acquire
      );
      block_list next_block = info.next->info.load(std::memory_order_acquire);
#ifndef NDEBUG
      assert(ok);
      assert(next_block.offset == blocks.offset);
#else
      if (!ok) {
        std::printf("!OK\n");
        std::fflush(stdout);
      }
      if (next_block.offset != blocks.offset) {
        std::printf("FAIL\n");
        std::fflush(stdout);
      }
#endif

      block->info.store(
        block_list{reinterpret_cast<data_block*>(TMC_ALL_ONES), TMC_ALL_ONES}
      );

      {
        auto ts = get_two_trackers(block, info.next);
        ts.first.push_op(info.offset, UNLINK_FROM, ALL_BLOCKS, tid);
        ts.first.push_op(info.offset, UNLINK_TO, info.next, tid);
        ts.first.push_op(info.offset, FREE, nullptr, tid);
        ts.second.push_op(next_block.offset, UNLINK_FROM, block, tid);
      }

      freelist_lock.spin_lock();
      freelist.push_back(block);
      freelist_lock.unlock();
    }
  }

  // TODO: Single-width CAS Conversion
  // Load index
  // Load block - if block at breakpoint, call AllocBlock
  //   Does this violate ? We shouldn't read from block until XCHG succeeds
  //   Might be able to move this after XCHG success

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
    auto tid = std::this_thread::get_id();
    block_list blocks = block_head.load(std::memory_order_acquire);
    data_block* block;
    size_t idx;
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
        // As indicated by the write_closed_at index
        if (closed.load(std::memory_order_acquire)) {
          if (blocks.offset >=
              write_closed_at.load(std::memory_order_acquire)) {
            return nullptr;
          }
        }
      }

      // Get a pointer to the element we got a ticket for
      block = blocks.next;
      idx = blocks.offset;

      // Increment block offset by RMW
      // Allocate or update next block pointer as necessary
      block_list updated{block, idx + 1};
      if ((updated.offset & CapacityMask) == 0) {
        block_list next = blocks.next->info.load(std::memory_order_acquire);
        if (next.next != nullptr) {
          updated.next = next.next;
          // if (next.next->info.load(std::memory_order_acquire).offset !=
          //     updated.offset) {
          //   std::printf("fail\n");
          //   std::fflush(stdout);
          // }
        } else {
          // This counter is needed to detect ABA when
          // block is deallocated and replaced while we are trying to add next.
          size_t expectedOffset = updated.offset - Capacity;
          block_list expected{nullptr, expectedOffset};

          freelist_lock.spin_lock();
          bool fromFreeList = !freelist.empty();
          if (!fromFreeList) {
            freelist_lock.unlock();
            next.next = new data_block(updated.offset);
          } else {
            data_block* f = freelist.back();
            freelist.pop_back();
            freelist_lock.unlock();
            for (size_t i = 0; i < Capacity; ++i) {
              f->values[i].flags.store(0, std::memory_order_relaxed);
            }
            f->info.store(
              block_list{nullptr, updated.offset}, std::memory_order_release
            );
            next.next = f;
          }
          // Created new block, update block next first
          if (blocks.next->info.compare_exchange_strong(
                expected, next, std::memory_order_acq_rel,
                std::memory_order_acquire
              )) {
            {
              auto ts = get_two_trackers(block, next.next);
              ts.first.push_op(expectedOffset, LINK_TO, next.next, tid);
              ts.second.push_op(updated.offset, ALLOC, nullptr, tid);
              ts.second.push_op(updated.offset, LINK_FROM, block, tid);
            }

            updated.next = next.next;
          } else {
            if (!fromFreeList) {
              // This block never entered circulation; it's safe to delete
              delete next.next;
            } else {
              // Freelist blocks cannot be deleted
              freelist_lock.spin_lock();
              freelist.push_back(next.next);
              freelist_lock.unlock();
            }
            if (expected.offset != expectedOffset) {
              // ABA condition detected
              blocks = block_head.load(std::memory_order_acquire);
              continue;
            } else {
              // Another thread created the block first
              updated.next = expected.next;
            }
          }
        }
        if (block_head.compare_exchange_strong(
              blocks, updated, std::memory_order_acq_rel,
              std::memory_order_acquire
            )) {
          size_t off = blocks.next->info.load(std::memory_order_acquire).offset;
          size_t nextoff =
            updated.next->info.load(std::memory_order_acquire).offset;
          {
            auto t = get_two_trackers(blocks.next, updated.next);
            if constexpr (IsPush) {
              t.first.push_op(off, UNLINK_FROM, WRITE_BLOCKS, tid);
              t.second.push_op(nextoff, LINK_FROM, WRITE_BLOCKS, tid);
            } else {
              t.first.push_op(off, UNLINK_FROM, WRITE_BLOCKS, tid);
              t.second.push_op(nextoff, LINK_FROM, READ_BLOCKS, tid);
            }
          }
          if (blocks_lock.try_lock()) {
            try_free_block();
            blocks_lock.unlock();
          }
          break;
        }
      } else {
        if (block_head.compare_exchange_strong(
              blocks, updated, std::memory_order_acq_rel,
              std::memory_order_acquire
            )) {
          assert(block == updated.next);
          size_t off =
            updated.next->info.load(std::memory_order_acquire).offset;
          {
            auto t = get_tracker(updated.next);
            if constexpr (IsPush) {
              t.push_op(off, UNLINK_FROM, WRITE_BLOCKS, tid);
            } else {
              t.push_op(off, UNLINK_FROM, WRITE_BLOCKS, tid);
            }
          }
          break;
        }
      }
    }

    assert(
      idx >= block->info.load(std::memory_order_acquire).offset &&
      idx < block->info.load(std::memory_order_acquire).offset + Capacity
    );
    element* elem = &block->values[idx & CapacityMask];
    return elem;
  }

  template <typename U> queue_error push(U&& u) {
    // Get write ticket and associated block.
    element* elem = get_ticket<true>(write_blocks);
    if (elem == nullptr) {
      return CLOSED;
    }
    size_t flags = elem->flags.load(std::memory_order_acquire);
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
      size_t flags = elem->flags.load(std::memory_order_acquire);
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
    size_t expected = 0;
    if (!closed.compare_exchange_strong(
          expected, 1, std::memory_order_seq_cst, std::memory_order_seq_cst
        )) {
      return;
    }

    block_list wblock = write_blocks.load(std::memory_order_seq_cst);
    while (true) {
      block_list updated{wblock.next, wblock.offset + 1};
      if (write_blocks.compare_exchange_strong(
            wblock, updated, std::memory_order_seq_cst,
            std::memory_order_seq_cst
          )) {
        break;
      }
    }
    write_closed_at.store(wblock.offset, std::memory_order_seq_cst);
    blocks_lock.unlock();
  }

  // Waits for the queue to drain.
  void drain_sync() {
    blocks_lock.spin_lock();
    size_t woff = write_closed_at.load(std::memory_order_seq_cst);
    block_list wblock = write_blocks.load(std::memory_order_seq_cst);
    block_list rblock = read_blocks.load(std::memory_order_seq_cst);
    size_t roff = rblock.offset;
    block_list blocks = all_blocks.load(std::memory_order_seq_cst);
    data_block* block = blocks.next;
    size_t i = blocks.offset;
    // drained_to.load(std::memory_order_acquire);

    // Wait for the queue to drain (elements prior to write_closed_at write
    // index)
    while (true) {
      while (i < roff && i < woff) {
        size_t idx = i & CapacityMask;
        auto v = &block->values[idx];
        // Data is present at these elements; wait for the queue to drain
        while (v->flags.load(std::memory_order_acquire) != 3) {
          TMC_CPU_PAUSE();
        }

        ++i;
        if ((i & CapacityMask) == 0) {
          block_list next = block->info.load(std::memory_order_acquire);
          while (next.next == nullptr) {
            // A block is being constructed. Wait for it.
            TMC_CPU_PAUSE();
            next = block->info.load(std::memory_order_acquire);
          }
          block = next.next;
        }
      }
      if (roff < woff) {
        // Wait for readers to catch up.
        TMC_CPU_PAUSE();
        rblock = read_blocks.load(std::memory_order_seq_cst);
        roff = rblock.offset;
      } else {
        break;
      }
    }

    // i >= woff now and all data has been drained.
    // Now handle waking up waiting consumers.

    // `closed` is accessed by relaxed load in consumer.
    // In order to ensure that it is seen in a timely fashion, this
    // creates a release sequence with the acquire load in consumer.

    if (closed.load() != 3) {
      rblock = read_blocks.load(std::memory_order_seq_cst);
      while (true) {
        block_list updated{rblock.next, rblock.offset + 1};
        if (read_blocks.compare_exchange_strong(
              rblock, updated, std::memory_order_seq_cst,
              std::memory_order_seq_cst
            )) {
          read_closed_at.store(rblock.offset, std::memory_order_seq_cst);
          closed.store(3, std::memory_order_seq_cst);
          break;
        }
      }
    }
    roff = read_closed_at.load(std::memory_order_seq_cst);

    // No data will be written to these elements. They are past the
    // write_closed_at write index. `roff` is now read_closed_at.
    // Consumers will be waiting at indexes prior to `roff`.
    while (i < roff) {
      size_t idx = i & CapacityMask;
      auto v = &block->values[idx];

      // Wait for consumer to appear
      size_t flags = v->flags.load(std::memory_order_acquire);
      while ((flags & 2) == 0) {
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
        block_list next = block->info.load(std::memory_order_acquire);
        while (next.next == nullptr) {
          // A block is being constructed. Wait for it.
          TMC_CPU_PAUSE();
          next = block->info.load(std::memory_order_acquire);
        }
        block = next.next;
      }
    }
    drained_to.store(i, std::memory_order_release);
    blocks_lock.unlock();
  }

  ~ticket_queue() {
    // drain_sync();
    blocks_lock.spin_lock();
    trackers_lock.spin_lock();
    block_list blocks = all_blocks.load(std::memory_order_acquire);
    data_block* block = blocks.next;
    size_t idx = blocks.offset;
    while (block != nullptr) {
      block_list next = block->info.load(std::memory_order_acquire);
      if (idx != next.offset) {
        std::printf("idx %zu != offset %zu\n", idx, next.offset);
      }
      delete block;
      tracker* tr = blocks_map[block];
      delete tr;
      blocks_map.erase(block);
      block = next.next;
      idx = idx + Capacity;
    }
    freelist_lock.spin_lock();
    for (size_t i = 0; i < freelist.size(); ++i) {
      block = freelist[i];
      delete block;
      tracker* tr = blocks_map[block];
      delete tr;
      blocks_map.erase(block);
    }
    if (!blocks_map.empty()) {
      for (auto it = blocks_map.begin(); it != blocks_map.end(); ++it) {
        data_block* leak = it->first;
        tracker* track = it->second;
        std::printf(
          "leaked: %p  %zu\n", leak,
          leak->info.load(std::memory_order_acquire).offset
        );

        // enum op_name { ALLOC, FREE, LINK_TO, LINK_FROM, UNLINK_TO,
        // UNLINK_FROM }; struct operation {
        //   data_block* target;
        //   size_t offset;
        //   op_name op;
        // };
        for (size_t i = 0; i < track->ops.size(); ++i) {
          operation& op = track->ops[i];
          const char* opName;
          switch (op.op) {
          case ALLOC:
            opName = "ALLOC";
            break;
          case FREE:
            opName = "FREE";
            break;
          case LINK_TO:
            opName = "LINK_TO";
            break;
          case LINK_FROM:
            opName = "LINK_FROM";
            break;
          case UNLINK_TO:
            opName = "UNLINK_TO";
            break;
          case UNLINK_FROM:
            opName = "UNLINK_FROM";
            break;
          }
          std::printf("%-12s: %p %zu", opName, op.target, op.offset);
        }
      }
      std::terminate();
    }
  }

  ticket_queue(const ticket_queue&) = delete;
  ticket_queue& operator=(const ticket_queue&) = delete;
  // TODO implement move constructor
};

} // namespace tmc
