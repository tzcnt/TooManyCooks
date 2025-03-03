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
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

// TODO mask with highest bit set to 0 when comparing indexes

namespace this_thread {
inline thread_local size_t thread_slot = TMC_ALL_ONES;
} // namespace this_thread

namespace tmc {
enum queue_error { OK = 0, CLOSED = 1, EMPTY = 2, FULL = 3 };
template <typename T, size_t Size = 256> class queue_handle;
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

  friend queue_handle<T, Size>;

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

  enum op_name {
    ALLOC_RAW,
    FREE_RAW,
    ALLOC_LIST,
    FREE_LIST,
    LINK_TO,
    LINK_FROM,
    UNLINK_TO,
    UNLINK_FROM
  };
  struct operation {
    data_block* target;
    size_t offset;
    op_name op;
    size_t tid;
  };
  struct alignas(64) tracker {
    tmc::tiny_lock lock;
    std::vector<operation> ops;
    tracker() {}
  };

  struct tracker_scope {
    tracker* t;
    tmc::tiny_lock_guard lg;
    tracker_scope(tracker* tr) : t{tr}, lg{t->lock} {}

    inline void push_op(size_t off, op_name op, data_block* tar, size_t tid) {
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

  // std::vector<data_block*> freelist;
  // tmc::tiny_lock freelist_lock;

  std::atomic<size_t> closed;
  std::atomic<size_t> write_closed_at;
  std::atomic<size_t> read_closed_at;
  std::atomic<size_t> drained_to; // exclusive
  alignas(16) std::atomic<block_list> all_blocks;
  alignas(16) std::atomic<block_list> blocks_tail;
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
    // freelist.reserve(1024);
    auto block = new data_block(0);
    all_blocks = {block, 0};
    blocks_tail = {block, 0};
    write_blocks = {block, 0};
    read_blocks = {block, 0};
  }

  static_assert(std::atomic<block_list>::is_always_lock_free);

  template <size_t Max>
  size_t
  unlink_blocks(data_block*& block, std::array<data_block*, Max>& unlinked) {
    size_t unlinkedCount = 0;
    for (; unlinkedCount < Max; ++unlinkedCount) {
      if (block == nullptr) {
        break;
      }
      block_list info = block->info.load(std::memory_order_acquire);
      for (size_t idx = 0; idx < Capacity; ++idx) {
        auto v = &block->values[idx];
        // All elements in this block must be done before it can be deleted.
        size_t flags = v->flags.load(std::memory_order_acquire);
        if (flags != 3) {
          drained_to.store(info.offset + idx, std::memory_order_release);
          return unlinkedCount;
        }
      }
      unlinked[unlinkedCount] = block;
      block = info.next;
    }
    return unlinkedCount;
  }

  // TODO add bool template argument to push this work to a background task
  // Access to this function (the blocks pointer specifically) must be
  // externally synchronized (via blocks_lock).
  void try_free_block() {
    std::array<data_block*, 4> unlinked;
    auto tid = this_thread::thread_slot;
    // TODO think about the order / kind of these atomic loads
    block_list blocks = all_blocks.load(std::memory_order_acquire);
    data_block* block = blocks.next;
    size_t drain_offset = drained_to.load(std::memory_order_acquire);
    while (true) {
      size_t unlinkedCount = unlink_blocks(block, unlinked);
      if (unlinkedCount == 0) {
        return;
      }
      assert(block != nullptr);
      assert(
        block->info.load(std::memory_order_acquire).offset ==
        blocks.offset + Capacity * unlinkedCount
      );

      block_list expected = blocks;
      blocks.offset = blocks.offset + Capacity * unlinkedCount;
      blocks.next = block;
      [[maybe_unused]] bool ok = all_blocks.compare_exchange_strong(
        expected, blocks, std::memory_order_acq_rel, std::memory_order_acquire
      );
#ifndef NDEBUG
      assert(ok);
#else
      if (!ok) {
        std::printf("!OK2\n");
        std::fflush(stdout);
      }
#endif

      drain_offset = blocks.offset;

      for (size_t i = 0; i < unlinkedCount; ++i) {
        data_block* b = unlinked[i];
        for (size_t i = 0; i < Capacity; ++i) {
          b->values[i].flags.store(0, std::memory_order_relaxed);
        }
      }

      block_list tail = blocks_tail.load(std::memory_order_acquire);
      data_block* tailBlock = tail.next;
      block_list tailBlockInfo =
        tailBlock->info.load(std::memory_order_acquire);
      block_list updatedTailBlockInfo;
      updatedTailBlockInfo.next = unlinked[0];
      if (unlinkedCount > 1) {
        tailBlock = tail.next;
      }
      do {
        while (tailBlockInfo.next != nullptr) {
          tailBlock = tailBlockInfo.next;
          tailBlockInfo = tailBlock->info.load(std::memory_order_acquire);
        }
        size_t i = 0;
        size_t boff = tailBlockInfo.offset + Capacity;
        for (; i < unlinkedCount - 1; ++i) {
          data_block* b = unlinked[i];
          b->info.store(
            block_list{unlinked[i + 1], boff}, std::memory_order_release
          );
          boff += Capacity;
        }
        unlinked[i]->info.store(
          block_list{nullptr, boff}, std::memory_order_release
        );

        updatedTailBlockInfo.offset = tailBlockInfo.offset;
      } while (!tailBlock->info.compare_exchange_strong(
        tailBlockInfo, updatedTailBlockInfo, std::memory_order_acq_rel,
        std::memory_order_acquire
      ));

      ok = blocks_tail.compare_exchange_strong(
        tail, updatedTailBlockInfo, std::memory_order_acq_rel,
        std::memory_order_acquire
      );
#ifndef NDEBUG
      assert(ok);
#else
      if (!ok) {
        std::printf("!OK2\n");
        std::fflush(stdout);
      }
#endif
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
    auto tid = this_thread::thread_slot;
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
          // assert(
          //   next.next->info.load(std::memory_order_acquire).offset ==
          //   updated.offset
          // );
          // if (next.next->info.load(std::memory_order_acquire).offset !=
          //     updated.offset) {
          //   blocks = block_head.load(std::memory_order_acquire);
          //   continue;
          // }
        } else {
          // This counter is needed to detect ABA when
          // block is deallocated and replaced while we are trying to add
          // next.
          size_t expectedOffset = updated.offset - Capacity;
          block_list expected{nullptr, expectedOffset};
          next.next = new data_block(updated.offset);
          // Created new block, update block next first
          if (blocks.next->info.compare_exchange_strong(
                expected, next, std::memory_order_acq_rel,
                std::memory_order_acquire
              )) {
            // next.next->info.store(
            //   block_list{nullptr, updated.offset}, std::memory_order_release
            // );
            // {
            //   auto ts = get_two_trackers(block, next.next);
            //   ts.first.push_op(expectedOffset, LINK_TO, next.next, tid);
            //   if (!fromFreeList) {
            //     ts.second.push_op(updated.offset, ALLOC_RAW, nullptr, tid);
            //   }
            //   ts.second.push_op(updated.offset, LINK_FROM, block, tid);
            // }

            updated.next = next.next;
          } else {
            delete next.next;
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
          // {
          //   auto t = get_two_trackers(blocks.next, updated.next);
          //   if constexpr (IsPush) {
          //     t.first.push_op(off, UNLINK_FROM, WRITE_BLOCKS, tid);
          //     t.second.push_op(nextoff, LINK_FROM, WRITE_BLOCKS, tid);
          //   } else {
          //     t.first.push_op(off, UNLINK_FROM, READ_BLOCKS, tid);
          //     t.second.push_op(nextoff, LINK_FROM, READ_BLOCKS, tid);
          //   }
          // }
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
          break;
        }
      }
    }

    size_t boff = block->info.load(std::memory_order_acquire).offset;
    // while (idx >= boff + Capacity) {
    //   TMC_CPU_PAUSE();
    //   boff = block->info.load(std::memory_order_acquire).offset;
    // }
    assert(idx >= boff && idx < boff + Capacity);
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

  void dump_ops(tracker* track) {
    for (size_t i = 0; i < track->ops.size(); ++i) {
      operation& op = track->ops[i];
      const char* opName;
      switch (op.op) {
      case ALLOC_RAW:
        opName = "ALLOC_RAW";
        break;
      case ALLOC_LIST:
        opName = "ALLOC_LIST";
        break;
      case FREE_RAW:
        opName = "FREE_RAW";
        break;
      case FREE_LIST:
        opName = "FREE_LIST";
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
      std::printf(
        "%-2zu | %-12s: %p %zu\n", op.tid, opName, op.target, op.offset
      );
    }
  }

  ~ticket_queue() {
    // drain_sync();
    blocks_lock.spin_lock();
    block_list blocks = all_blocks.load(std::memory_order_acquire);
    data_block* block = blocks.next;
    size_t idx = blocks.offset;
    // std::unordered_set<data_block*> freed_block_set;
    while (block != nullptr) {
      block_list next = block->info.load(std::memory_order_acquire);
      if (idx != next.offset) {
        std::printf("idx %zu != offset %zu\n", idx, next.offset);
      }
      delete block;
      // freed_block_set.emplace(block);
      block = next.next;
      idx = idx + Capacity;
    }
    // freelist_lock.spin_lock();
    // for (size_t i = 0; i < freelist.size(); ++i) {
    //   block = freelist[i];
    //   delete block;
    //   freed_block_set.emplace(block);
    // }
    // std::unordered_set<data_block*> leaked_block_set;
    // for (auto it = blocks_map.begin(); it != blocks_map.end(); ++it) {
    //   if (!freed_block_set.contains(it->first)) {
    //     leaked_block_set.emplace(it->first);
    //   }
    // }
    // if (!leaked_block_set.empty()) {
    //   for (auto it = leaked_block_set.begin(); it !=
    //   leaked_block_set.end();
    //        ++it) {
    //     data_block* leak = *it;
    //     tracker* track = blocks_map[leak];
    //     // tracker* track = it->second;
    //     std::printf(
    //       "leaked: %p  %zu\n", leak,
    //       leak->info.load(std::memory_order_acquire).offset
    //     );

    //     dump_ops(track);

    //     operation lastOp = track->ops.back();
    //     if (lastOp.op != LINK_FROM) {
    //       std::printf("UNKNOWN FINAL OPERATION\n");
    //       std::terminate();
    //     }
    //     if (!blocks_map.contains(lastOp.target)) {
    //       std::printf("UNKNOWN FINAL TARGET %p\n", lastOp.target);
    //       std::terminate();
    //     }

    //     std::printf("\nFINAL LINK_FROM NEIGHBOR LOG (%p)\n",
    //     lastOp.target); dump_ops(blocks_map[lastOp.target]);
    //     std::fflush(stdout);

    //     std::terminate();
    //   }
    // }
    // for (auto it = blocks_map.begin(); it != blocks_map.end(); ++it) {
    //   tracker* tr = it->second;
    //   delete tr;
    // }
  }

  ticket_queue(const ticket_queue&) = delete;
  ticket_queue& operator=(const ticket_queue&) = delete;
  // TODO implement move constructor
};

class alignas(64) hazard_pointer {
  std::atomic<hazard_pointer*> next;
  std::atomic<void*> target;
  std::atomic<bool> has_owner;
};

/// Handles share ownership of a queue by reference counting.
/// Access to the queue (from multiple handles) is thread-safe,
/// but access to a single handle from multiple threads is not.
/// To access the queue from multiple threads or tasks concurrently,
/// make a copy of the handle for each.
template <typename T, size_t Size> class queue_handle {
  using queue_t = ticket_queue<T, Size>;
  std::shared_ptr<queue_t> q;
  hazard_pointer* hazptr;
  NO_CONCURRENT_ACCESS_LOCK;
  friend queue_t;
  queue_handle(std::shared_ptr<queue_t>&& QIn)
      : q{std::move(QIn)}, hazptr{nullptr} {}

public:
  static inline queue_handle make() {
    return queue_handle{std::make_shared<queue_t>()};
  }
  queue_handle(const queue_handle& Other) : q(Other.q), hazptr{nullptr} {}
  queue_handle& operator=(const queue_handle& Other) {
    q = Other.q;
    hazptr = nullptr;
  }

  template <typename U> queue_error push(U&& u) {
    ASSERT_NO_CONCURRENT_ACCESS();
    return q->template push<U>(std::forward<U>(u));
  }

  // May return a value or CLOSED
  queue_t::template aw_ticket_queue_pull<false> pull() {
    ASSERT_NO_CONCURRENT_ACCESS();
    return q->pull();
  }

  void close() {
    ASSERT_NO_CONCURRENT_ACCESS();
    q->close();
  }

  void drain_sync() {
    ASSERT_NO_CONCURRENT_ACCESS();
    q->drain_sync();
  }
  // TODO make queue call close() and drain_sync() in its destructor
};

} // namespace tmc
