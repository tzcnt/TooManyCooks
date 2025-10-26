// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

// Provides tmc::channel, an async MPMC unbounded linearizable queue.

// A channel can be created by tmc::make_channel<T>().
// Producers enqueue values with co_await push() or post().
// Consumers retrieve values in FIFO order with co_await pull().
// If no values are available, the consumer will suspend until a value is ready.

// Access to the channel is through a token `chan_tok` which shares ownership of
// the channel through reference counting, as well as holds a hazard pointer to
// the block in use. Any number of tokens can access the channel simultaneously,
// but access to a single token is not thread-safe. A token copy should be
// created (using the token copy constructor) for each thread or task that will
// access the channel concurrently.

// The hazard pointer scheme is loosely based on
// 'A wait-free queue as fast as fetch-and-add' by Yang & Mellor-Crummey
// https://dl.acm.org/doi/10.1145/2851141.2851168

#include "tmc/current.hpp"
#include "tmc/detail/compat.hpp"
#include "tmc/detail/concepts_awaitable.hpp"
#include "tmc/detail/tiny_lock.hpp"
#include "tmc/ex_any.hpp"
#include "tmc/task.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <coroutine>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace tmc {
namespace detail {
// Allocates elements without constructing them, to be constructed later using
// placement new. T need not be default, copy, or move constructible.
// The caller must track whether the element exists, and manually invoke the
// destructor if necessary.
template <typename T> struct channel_storage {
  union alignas(alignof(T)) {
    T value;
  };
#ifndef NDEBUG
  bool exists = false;
#endif

  channel_storage() noexcept {}

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
  channel_storage(channel_storage&& Other) noexcept {
    emplace(static_cast<T&&>(Other.value));
    Other.destroy();
  }
  channel_storage& operator=(channel_storage&& Other) noexcept {
    emplace(static_cast<T&&>(Other.value));
    Other.destroy();
    return *this;
  }

  // If data was present, the caller is responsible for destroying it.
#ifndef NDEBUG
  ~channel_storage() { assert(!exists); }
#else
  ~channel_storage()
    requires(std::is_trivially_destructible_v<T>)
  = default;
  ~channel_storage()
    requires(!std::is_trivially_destructible_v<T>)
  {}
#endif

  channel_storage(const channel_storage&) = delete;
  channel_storage& operator=(const channel_storage&) = delete;
};
} // namespace detail

struct chan_default_config {
  /// The number of elements that can be stored in each block in the channel
  /// linked list.
  static inline constexpr size_t BlockSize = 4096;

  /// At level 0, queue elements will be padded up to the next increment of 64
  /// bytes. This reduces false sharing between neighboring elements.
  /// At level 1, no padding will be applied.
  /// At level 2, no padding will be applied, and the flags value will be
  /// combined with the consumer pointer.
  static inline constexpr size_t PackingLevel = 0;

  /// If true, the first storage block will be a member of the channel object
  /// (instead of dynamically allocated). Subsequent storage blocks are always
  /// dynamically allocated. Incompatible with set_reuse_blocks(false).
  static inline constexpr bool EmbedFirstBlock = false;
};

/// Tokens share ownership of a channel by reference counting.
/// Access to the channel (from multiple tokens) is thread-safe,
/// but access to a single token from multiple threads is not.
/// To access the channel from multiple threads or tasks concurrently,
/// make a copy of the token for each (by using the copy constructor).
template <typename T, typename Config = tmc::chan_default_config>
class chan_tok;

/// Creates a new channel and returns an access token to it.
template <typename T, typename Config = tmc::chan_default_config>
inline chan_tok<T, Config> make_channel() noexcept;

struct chan_err {
  enum value { OK = 0u, EMPTY = 1u, CLOSED = 2u };
};

template <typename T, typename Config = tmc::chan_default_config>
class channel {
  static inline constexpr size_t BlockSize = Config::BlockSize;
  static inline constexpr size_t BlockSizeMask = BlockSize - 1;
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

  // An offset far enough forward that it won't protect anything for a very long
  // time, but close enough that it isn't considered "circular less than" 0.
  // On 32 bit this is only 1Gi elements. The worst case is that a
  // thread suspends for a very long time, and the queue processes 1Gi
  // elements and then cannot free any blocks until that thread wakes. This is
  // extremely unlikely, and not an error - it will just prevent block
  // reclamation. On 64 bit in practice this will never happen.
  static inline constexpr size_t InactiveHazptrOffset =
    TMC_ONE_BIT << (TMC_PLATFORM_BITS - 2);

  // Defaults to 2M items per second; this is 1 item every 500ns.
  static inline constexpr size_t DefaultHeavyLoadThreshold = 2000000;

  // The number of elements that will be produced or consumed by a token before
  // it checks if it should run the clustering algorithm.
  // At the default heavy load threshold, this results in running the clustering
  // algorithm every 5ms.
  static inline constexpr size_t ClusterPeriod = 10000;

  friend chan_tok<T, Config>;
  template <typename Tc, typename Cc>
  friend chan_tok<Tc, Cc> make_channel() noexcept;

public:
  class aw_pull;
  class aw_push;

private:
  // The API of this class is a bit unusual, in order to match packed_element_t
  // (which can efficiently access both flags and consumer at the same time).
  class element_t {
    static inline constexpr size_t DATA_BIT = TMC_ONE_BIT;
    static inline constexpr size_t CONS_BIT = TMC_ONE_BIT << 1;
    static inline constexpr size_t BOTH_BITS = DATA_BIT | CONS_BIT;
    std::atomic<size_t> flags;
    aw_pull::aw_pull_impl* consumer;

  public:
    tmc::detail::channel_storage<T> data;

    static constexpr size_t UNPADLEN =
      sizeof(size_t) + sizeof(void*) + sizeof(tmc::detail::channel_storage<T>);
    static constexpr size_t WANTLEN =
      (UNPADLEN + 63) & static_cast<size_t>(-64); // round up to 64
    static constexpr size_t PADLEN =
      UNPADLEN < WANTLEN ? (WANTLEN - UNPADLEN) : 999;

    struct empty {};
    using Padding = std::conditional_t<
      Config::PackingLevel == 0 && PADLEN != 999, char[PADLEN], empty>;
    TMC_NO_UNIQUE_ADDRESS Padding pad;

    // If this returns false, data is ready and consumer should not wait.
    bool try_wait(aw_pull::aw_pull_impl* Cons) noexcept {
      consumer = Cons;
      size_t expected = 0;
      return flags.compare_exchange_strong(
        expected, CONS_BIT, std::memory_order_acq_rel, std::memory_order_acquire
      );
    }

    aw_pull::aw_pull_impl* try_get_waiting_consumer() noexcept {
      size_t f = flags.load(std::memory_order_acquire);
      if (0 == (CONS_BIT & f)) {
        return nullptr;
      } else {
        return consumer;
      }
    }

    // Sets the data ready flag,
    // or returns a consumer pointer if that consumer was already waiting.
    aw_pull::aw_pull_impl* set_data_ready_or_get_waiting_consumer() noexcept {
      uintptr_t expected = 0;
      if (flags.compare_exchange_strong(
            expected, DATA_BIT, std::memory_order_acq_rel,
            std::memory_order_acquire
          )) {
        return nullptr;
      } else {
        return consumer;
      }
    }

    // Called by drain()
    aw_pull::aw_pull_impl* spin_wait_for_waiting_consumer() noexcept {
      // Wait for consumer to appear
      size_t f = flags.load(std::memory_order_acquire);
      while (0 == (CONS_BIT & f)) {
        TMC_CPU_PAUSE();
        f = flags.load(std::memory_order_acquire);
      }

      // The consumer may have seen the closed flag and did not wait.
      // Otherwise, return the waiting consumer.
      if (BOTH_BITS == f) {
        return nullptr;
      } else {
        return consumer;
      }
    }

    bool is_data_waiting() noexcept {
      return DATA_BIT == flags.load(std::memory_order_acquire);
    }

    bool is_done() noexcept {
      return BOTH_BITS == flags.load(std::memory_order_acquire);
    }

    void set_done() noexcept {
      flags.store(BOTH_BITS, std::memory_order_release);
    }

    void reset() noexcept { flags.store(0, std::memory_order_relaxed); }
  };

  // Same API as element_t
  struct packed_element_t {
    static inline constexpr uintptr_t DATA_BIT = TMC_ONE_BIT;
    static inline constexpr uintptr_t CONS_BIT = TMC_ONE_BIT << 1;
    static inline constexpr uintptr_t BOTH_BITS = DATA_BIT | CONS_BIT;
    std::atomic<void*> flags;

  public:
    tmc::detail::channel_storage<T> data;

    // If this returns false, data is ready and consumer should not wait.
    bool try_wait(aw_pull::aw_pull_impl* Cons) noexcept {
      void* expected = nullptr;
      return flags.compare_exchange_strong(
        expected, static_cast<void*>(Cons), std::memory_order_acq_rel,
        std::memory_order_acquire
      );
    }

    aw_pull::aw_pull_impl* try_get_waiting_consumer() noexcept {
      void* f = flags.load(std::memory_order_acquire);
      return static_cast<aw_pull::aw_pull_impl*>(f);
    }

    // Sets the data ready flag,
    // or returns a consumer pointer if that consumer was already waiting.
    aw_pull::aw_pull_impl* set_data_ready_or_get_waiting_consumer() noexcept {
      void* expected = nullptr;
      if (flags.compare_exchange_strong(
            expected, reinterpret_cast<void*>(DATA_BIT),
            std::memory_order_acq_rel, std::memory_order_acquire
          )) {
        return nullptr;
      } else {
        return static_cast<aw_pull::aw_pull_impl*>(expected);
      }
    }

    // Called by drain()
    aw_pull::aw_pull_impl* spin_wait_for_waiting_consumer() noexcept {
      // Wait for consumer to appear
      void* f = flags.load(std::memory_order_acquire);
      while (nullptr == f) {
        TMC_CPU_PAUSE();
        f = flags.load(std::memory_order_acquire);
      }

      // The consumer may have seen the closed flag and did not wait.
      // Otherwise, return the waiting consumer.
      if (BOTH_BITS == reinterpret_cast<uintptr_t>(f)) {
        return nullptr;
      } else {
        return static_cast<aw_pull::aw_pull_impl*>(f);
      }
    }

    bool is_data_waiting() noexcept {
      void* f = flags.load(std::memory_order_acquire);
      return DATA_BIT == reinterpret_cast<uintptr_t>(f);
    }

    bool is_done() noexcept {
      void* f = flags.load(std::memory_order_acquire);
      return BOTH_BITS == reinterpret_cast<uintptr_t>(f);
    }

    void set_done() noexcept {
      // Clear the consumer pointer
      flags.store(
        reinterpret_cast<void*>(BOTH_BITS), std::memory_order_release
      );
    }

    void reset() noexcept {
      // Clear the consumer pointer
      flags.store(nullptr, std::memory_order_relaxed);
    }
  };

  using element =
    std::conditional_t < Config::PackingLevel<2, element_t, packed_element_t>;
  static_assert(
    Config::PackingLevel < 2 || TMC_PLATFORM_BITS == 64,
    "Packing level 2 requires 64-bit mode due to the use of pointer tagging."
  );

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

  class alignas(64) hazard_ptr {
    std::atomic<bool> owned;
    std::atomic<hazard_ptr*> next;
    std::atomic<size_t> active_offset;
    std::atomic<data_block*> write_block;
    std::atomic<data_block*> read_block;
    std::atomic<size_t> read_count;
    std::atomic<size_t> write_count;
    std::atomic<int> thread_index;
    std::atomic<int> requested_thread_index;
    std::atomic<size_t> next_protect_write;
    std::atomic<size_t> next_protect_read;
    size_t lastTimestamp;
    size_t minCycles;

    friend class channel;

    void release_blocks() noexcept {
      // These elements may be read (by try_reclaim_block()) after
      // take_ownership() has been called, but before init() has been called.
      // These defaults ensure sane behavior.
      write_block.store(nullptr, std::memory_order_relaxed);
      read_block.store(nullptr, std::memory_order_relaxed);
    }

    hazard_ptr() noexcept {
      thread_index.store(
        static_cast<int>(tmc::current_thread_index()), std::memory_order_relaxed
      );
      active_offset.store(InactiveHazptrOffset, std::memory_order_relaxed);
      release_blocks();
    }

    void init(data_block* head, size_t MinCycles) noexcept {
      thread_index.store(
        static_cast<int>(tmc::current_thread_index()), std::memory_order_relaxed
      );
      requested_thread_index.store(-1, std::memory_order_relaxed);
      read_count.store(0, std::memory_order_relaxed);
      write_count.store(0, std::memory_order_relaxed);
      size_t headOff = head->offset.load(std::memory_order_relaxed);
      next_protect_write.store(headOff);
      next_protect_read.store(headOff);
      active_offset.store(
        headOff + InactiveHazptrOffset, std::memory_order_relaxed
      );
      read_block.store(head, std::memory_order_relaxed);
      write_block.store(head, std::memory_order_relaxed);

      lastTimestamp = TMC_CPU_TIMESTAMP();
      minCycles = MinCycles;
    }

    bool should_suspend() noexcept {
      return write_count + read_count >= ClusterPeriod;
    }

    size_t elapsed() noexcept {
      size_t currTimestamp = TMC_CPU_TIMESTAMP();
      size_t elapsed = currTimestamp - lastTimestamp;
      lastTimestamp = currTimestamp;
      return elapsed;
    }

    bool try_take_ownership() noexcept {
      bool expected = false;
      return owned.compare_exchange_strong(expected, true);
    }

    void inc_read_count() noexcept {
      auto count = read_count.load(std::memory_order_relaxed);
      read_count.store(count + 1, std::memory_order_relaxed);
      thread_index.store(
        static_cast<int>(tmc::current_thread_index()), std::memory_order_relaxed
      );
    }

    void inc_write_count() noexcept {
      auto count = write_count.load(std::memory_order_relaxed);
      write_count.store(count + 1, std::memory_order_relaxed);
      thread_index.store(
        static_cast<int>(tmc::current_thread_index()), std::memory_order_relaxed
      );
    }

    template <typename Pred, typename Func>
    void for_each_owned_hazptr(Pred pred, Func func) noexcept {
      hazard_ptr* curr = this;
      while (pred()) {
        hazard_ptr* n = curr->next.load(std::memory_order_acquire);
        bool is_owned = curr->owned.load(std::memory_order_relaxed);
        if (is_owned) {
          func(curr);
        }
        if (n == this) {
          break;
        }
        curr = n;
      }
    }

  public:
    /// Returns the hazard pointer back to the hazard pointer freelist, so that
    /// it can be reused by another thread or task.
    void release_ownership() noexcept {
      release_blocks();
      owned.store(false);
    }
  };

  static_assert(std::atomic<size_t>::is_always_lock_free);
  static_assert(std::atomic<data_block*>::is_always_lock_free);

  static inline constexpr size_t WRITE_CLOSING_BIT = TMC_ONE_BIT;
  static inline constexpr size_t WRITE_CLOSED_BIT = TMC_ONE_BIT << 1;
  static inline constexpr size_t READ_CLOSED_BIT = TMC_ONE_BIT << 2;
  static inline constexpr size_t ALL_CLOSED_BITS = (TMC_ONE_BIT << 3) - 1;

  // Infrequently modified values can share a cache line.
  // Written by drain() / close()
  alignas(64) std::atomic<size_t> closed;
  std::atomic<size_t> write_closed_at;
  std::atomic<size_t> read_closed_at;

  // Written by get_hazard_ptr()
  std::atomic<size_t> haz_ptr_counter;
  std::atomic<hazard_ptr*> hazard_ptr_list;

  // Written by set_*() configuration functions
  std::atomic<size_t> ReuseBlocks;
  std::atomic<size_t> MinClusterCycles;
  std::atomic<size_t> ConsumerSpins;
  char pad0[64];
  std::atomic<size_t> write_offset;
  char pad1[64];
  std::atomic<size_t> read_offset;
  char pad2[64];
  // Blocks try_cluster(). Use tiny_lock since only try_lock() is called.
  tmc::tiny_lock cluster_lock;

  // Blocks try_reclaim_blocks(), close(), and drain().
  // Rarely blocks get_hazard_ptr() - if racing with try_reclaim_blocks().
  alignas(64) std::mutex blocks_lock;
  std::atomic<size_t> reclaim_counter;
  std::atomic<data_block*> head_block;
  std::atomic<data_block*> tail_block;

  struct empty {};
  using EmbeddedBlock =
    std::conditional_t<Config::EmbedFirstBlock, data_block, empty>;
  TMC_NO_UNIQUE_ADDRESS EmbeddedBlock embedded_block;

  channel() noexcept {
    closed.store(0, std::memory_order_relaxed);
    write_closed_at.store(0, std::memory_order_relaxed);
    read_closed_at.store(0, std::memory_order_relaxed);

    data_block* block;
    if constexpr (Config::EmbedFirstBlock) {
      block = &embedded_block;
    } else {
      block = new data_block(0);
    }
    head_block.store(block, std::memory_order_relaxed);
    tail_block.store(block, std::memory_order_relaxed);
    read_offset.store(0, std::memory_order_relaxed);
    write_offset.store(0, std::memory_order_relaxed);

    ReuseBlocks.store(true, std::memory_order_relaxed);
    MinClusterCycles.store(
      TMC_CPU_FREQ / (DefaultHeavyLoadThreshold / ClusterPeriod),
      std::memory_order_relaxed
    );
    ConsumerSpins.store(0, std::memory_order_relaxed);

    haz_ptr_counter.store(0, std::memory_order_relaxed);
    reclaim_counter.store(0, std::memory_order_relaxed);
    hazard_ptr* haz = new hazard_ptr;
    haz->next.store(haz, std::memory_order_relaxed);
    haz->owned.store(false, std::memory_order_relaxed);
    hazard_ptr_list.store(haz, std::memory_order_relaxed);
    tmc::detail::memory_barrier();
  }

  struct cluster_data {
    int destination;
    hazard_ptr* id;
  };

  // Uses an extremely simple algorithm to determine the best thread to assign
  // workers to.
  static inline void cluster(std::vector<cluster_data>& ClusterOn) noexcept {
    if (ClusterOn.size() == 0) {
      return;
    }
    // Using the average is a hack - it would be better to determine
    // which group already has the most active tasks in it.
    int avg = 0;
    for (size_t i = 0; i < ClusterOn.size(); ++i) {
      avg += ClusterOn[i].destination;
    }
    avg /= static_cast<int>(ClusterOn.size()); // integer division, yuck

    // Find the tid that is the closest to the average.
    // This becomes the clustering point.
    int minDiff = std::numeric_limits<int>::max();
    int closest = 0;
    for (size_t i = 0; i < ClusterOn.size(); ++i) {
      int tid = ClusterOn[i].destination;
      int diff;
      if (tid >= avg) {
        diff = tid - avg;
      } else {
        diff = avg - tid;
      }
      if (diff < minDiff) {
        diff = minDiff;
        closest = tid;
      }
    }

    for (size_t i = 0; i < ClusterOn.size(); ++i) {
      ClusterOn[i].id->requested_thread_index.store(
        closest, std::memory_order_relaxed
      );
    }
  }

  // Tries to move producers and closers near each other.
  // Returns true if this thread ran the clustering algorithm, or if another
  // thread already ran the clustering algorithm and the result is ready.
  // Returns false if another thread is currently running the clustering
  // algorithm.
  bool try_cluster(hazard_ptr* Haz) noexcept {
    if (!cluster_lock.try_lock()) {
      return false;
    }
    int rti = Haz->requested_thread_index.load(std::memory_order_relaxed);
    if (rti != -1) {
      // Another thread already calculated rti for us
      cluster_lock.unlock();
      return true;
    }
    std::vector<cluster_data> reader;
    std::vector<cluster_data> writer;
    std::vector<cluster_data> both;
    reader.reserve(64);
    writer.reserve(64);
    Haz->for_each_owned_hazptr(
      []() { return true; },
      [&](hazard_ptr* curr) {
        size_t reads = curr->read_count.load(std::memory_order_relaxed);
        size_t writes = curr->write_count.load(std::memory_order_relaxed);
        int tid = curr->thread_index.load(std::memory_order_relaxed);
        if (writes == 0) {
          if (reads != 0) {
            reader.emplace_back(tid, curr);
          }
        } else {
          if (reads == 0) {
            writer.emplace_back(tid, curr);
          } else {
            both.emplace_back(tid, curr);
          }
        }
      }
    );

    if (writer.size() + reader.size() + both.size() <= 4) {
      // Cluster small numbers of workers together
      for (size_t i = 0; i < reader.size(); ++i) {
        writer.push_back(reader[i]);
      }
      for (size_t i = 0; i < both.size(); ++i) {
        writer.push_back(both[i]);
      }
      cluster(writer);
    } else {
      // Separate clusters for each kind of worker
      cluster(writer);
      cluster(reader);
      cluster(both);
    }

    cluster_lock.unlock();
    return true;
  }

  hazard_ptr* get_hazard_ptr_impl() noexcept {
    hazard_ptr* start = hazard_ptr_list.load(std::memory_order_relaxed);
    hazard_ptr* ptr = start;
    while (true) {
      hazard_ptr* next = ptr->next.load(std::memory_order_acquire);
      bool is_owned = ptr->owned.load(std::memory_order_relaxed);
      if ((is_owned == false) && ptr->try_take_ownership()) {
        break;
      }
      if (next == start) {
        hazard_ptr* newptr = new hazard_ptr;
        newptr->owned.store(true, std::memory_order_relaxed);
        do {
          newptr->next.store(next, std::memory_order_release);
        } while (!ptr->next.compare_exchange_strong(
          next, newptr, std::memory_order_acq_rel, std::memory_order_acquire
        ));
        ptr = newptr;
        break;
      }
      ptr = next;
    }
    return ptr;
  }

  static inline bool circular_less_than(size_t a, size_t b) noexcept {
    return a - b > (TMC_ONE_BIT << (TMC_PLATFORM_BITS - 1));
  }

  // Load src and move it into dst if src < dst.
  static inline void
  keep_min(size_t& Dst, std::atomic<size_t> const& Src) noexcept {
    size_t val = Src.load(std::memory_order_acquire);
    if (circular_less_than(val, Dst)) {
      Dst = val;
    }
  }

  // Move src into dst if src < dst.
  static inline void keep_min(size_t& Dst, size_t Src) noexcept {
    if (circular_less_than(Src, Dst)) {
      Dst = Src;
    }
  }

  // Advances DstBlock to be equal to NewHead. Possibly reduces MinProtect if
  // DstBlock was already updated by its owning thread.
  static inline void try_advance_hazptr_block(
    std::atomic<data_block*>& DstBlock, size_t& MinProtected,
    data_block* NewHead, std::atomic<size_t> const& HazardOffset
  ) noexcept {
    data_block* block = DstBlock.load(std::memory_order_acquire);
    if (block == nullptr) {
      // A newly owned hazptr. It will reload the value of head after this
      // reclaim operation completes, or cause the entire reclaim operation to
      // be abandoned. In either case, we don't need to update it here.
      // May also be a newly released hazptr, in which case we don't want to
      // overwrite the value of block either.
      return;
    }
    if (circular_less_than(
          block->offset.load(std::memory_order_relaxed),
          NewHead->offset.load(std::memory_order_relaxed)
        )) {
      if (!DstBlock.compare_exchange_strong(
            block, NewHead, std::memory_order_seq_cst
          )) {
        if (block == nullptr) {
          // A newly released hazptr.
          return;
        }
        // If this hazptr updated its own block, but the updated block is
        // still earlier than the new head, then we cannot free that block.
        keep_min(MinProtected, block->offset.load(std::memory_order_relaxed));
      }
      // Reload hazptr after trying to modify block to ensure that if it was
      // written, its value is seen.
      keep_min(MinProtected, HazardOffset);
    }
  }

  // Starting from OldHead, advance forward through the block list, stopping at
  // the first block that is protected by a hazard pointer. This block is
  // returned to become the NewHead. If OldHead is protected, then it will be
  // returned unchanged, and no blocks can be reclaimed.
  data_block* try_advance_head(
    hazard_ptr* Haz, data_block* OldHead, size_t ProtectIdx
  ) noexcept {
    // In the current implementation, this is called only from consumers.
    // Therefore, this token's hazptr will be active, and protecting read_block.
    // However, if producers are lagging behind, and no producer is currently
    // active, write_block would not be protected. Therefore, write_offset
    // should be passed to ProtectIdx to cover this scenario.
    ProtectIdx = ProtectIdx & ~BlockSizeMask; // round down to block index

    // Find the lowest offset that is protected by ProtectIdx or any hazptr.
    size_t oldOff = OldHead->offset.load(std::memory_order_relaxed);
    Haz->for_each_owned_hazptr(
      [&]() { return circular_less_than(oldOff, ProtectIdx); },
      [&](hazard_ptr* curr) { keep_min(ProtectIdx, curr->active_offset); }
    );

    // If head block is protected, nothing can be reclaimed.
    if (circular_less_than(ProtectIdx, 1 + oldOff)) {
      return OldHead;
    }

    // Find the block associated with this offset.
    data_block* newHead = OldHead;
    while (circular_less_than(
      newHead->offset.load(std::memory_order_relaxed), ProtectIdx
    )) {
      newHead = newHead->next.load(std::memory_order_acquire);
    }

    // Then update all hazptrs to be at this block or later.
    Haz->for_each_owned_hazptr(
      [&]() { return circular_less_than(oldOff, ProtectIdx); },
      [&](hazard_ptr* curr) {
        try_advance_hazptr_block(
          curr->write_block, ProtectIdx, newHead, curr->active_offset
        );
        try_advance_hazptr_block(
          curr->read_block, ProtectIdx, newHead, curr->active_offset
        );
      }
    );

    // ProtectIdx may have been reduced by the double-check in
    // try_advance_block. If so, reduce newHead as well.
    if (circular_less_than(
          ProtectIdx, newHead->offset.load(std::memory_order_relaxed)
        )) {
      newHead = OldHead;
      while (circular_less_than(
        newHead->offset.load(std::memory_order_relaxed), ProtectIdx
      )) {
        newHead = newHead->next;
      }
    }

#ifndef NDEBUG
    assert(circular_less_than(
      newHead->offset.load(std::memory_order_relaxed),
      1 + read_offset.load(std::memory_order_acquire)
    ));
    assert(circular_less_than(
      newHead->offset.load(std::memory_order_relaxed),
      1 + write_offset.load(std::memory_order_acquire)
    ));
#endif
    return newHead;
  }

  void reclaim_blocks(data_block* OldHead, data_block* NewHead) noexcept {
    if (!ReuseBlocks.load(std::memory_order_relaxed)) {
      while (OldHead != NewHead) {
        data_block* next = OldHead->next.load(std::memory_order_relaxed);
        delete OldHead;
        OldHead = next;
      }
    } else {
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

        data_block* tailBlock = tail_block.load(std::memory_order_acquire);
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

        tail_block.store(unlinked[unlinkedCount - 1]);
      }
    }
  }

  // Access to this function must be externally synchronized (via blocks_lock).
  // Blocks that are not protected by a hazard pointer will be reclaimed, and
  // head_block will be advanced to the first protected block.
  void try_reclaim_blocks(hazard_ptr* Haz, size_t ProtectIdx) noexcept {
    data_block* oldHead = head_block.load(std::memory_order_acquire);
    // reclaim_counter and haz_ptr_counter behave as a split lock shared with
    // get_hazard_ptr(). If both operations run at the same time, this will
    // abandon its operation before the final stage.
    size_t hazptrCount = haz_ptr_counter.load(std::memory_order_acquire);

    // Perform the private stage of the operation.
    data_block* newHead = try_advance_head(Haz, oldHead, ProtectIdx);
    if (newHead == oldHead) {
      return;
    }
    head_block.store(newHead, std::memory_order_release);

    // Signal to get_hazard_ptr() that we updated head_block.
    reclaim_counter.fetch_add(1, std::memory_order_seq_cst);

    // Check if get_hazard_ptr() was running.
    size_t hazptrCheck = haz_ptr_counter.load(std::memory_order_seq_cst);
    if (hazptrCount != hazptrCheck) {
      // A hazard pointer was acquired during try_advance_head().
      // It may have an outdated value of head. Our options are to run
      // try_advance_head() again, or just abandon (rollback) the operation. For
      // now, I've chosen to abandon the operation. This will run again when the
      // next block is allocated.
      head_block.store(oldHead, std::memory_order_release);
      return;
    }
    reclaim_blocks(oldHead, newHead);
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

  // Idx will be initialized by this function
  element* get_write_ticket(hazard_ptr* Haz, size_t& Idx) noexcept {
    size_t actOff = Haz->next_protect_write.load(std::memory_order_relaxed);
    Haz->active_offset.store(actOff, std::memory_order_relaxed);

    // seq_cst is needed here to create a StoreLoad barrier between setting
    // hazptr and loading the block
    Idx = write_offset.fetch_add(1, std::memory_order_seq_cst);
    data_block* block = Haz->write_block.load(std::memory_order_seq_cst);

    [[maybe_unused]] size_t boff =
      block->offset.load(std::memory_order_relaxed);
    assert(circular_less_than(actOff, 1 + Idx));
    assert(circular_less_than(boff, 1 + Idx));

    // close() will set `closed` before incrementing write_offset.
    // Thus we are guaranteed to see it if we acquire offset first (our Idx will
    // be past write_closed_at).
    //
    // We may also see it earlier than that, in which case we should not return
    // early (our Idx is less than write_closed_at).
    auto closedState = closed.load(std::memory_order_acquire);
    if (0 != closedState) [[unlikely]] {
      // Wait for the write_closed_at index to become available.
      while (0 == (closedState & WRITE_CLOSED_BIT)) {
        TMC_CPU_PAUSE();
        closedState = closed.load(std::memory_order_acquire);
      }
      if (circular_less_than(
            write_closed_at.load(std::memory_order_relaxed), 1 + Idx
          )) {
        return nullptr;
      }
    }
    block = find_block(block, Idx);
    // Update last known block.
    Haz->write_block.store(block, std::memory_order_release);
    Haz->next_protect_write.store(boff, std::memory_order_relaxed);
    element* elem = &block->values[Idx & BlockSizeMask];
    return elem;
  }

  // StartIdx and EndIdx will be initialized by this function
  data_block* get_write_ticket_bulk(
    hazard_ptr* Haz, size_t Count, size_t& StartIdx, size_t& EndIdx
  ) noexcept {
    size_t actOff = Haz->next_protect_write.load(std::memory_order_relaxed);
    Haz->active_offset.store(actOff, std::memory_order_relaxed);

    // seq_cst is needed here to create a StoreLoad barrier between setting
    // hazptr and loading the block
    StartIdx = write_offset.fetch_add(Count, std::memory_order_seq_cst);
    EndIdx = StartIdx + Count;
    data_block* block = Haz->write_block.load(std::memory_order_seq_cst);

    [[maybe_unused]] size_t boff =
      block->offset.load(std::memory_order_relaxed);
    assert(circular_less_than(actOff, 1 + StartIdx));
    assert(circular_less_than(boff, 1 + StartIdx));

    // close() will set `closed` before incrementing write_offset.
    // Thus we are guaranteed to see it if we acquire offset first (our Idx will
    // be past write_closed_at).
    //
    // We may also see it earlier than that, in which case we should not return
    // early (our Idx is less than write_closed_at).
    auto closedState = closed.load(std::memory_order_acquire);
    if (0 != closedState) [[unlikely]] {
      // Wait for the write_closed_at index to become available.
      while (0 == (closedState & WRITE_CLOSED_BIT)) {
        TMC_CPU_PAUSE();
        closedState = closed.load(std::memory_order_acquire);
      }
      if (circular_less_than(
            write_closed_at.load(std::memory_order_relaxed), 1 + StartIdx
          )) {
        return nullptr;
      }
    }

    // Ensure all blocks for the operation are allocated and available.
    data_block* startBlock = find_block(block, StartIdx);

    data_block* protectBlock;
    if (StartIdx != EndIdx) [[likely]] {
      data_block* endBlock = find_block(startBlock, EndIdx - 1);
      protectBlock = endBlock;
    } else {
      // User passed an empty range, or Count == 0
      protectBlock = startBlock;
    }
    // Update last known block.
    Haz->write_block.store(protectBlock, std::memory_order_release);
    Haz->next_protect_write.store(
      protectBlock->offset.load(std::memory_order_relaxed),
      std::memory_order_relaxed
    );
    return startBlock;
  }

  // Idx will be initialized by this function
  element* get_read_ticket(hazard_ptr* Haz, size_t& Idx) noexcept {
    size_t actOff = Haz->next_protect_read.load(std::memory_order_relaxed);
    Haz->active_offset.store(actOff, std::memory_order_relaxed);

    // seq_cst is needed here to create a StoreLoad barrier between setting
    // hazptr and loading the block
    Idx = read_offset.fetch_add(1, std::memory_order_seq_cst);
    data_block* block = Haz->read_block.load(std::memory_order_seq_cst);

    [[maybe_unused]] size_t boff =
      block->offset.load(std::memory_order_relaxed);
    assert(circular_less_than(actOff, 1 + Idx));
    assert(circular_less_than(boff, 1 + Idx));

    // close() will set `closed` before incrementing read_offset.
    // Thus we are guaranteed to see it if we acquire offset first (our Idx
    // will be past read_closed_at).
    //
    // We may see closed earlier than that, in which case our index will be
    // between write_closed_at and read_closed_at. Make a best effort to return
    // early in this case.
    auto closedState = closed.load(std::memory_order_acquire);
    if (0 != closedState) [[unlikely]] {
      // Wait for the write_closed_at index to become available.
      while (0 == (closedState & WRITE_CLOSED_BIT)) {
        TMC_CPU_PAUSE();
        closedState = closed.load(std::memory_order_acquire);
      }
      // If closed, continue draining until the channel is empty.
      if (circular_less_than(
            write_closed_at.load(std::memory_order_relaxed), 1 + Idx
          )) {
        // After channel is empty, we still need to mark each element as
        // finished. This is a side effect of using fetch_add - we are still
        // consuming indexes even if they aren't used.
        block = find_block(block, Idx);
        element* elem = &block->values[Idx & BlockSizeMask];
        elem->set_done();
        return nullptr;
      }
    }
    block = find_block(block, Idx);
    // Update last known block.
    // Note that if hazptr was to an older block, that block will still be
    // protected (by active_offset). This prevents a channel consisting of a
    // single block from trying to unlink/link that block to itself.
    Haz->read_block.store(block, std::memory_order_release);
    Haz->next_protect_read.store(boff, std::memory_order_relaxed);
    // Try to reclaim old blocks. Checking for index 1 ensures that at least
    // this token's hazptr will already be advanced to the new block.
    // Only consumers participate in reclamation and only 1 consumer at a time.
    if ((Idx & BlockSizeMask) == 1 && blocks_lock.try_lock()) {
      size_t protectIdx = write_offset.load(std::memory_order_acquire);
      try_reclaim_blocks(Haz, protectIdx);
      blocks_lock.unlock();
    }
    element* elem = &block->values[Idx & BlockSizeMask];
    return elem;
  }

  template <typename U> void write_element(element* Elem, U&& Val) noexcept {
    auto cons = Elem->try_get_waiting_consumer();
    if (cons != nullptr) {
      // Still need to store so block can be freed
      Elem->set_done();
      cons->t.emplace(std::forward<U>(Val));
      tmc::detail::post_checked(
        cons->continuation_executor, std::move(cons->continuation), cons->prio
      );
      return;
    }

    // No consumer waiting, store the data
    Elem->data.emplace(std::forward<U>(Val));

    // Finalize transaction
    cons = Elem->set_data_ready_or_get_waiting_consumer();
    if (cons != nullptr) {
      // Consumer started waiting for this data during our RMW cycle
      cons->t = std::move(Elem->data);
      tmc::detail::post_checked(
        cons->continuation_executor, std::move(cons->continuation), cons->prio
      );
      Elem->set_done();
    }
  }

public:
  // Gets a hazard pointer from the list, and takes ownership of it.
  hazard_ptr* get_hazard_ptr() noexcept {
    // reclaim_counter and haz_ptr_counter behave as a split lock shared with
    // try_reclaim_blocks(). If both operations run at the same time, we may see
    // an outdated value of head, and will need to reload head.
    size_t reclaimCount = reclaim_counter.load(std::memory_order_acquire);

    // Perform the private stage of the operation.
    hazard_ptr* ptr = get_hazard_ptr_impl();

    size_t cycles = MinClusterCycles.load(std::memory_order_relaxed);
    // Reload head_block until try_reclaim_blocks was not running.
    size_t reclaimCheck;
    do {
      reclaimCheck = reclaimCount;
      ptr->init(head_block.load(std::memory_order_acquire), cycles);
      // Signal to try_reclaim_blocks() that we read the value of head_block.
      haz_ptr_counter.fetch_add(1, std::memory_order_seq_cst);
      // Check if try_reclaim_blocks() was running (again)
      reclaimCount = reclaim_counter.load(std::memory_order_seq_cst);
    } while (reclaimCount != reclaimCheck);
    return ptr;
  }

  template <typename U> bool post(hazard_ptr* Haz, U&& Val) noexcept {
    Haz->inc_write_count();
    // Get write ticket and associated block, protected by hazptr.
    size_t idx;
    element* elem = get_write_ticket(Haz, idx);
    if (elem == nullptr) [[unlikely]] {
      return false;
    }

    // Store the data / wake any waiting consumers
    write_element(elem, std::forward<U>(Val));

    // Then release the hazard pointer
    Haz->active_offset.store(
      idx + InactiveHazptrOffset, std::memory_order_release
    );

    return true;
  }

  template <typename It>
  bool post_bulk(hazard_ptr* Haz, It&& Items, size_t Count) noexcept {
    Haz->inc_write_count();
    // Get write ticket and associated block, protected by hazptr.
    size_t startIdx, endIdx;
    data_block* block = get_write_ticket_bulk(Haz, Count, startIdx, endIdx);
    if (block == nullptr) [[unlikely]] {
      return false;
    }

    size_t idx = startIdx;
    while (idx < endIdx) {
      element* elem = &block->values[idx & BlockSizeMask];
      // Store the data / wake any waiting consumers
      write_element(elem, std::move(*Items));
      ++Items;
      ++idx;
      if ((idx & BlockSizeMask) == 0) {
        block = block->next.load(std::memory_order_acquire);
        // all blocks should have been preallocated for [startIdx, endIdx)
        assert(block != nullptr || idx >= endIdx);
      }
    }

    // Then release the hazard pointer
    Haz->active_offset.store(
      endIdx + InactiveHazptrOffset, std::memory_order_release
    );

    return true;
  }

  class aw_pull : private tmc::detail::AwaitTagNoGroupCoAwait {
    channel& chan;
    hazard_ptr* haz_ptr;

    friend channel;
    friend chan_tok<T, Config>;

    aw_pull(channel& Chan, hazard_ptr* Haz) noexcept
        : chan(Chan), haz_ptr{Haz} {}

    struct aw_pull_impl {
      aw_pull& parent;
      size_t thread_hint;
      element* elem;
      size_t release_idx;
      bool ok;
      tmc::ex_any* continuation_executor;
      std::coroutine_handle<> continuation;
      size_t prio;
      tmc::detail::channel_storage<T> t;

      aw_pull_impl(aw_pull& Parent) noexcept
          : parent{Parent}, thread_hint(tmc::current_thread_index()), ok{true},
            continuation_executor{tmc::detail::this_thread::executor},
            continuation{nullptr},
            prio(tmc::detail::this_thread::this_task.prio) {}
      bool await_ready() noexcept {
        parent.haz_ptr->inc_read_count();
        // Get read ticket and associated block, protected by hazptr.
        size_t idx;
        elem = parent.chan.get_read_ticket(parent.haz_ptr, idx);
        release_idx = idx + InactiveHazptrOffset;
        if (elem == nullptr) [[unlikely]] {
          // The queue is closed and drained.
          ok = false;
          return true;
        }

        if (parent.haz_ptr->should_suspend()) [[unlikely]] {
          if (parent.haz_ptr->write_count + parent.haz_ptr->read_count ==
              ClusterPeriod) {
            size_t elapsed = parent.haz_ptr->elapsed();
            size_t readerCount = 0;
            parent.haz_ptr->for_each_owned_hazptr(
              [&]() { return true; },
              [&](hazard_ptr* curr) {
                auto reads = curr->read_count.load(std::memory_order_relaxed);
                if (reads != 0) {
                  ++readerCount;
                }
              }
            );

            if (elapsed >= parent.haz_ptr->minCycles * readerCount) {
              // Just suspend without rebalancing (to allow other consumers to
              // run)
              parent.haz_ptr->write_count.store(0, std::memory_order_relaxed);
              parent.haz_ptr->read_count.store(0, std::memory_order_relaxed);
              return false;
            }
          }
          // Try to get rti. Suspend if we can get it.
          // If we don't get it on this call to push(), don't suspend and try
          // again to get it on the next call.
          int rti = parent.haz_ptr->requested_thread_index.load(
            std::memory_order_relaxed
          );
          if (rti == -1) {
            if (parent.chan.try_cluster(parent.haz_ptr)) {
              rti = parent.haz_ptr->requested_thread_index.load(
                std::memory_order_relaxed
              );
            }
          }
          if (rti != -1) {
            parent.haz_ptr->write_count.store(0, std::memory_order_relaxed);
            parent.haz_ptr->read_count.store(0, std::memory_order_relaxed);
            return false;
          }
        }

        if (elem->is_data_waiting()) {
          // Data is already ready here.
          t = std::move(elem->data);
          // Still need to store so block can be freed
          elem->set_done();
          return true;
        }
        size_t spins =
          parent.chan.ConsumerSpins.load(std::memory_order_relaxed);
        for (size_t i = 0; i < spins; ++i) {
          TMC_CPU_PAUSE();
          if (elem->is_data_waiting()) {
            // Data is already ready here.
            t = std::move(elem->data);
            // Still need to store so block can be freed
            elem->set_done();
            return true;
          }
        }

        // If we suspend, hold on to the hazard pointer to keep the block alive
        return false;
      }
      bool await_suspend(std::coroutine_handle<> Outer) noexcept {
        int rti = parent.haz_ptr->requested_thread_index.load(
          std::memory_order_relaxed
        );
        if (rti != -1) {
          thread_hint = static_cast<size_t>(rti);
          parent.haz_ptr->requested_thread_index.store(
            -1, std::memory_order_relaxed
          );
        }

        continuation = Outer;
        if (!elem->try_wait(this)) {
          // data became ready during our RMW cycle
          t = std::move(elem->data);
          elem->set_done();
          if (thread_hint != NO_HINT) {
            // We are done with the block, but we are going to suspend anyway,
            // so release the block reference now so that it can be reclaimed by
            // another thread.
            parent.haz_ptr->active_offset.store(
              release_idx, std::memory_order_release
            );
            // Periodically suspend consumers to avoid starvation if producer is
            // running in same node
            tmc::detail::post_checked(
              continuation_executor, std::move(continuation), prio, thread_hint
            );
            return true;
          }
          return false;
        }
        return true;
      }

      std::optional<T> await_resume() noexcept {
        parent.haz_ptr->active_offset.store(
          release_idx, std::memory_order_release
        );
        if (ok) {
          std::optional<T> result(std::move(t.value));
          t.destroy();
          return result;
        } else {
          // The queue is closed and drained.
          return std::nullopt;
        }
      }
    };

  public:
    aw_pull_impl operator co_await() && noexcept { return aw_pull_impl(*this); }
  };

  std::variant<T, std::monostate, std::monostate> try_pull(hazard_ptr* Haz) {
    Haz->inc_read_count();
    // Get read ticket and associated block, protected by hazptr.
    size_t actOff = Haz->next_protect_read.load(std::memory_order_relaxed);

    // seq_cst is needed here to create a StoreLoad barrier between setting
    // hazptr and loading the block
    Haz->active_offset.store(actOff, std::memory_order_seq_cst);

    size_t Idx = read_offset.load(std::memory_order_seq_cst);
    while (true) {
      auto woff = write_offset.load(std::memory_order_relaxed);
      // If woff <= roff, the queue appears empty.
      if (circular_less_than(woff, Idx + 1)) {
        auto closedState = closed.load(std::memory_order_acquire);
        if (0 != closedState) [[unlikely]] {
          // Wait for the write_closed_at index to become available.
          while (0 == (closedState & WRITE_CLOSED_BIT)) {
            TMC_CPU_PAUSE();
            closedState = closed.load(std::memory_order_acquire);
          }
          // If closed, continue draining until the channel is empty.
          if (circular_less_than(
                write_closed_at.load(std::memory_order_relaxed), 1 + Idx
              )) {
            Haz->active_offset.store(
              Idx + InactiveHazptrOffset, std::memory_order_release
            );
            return std::variant<T, std::monostate, std::monostate>(
              std::in_place_index<chan_err::CLOSED>
            );
          }
        }
        return std::variant<T, std::monostate, std::monostate>(
          std::in_place_index<chan_err::EMPTY>
        );
      }
      // Queue appears non-empty. See if data is ready for consumption at our
      // speculative Idx.
      data_block* block = Haz->read_block.load(std::memory_order_seq_cst);

      [[maybe_unused]] size_t boff =
        block->offset.load(std::memory_order_relaxed);
      assert(circular_less_than(actOff, 1 + Idx));
      assert(circular_less_than(boff, 1 + Idx));

      block = find_block(block, Idx);

      element* elem = &block->values[Idx & BlockSizeMask];
      if (elem->is_data_waiting()) {
        if (read_offset.compare_exchange_strong(
              Idx, Idx + 1, std::memory_order_seq_cst, std::memory_order_seq_cst
            )) {
          // Update last known block.
          // Note that if hazptr was to an older block, that block will still be
          // protected (by active_offset). This prevents a channel consisting of
          // a single block from trying to unlink/link that block to itself.
          Haz->read_block.store(block, std::memory_order_release);
          Haz->next_protect_read.store(boff, std::memory_order_relaxed);
          // Try to reclaim old blocks. Checking for index 1 ensures that at
          // least this token's hazptr will already be advanced to the new
          // block. Only consumers participate in reclamation and only 1
          // consumer at a time.
          if ((Idx & BlockSizeMask) == 1 && blocks_lock.try_lock()) {
            try_reclaim_blocks(Haz, woff);
            blocks_lock.unlock();
          }

          auto output = std::variant<T, std::monostate, std::monostate>(
            std::in_place_index<chan_err::OK>, std::move(elem->data.value)
          );
          elem->data.destroy();
          // Still need to store so block can be freed
          elem->set_done();
          Haz->active_offset.store(
            Idx + InactiveHazptrOffset, std::memory_order_release
          );
          return output;
        }
      } else {
        auto oldIdx = Idx;
        Idx = read_offset.load(std::memory_order_seq_cst);
        if (Idx == oldIdx) {
          return std::variant<T, std::monostate, std::monostate>(
            std::in_place_index<chan_err::EMPTY>
          );
        }
      }
    }
  }

  class aw_push : private tmc::detail::AwaitTagNoGroupCoAwait {
    channel& chan;
    hazard_ptr* haz_ptr;
    T t;

    friend chan_tok<T, Config>;

    template <typename U>
    aw_push(channel& Chan, hazard_ptr* Haz, U Val) noexcept
        : chan{Chan}, haz_ptr{Haz}, t{std::forward<U>(Val)} {}

    struct aw_push_impl {
      aw_push& parent;
      bool result;

      aw_push_impl(aw_push& Parent) noexcept : parent{Parent} {}

      bool await_ready() noexcept {
        result = parent.chan.post(parent.haz_ptr, std::move(parent.t));
        if (parent.haz_ptr->should_suspend()) [[unlikely]] {
          if (parent.haz_ptr->write_count + parent.haz_ptr->read_count ==
              ClusterPeriod) {
            size_t elapsed = parent.haz_ptr->elapsed();
            size_t writerCount = 0;
            parent.haz_ptr->for_each_owned_hazptr(
              [&]() { return true; },
              [&](hazard_ptr* curr) {
                auto writes = curr->write_count.load(std::memory_order_relaxed);
                if (writes != 0) {
                  ++writerCount;
                }
              }
            );

            if (elapsed >= parent.haz_ptr->minCycles * writerCount) {
              // Just suspend without clustering (to allow other producers to
              // run)
              parent.haz_ptr->write_count.store(0, std::memory_order_relaxed);
              parent.haz_ptr->read_count.store(0, std::memory_order_relaxed);
              return false;
            }
          }

          // Try to get rti. Suspend if we can get it.
          // If we don't get it on this call to push(), don't suspend and try
          // again to get it on the next call.
          int rti = parent.haz_ptr->requested_thread_index.load(
            std::memory_order_relaxed
          );
          if (rti == -1) {
            if (parent.chan.try_cluster(parent.haz_ptr)) {
              rti = parent.haz_ptr->requested_thread_index.load(
                std::memory_order_relaxed
              );
            }
          }
          if (rti != -1) {
            parent.haz_ptr->write_count.store(0, std::memory_order_relaxed);
            parent.haz_ptr->read_count.store(0, std::memory_order_relaxed);
            return false;
          }
        }

        return true;
      }

      void await_suspend(std::coroutine_handle<> Outer) noexcept {
        size_t target =
          static_cast<size_t>(parent.haz_ptr->requested_thread_index);
        parent.haz_ptr->requested_thread_index.store(
          -1, std::memory_order_relaxed
        );
        tmc::detail::post_checked(
          tmc::detail::this_thread::executor, std::move(Outer),
          tmc::detail::this_thread::this_task.prio, target
        );
      }

      bool await_resume() noexcept { return result; }
    };

  public:
    aw_push_impl operator co_await() && noexcept { return aw_push_impl(*this); }
  };

  void close() noexcept {
    std::scoped_lock<std::mutex> lg(blocks_lock);
    if (0 != closed.load(std::memory_order_relaxed)) {
      return;
    }
    size_t woff = write_offset.load(std::memory_order_seq_cst);
    // Setting this to a distant-but-greater value before setting closed
    // prevents consumers from exiting too early.
    write_closed_at.store(
      woff + InactiveHazptrOffset, std::memory_order_seq_cst
    );

    closed.store(WRITE_CLOSING_BIT, std::memory_order_seq_cst);

    // Now mark the real closed_at index. Past this index, producers are
    // guaranteed to not produce. Prior to this index, producers may or may not
    // produce, depending on when they see the closed flag being set.
    woff = write_offset.fetch_add(1, std::memory_order_seq_cst);
    write_closed_at.store(woff, std::memory_order_seq_cst);
    closed.store(
      WRITE_CLOSING_BIT | WRITE_CLOSED_BIT, std::memory_order_seq_cst
    );
  }

  struct aw_drain_pause : private tmc::detail::AwaitTagNoGroupAsIs {
    bool await_ready() noexcept { return false; }
    void await_suspend(std::coroutine_handle<> Outer) noexcept {
      tmc::detail::post_checked(
        tmc::current_executor(), std::move(Outer), tmc::current_priority(),
        tmc::current_thread_index()
      );
    }
    void await_resume() noexcept {}
  };

  // TODO - currently the implementation of drain() is the same as drain_wait(),
  // but with deadlock detection. Ideally this would instead be implemented
  // using a shared state with the consumers; once all consumers finish they
  // would resume the drainer.
  tmc::task<void> drain() noexcept {
    close(); // close() is idempotent and a precondition to call this.
    blocks_lock.lock();
    size_t woff = write_closed_at.load(std::memory_order_seq_cst);
    size_t roff = read_offset.load(std::memory_order_seq_cst);

    // Fast-path reclaim blocks up to the earlier of read or write index
    {
      size_t protectIdx = woff - 1;
      keep_min(protectIdx, roff - 1);
      protectIdx = protectIdx & ~BlockSizeMask; // round down to block index

      // Ensure that the protected blocks are visible before running
      // try_reclaim_blocks(). Normally it runs after get_read_ticket() so the
      // block is guaranteed to be visible in that case, but here it may not be.
      data_block* block = head_block.load(std::memory_order_acquire);
      while (circular_less_than(
        block->offset.load(std::memory_order_relaxed), protectIdx
      )) {
        data_block* next = block->next.load(std::memory_order_acquire);
        while (next == nullptr) {
          // A block is being constructed; wait for it
          TMC_CPU_PAUSE();
          next = block->next.load(std::memory_order_acquire);
        }
        block = next;
      }

      hazard_ptr* haz = hazard_ptr_list.load(std::memory_order_relaxed);
      try_reclaim_blocks(haz, protectIdx);
    }

    data_block* block = head_block.load(std::memory_order_acquire);
    size_t i = block->offset.load(std::memory_order_relaxed);

    // Slow-path wait for the channel to drain.
    // Check each element prior to write_closed_at write index.
    // Producers and consumers will be present at these indexes.
    size_t consumerWaitSpins = 0;
    while (true) {
      while (circular_less_than(i, roff) && circular_less_than(i, woff)) {
        size_t idx = i & BlockSizeMask;
        auto v = &block->values[idx];
        while (!v->is_done()) {
          TMC_CPU_PAUSE();
        }

        ++i;
        block = find_block(block, i);
      }

      if (circular_less_than(roff, woff)) {
        // Wait for readers to catch up.
        TMC_CPU_PAUSE();
        size_t newRoff = read_offset.load(std::memory_order_seq_cst);
        if (roff == newRoff) {
          ++consumerWaitSpins;
          if (consumerWaitSpins == 10) {
            // If we spun 10 times without seeing roff change, we may be
            // deadlocked with a consumer waiting to run in this thread's
            // private work queue, or on a single-threaded executor.
            // Suspend this task to allow consumers to run.
            consumerWaitSpins = 0;
            // Don't hold mutex across the suspend point
            blocks_lock.unlock();

            // This depends on being able to go to the back of the line
            // (needs FIFO processing even if using a single thread)
            // Thus you need to reimplement inbox (for now), or allow pushing to
            // back of line (in future single thread queue implementation)
            co_await aw_drain_pause{};

            blocks_lock.lock();
            // Consumer may reclaim blocks while we are suspended
            block = head_block.load(std::memory_order_seq_cst);
            i = block->offset.load(std::memory_order_relaxed);
            newRoff = read_offset.load(std::memory_order_relaxed);
          }
        }
        roff = newRoff;
      } else {
        break;
      }
    }

    // i >= woff now and all data has been drained.
    // Now handle waking up waiting consumers.

    // `closed` is accessed by relaxed load in consumer.
    // In order to ensure that it is seen in a timely fashion, this
    // creates a release sequence with the acquire load in consumer.

    if (closed.load() != ALL_CLOSED_BITS) {
      read_closed_at.store(read_offset.fetch_add(1, std::memory_order_seq_cst));
      closed.store(ALL_CLOSED_BITS, std::memory_order_seq_cst);
    }
    roff = read_closed_at.load(std::memory_order_seq_cst);

    // We  are past the write_closed_at write index; no producers will use these
    // indexes. `roff` is now read_closed_at. Consumers may be waiting at
    // indexes prior to `roff`, or they may see that the queue is closed and
    // mark their elements as done.
    while (circular_less_than(i, roff)) {
      size_t idx = i & BlockSizeMask;
      auto v = &block->values[idx];

      auto cons = v->spin_wait_for_waiting_consumer();
      if (cons != nullptr) {
        cons->ok = false;
        tmc::detail::post_checked(
          cons->continuation_executor, std::move(cons->continuation), cons->prio
        );
      }

      ++i;
      block = find_block(block, i);
    }
    blocks_lock.unlock();
  }

  void drain_wait() noexcept {
    close(); // close() is idempotent and a precondition to call this.
    blocks_lock.lock();
    size_t woff = write_closed_at.load(std::memory_order_seq_cst);
    size_t roff = read_offset.load(std::memory_order_seq_cst);

    // Fast-path reclaim blocks up to the earlier of read or write index
    {
      size_t protectIdx = woff - 1;
      keep_min(protectIdx, roff - 1);
      protectIdx = protectIdx & ~BlockSizeMask; // round down to block index

      // Ensure that the protected blocks are visible before running
      // try_reclaim_blocks(). Normally it runs after get_read_ticket() so the
      // block is guaranteed to be visible in that case, but here it may not be.
      data_block* block = head_block.load(std::memory_order_acquire);
      while (circular_less_than(
        block->offset.load(std::memory_order_relaxed), protectIdx
      )) {
        data_block* next = block->next.load(std::memory_order_acquire);
        while (next == nullptr) {
          // A block is being constructed; wait for it
          TMC_CPU_PAUSE();
          next = block->next.load(std::memory_order_acquire);
        }
        block = next;
      }

      hazard_ptr* haz = hazard_ptr_list.load(std::memory_order_relaxed);
      try_reclaim_blocks(haz, protectIdx);
    }

    data_block* block = head_block.load(std::memory_order_acquire);
    size_t i = block->offset.load(std::memory_order_relaxed);

    // Slow-path wait for the channel to drain.
    // Check each element prior to write_closed_at write index.
    // Producers and consumers will be present at these indexes.
    while (true) {
      while (circular_less_than(i, roff) && circular_less_than(i, woff)) {
        size_t idx = i & BlockSizeMask;
        auto v = &block->values[idx];
        while (!v->is_done()) {
          TMC_CPU_PAUSE();
        }

        ++i;
        block = find_block(block, i);
      }
      if (circular_less_than(roff, woff)) {
        // Wait for readers to catch up.
        TMC_CPU_PAUSE();
        size_t newRoff = read_offset.load(std::memory_order_seq_cst);
        roff = newRoff;
      } else {
        break;
      }
    }

    // i >= woff now and all data has been drained.
    // Now handle waking up waiting consumers.

    // `closed` is accessed by relaxed load in consumer.
    // In order to ensure that it is seen in a timely fashion, this
    // creates a release sequence with the acquire load in consumer.

    if (closed.load() != ALL_CLOSED_BITS) {
      read_closed_at.store(read_offset.fetch_add(1, std::memory_order_seq_cst));
      closed.store(ALL_CLOSED_BITS, std::memory_order_seq_cst);
    }
    roff = read_closed_at.load(std::memory_order_seq_cst);

    // We  are past the write_closed_at write index; no producers will use these
    // indexes. `roff` is now read_closed_at. Consumers may be waiting at
    // indexes prior to `roff`, or they may see that the queue is closed and
    // mark their elements as done.
    while (circular_less_than(i, roff)) {
      size_t idx = i & BlockSizeMask;
      auto v = &block->values[idx];

      auto cons = v->spin_wait_for_waiting_consumer();
      if (cons != nullptr) {
        cons->ok = false;
        tmc::detail::post_checked(
          cons->continuation_executor, std::move(cons->continuation), cons->prio
        );
      }

      ++i;
      block = find_block(block, i);
    }
    blocks_lock.unlock();
  }

  ~channel() {
    {
      // Since tokens share ownership of channel, at this point there can be no
      // active tokens. However it is possible that data was pushed to the
      // channel without being pulled. Run destructors for this data.
      close(); // ensure write_closed_at exists
      size_t woff = write_closed_at.load(std::memory_order_relaxed);
      size_t idx = read_offset.load(std::memory_order_relaxed);
      data_block* block = head_block.load(std::memory_order_acquire);
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
      data_block* block = head_block.load(std::memory_order_acquire);
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
    {
      hazard_ptr* start = hazard_ptr_list.load(std::memory_order_relaxed);
      hazard_ptr* curr = start;
      while (true) {
        assert(!curr->owned);
        hazard_ptr* next = curr->next.load(std::memory_order_relaxed);
        delete curr;
        if (next == start) {
          break;
        }
        curr = next;
      }
    }
  }

  channel(const channel&) = delete;
  channel& operator=(const channel&) = delete;
  channel(channel&&) = delete;
  channel& operator=(channel&&) = delete;
};

template <typename T, typename Config> class chan_tok {
  using chan_t = channel<T, Config>;
  using hazard_ptr = chan_t::hazard_ptr;
  std::shared_ptr<chan_t> chan;
  hazard_ptr* haz_ptr;
  NO_CONCURRENT_ACCESS_LOCK

  friend chan_tok make_channel<T, Config>() noexcept;

  chan_tok(std::shared_ptr<chan_t>&& Chan) noexcept
      : chan{std::move(Chan)}, haz_ptr{nullptr} {}

  hazard_ptr* get_hazard_ptr() noexcept {
    if (haz_ptr == nullptr) [[unlikely]] {
      haz_ptr = chan->get_hazard_ptr();
    }
    return haz_ptr;
  }

  void free_hazard_ptr() noexcept {
    if (haz_ptr != nullptr) [[likely]] {
      haz_ptr->release_ownership();
      haz_ptr = nullptr;
    }
  }

public:
  /// If the channel is open, this will always return true, indicating that Val
  /// was enqueued.
  ///
  /// If the channel is closed, this will return false, and Val will not be
  /// enqueued.
  ///
  /// Will not suspend or block.
  template <typename U> bool post(U&& Val) noexcept {
    ASSERT_NO_CONCURRENT_ACCESS();
    hazard_ptr* haz = get_hazard_ptr();
    return chan->post(haz, std::forward<U>(Val));
  }

  /// Returns a bool.
  /// If the channel is open, this will always return true, indicating that Val
  /// was enqueued.
  ///
  /// If the channel is closed, this will return false, and Val will not be
  /// enqueued.
  ///
  /// May suspend to do producer clustering under high load.
  template <typename U>
  [[nodiscard("You must co_await push().")]] chan_t::aw_push
  push(U&& Val) noexcept {
    ASSERT_NO_CONCURRENT_ACCESS();
    hazard_ptr* haz = get_hazard_ptr();
    return typename chan_t::aw_push(*chan, haz, std::forward<U>(Val));
  }

  /// Returns a std::optional<T>.
  /// If the channel is open, this will always return a value.
  ///
  /// If the channel is closed, this will continue to return values until the
  /// queue has been fully drained. After that, it will return an empty
  /// optional.
  ///
  /// May suspend until a value is available, or the queue is closed.
  [[nodiscard("You must co_await pull().")]] chan_t::aw_pull pull() noexcept {
    ASSERT_NO_CONCURRENT_ACCESS();
    hazard_ptr* haz = get_hazard_ptr();
    return typename chan_t::aw_pull(*chan, haz);
  }

  /// The index of the returned variant corresponds to a value of tmc::chan_err.
  /// If `result.index() == tmc::chan_err::OK`, a value has been retrieved
  /// from the channel and can be accessed with `std::get<0>(result)`.
  /// If `result.index() == tmc::chan_err::EMPTY`, the channel was empty.
  /// If `result.index() == tmc::chan_err::CLOSED`, the channel is closed.
  /// Warning: Avoid calling `try_pull()` in a tight loop from a coroutine or
  /// function that may run on an executor. It may deadlock with producers
  /// waiting to run on that executor. Prefer to `co_await pull()` instead.
  /// Spinning on `try_pull()` is safe from an external thread.
  std::variant<T, std::monostate, std::monostate> try_pull() noexcept {
    ASSERT_NO_CONCURRENT_ACCESS();
    hazard_ptr* haz = get_hazard_ptr();
    return chan->try_pull(haz);
  }

  /// If the channel is open, this will always return true, indicating that
  /// Count elements, starting from the Begin iterator, were enqueued.
  ///
  /// If the channel is closed, this will return false, and no items
  /// will be enqueued.
  ///
  /// Each item is moved (not copied) from the iterator into the channel.
  ///
  /// The closed check is performed first, then space is pre-allocated, then all
  /// Count items are moved into the channel. Thus, there cannot be a partial
  /// success - either all or none of the items will be moved.
  ///
  /// Will not suspend or block.
  template <typename TIter> bool post_bulk(TIter&& Begin, size_t Count) {
    hazard_ptr* haz = get_hazard_ptr();
    return chan->post_bulk(haz, static_cast<TIter&&>(Begin), Count);
  }

  /// Calculates the number of elements via `size_t Count = End - Begin;`
  ///
  /// If the channel is open, this will always return true, indicating that
  /// Count elements, starting from the Begin iterator, were enqueued.
  ///
  /// If the channel is closed, this will return false, and no items
  /// will be enqueued.
  ///
  /// Each item is moved (not copied) from the iterator into the channel.
  ///
  /// The closed check is performed first, then space is pre-allocated, then all
  /// Count items are moved into the channel. Thus, there cannot be a partial
  /// success - either all or none of the items will be moved.
  ///
  /// Will not suspend or block.
  template <typename TIter> bool post_bulk(TIter&& Begin, TIter&& End) {
    hazard_ptr* haz = get_hazard_ptr();
    return chan->post_bulk(
      haz, static_cast<TIter&&>(Begin), static_cast<size_t>(End - Begin)
    );
  }

  /// Calculates the number of elements via
  /// `size_t Count = Range.end() - Range.begin();`
  ///
  /// If the channel is open, this will always return true, indicating that
  /// Count elements from the beginning of the range were enqueued.
  ///
  /// If the channel is closed, this will return false, and no items
  /// will be enqueued.
  ///
  /// Each item is moved (not copied) from the iterator into the channel.
  ///
  /// The closed check is performed first, then space is pre-allocated, then all
  /// Count items are moved into the channel. Thus, there cannot be a partial
  /// success - either all or none of the items will be moved.
  ///
  /// Will not suspend or block.
  template <typename TRange> bool post_bulk(TRange&& Range) {
    hazard_ptr* haz = get_hazard_ptr();
    auto begin = static_cast<TRange&&>(Range).begin();
    auto end = static_cast<TRange&&>(Range).end();
    return chan->post_bulk(haz, begin, static_cast<size_t>(end - begin));
  }

  /// All future calls to `post()` and `push()` will immediately return false.
  /// Calls to `pull()` will continue to read data until all messages have been
  /// consumed, at which point all subsequent calls to `pull()` will immediately
  /// return an empty optional.
  ///
  /// This function is idempotent and thread-safe. It is not lock-free. It may
  /// contend the lock against `close()` and `drain()`.
  void close() noexcept { chan->close(); }

  /// If the channel is not already closed, it will be closed.
  /// Then, waits for consumers to drain all remaining data from the channel.
  /// After all data has been consumed from the channel, any waiting consumers
  /// will be awakened, and all current and future consumers will immediately
  /// return an empty optional.
  ///
  /// This function is idempotent and thread-safe. It is not lock-free. It may
  /// contend the lock against `close()` and `drain()`.
  tmc::task<void> drain() noexcept { return chan->drain(); }

  /// If the channel is not already closed, it will be closed.
  /// Then, waits for consumers to drain all remaining data from the channel.
  /// After all data has been consumed from the channel, any waiting consumers
  /// will be awakened, and all current and future consumers will immediately
  /// return an empty optional.
  ///
  /// Warning: Avoid calling `drain_wait()` from a coroutine or function that
  /// may run on an executor. It may deadlock with consumers waiting to run on a
  /// single-threaded executor. Prefer to `co_await drain()` instead.
  /// `drain_wait()` is safe to call from an external thread.
  ///
  /// This function is idempotent and thread-safe. It is not lock-free. It may
  /// contend the lock against `close()` and `drain()`.
  void drain_wait() noexcept { chan->drain_wait(); }

  /// If true, spent blocks will be cleared and moved to the tail of the queue.
  /// If false, spent blocks will be deleted.
  /// Default: true
  ///
  /// If Config::EmbedFirstBlock == true, this will be forced to true.
  chan_tok& set_reuse_blocks(bool Reuse) noexcept {
    if constexpr (!Config::EmbedFirstBlock) {
      chan->ReuseBlocks.store(Reuse, std::memory_order_relaxed);
      return *this;
    }
  }

  /// If a consumer sees no data is ready at a ticket, it will spin wait this
  /// many times. Each spin wait is an asm("pause") and reload.
  /// Default: 0
  chan_tok& set_consumer_spins(size_t SpinCount) noexcept {
    chan->ConsumerSpins.store(SpinCount, std::memory_order_relaxed);
    return *this;
  }

  /// If the total number of elements pushed per second to the queue is greater
  /// than this threshold, then the queue will attempt to move producers and
  /// consumers near each other to optimize sharing efficiency. The default
  /// value of 2,000,000 represents an item being pushed every 500ns. This
  /// behavior can be disabled entirely by setting this to 0.
  chan_tok& set_heavy_load_threshold(size_t Threshold) noexcept {
    size_t cycles =
      Threshold == 0 ? 0 : TMC_CPU_FREQ * chan_t::ClusterPeriod / Threshold;
    chan->MinClusterCycles.store(cycles, std::memory_order_relaxed);
    return *this;
  }

  /// Copy Constructor: The new chan_tok will have its own hazard pointer so
  /// that it can be used concurrently with the other token.
  ///
  /// If the other token is from a different channel, this token will now point
  /// to that channel.
  chan_tok(const chan_tok& Other) noexcept
      : chan(Other.chan), haz_ptr{nullptr} {}

  /// Copy Assignment: If the other token is from a different channel, this
  /// token will now point to that channel.
  chan_tok& operator=(const chan_tok& Other) noexcept {
    if (chan != Other.chan) {
      free_hazard_ptr();
      chan = Other.chan;
    }
  }

  /// Identical to the token copy constructor, but makes
  /// the intent more explicit - that a new token is being created which will
  /// independently own a reference count and hazard pointer to the underlying
  /// channel.
  chan_tok new_token() noexcept { return chan_tok(*this); }

  /// Move Constructor: The moved-from token will become empty; it will release
  /// its channel pointer, and its hazard pointer.
  chan_tok(chan_tok&& Other) noexcept
      : chan(std::move(Other.chan)), haz_ptr{Other.haz_ptr} {
    Other.haz_ptr = nullptr;
  }

  /// Move Assignment: The moved-from token will become empty; it will release
  /// its channel pointer, and its hazard pointer.
  ///
  /// If the other token is from a different channel, this token will now point
  /// to that channel.
  chan_tok& operator=(chan_tok&& Other) noexcept {
    if (chan != Other.chan) {
      free_hazard_ptr();
      haz_ptr = Other.haz_ptr;
      Other.haz_ptr = nullptr;
    } else {
      if (haz_ptr != nullptr) {
        // It's more efficient to keep our own hazptr
        Other.free_hazard_ptr();
      } else {
        haz_ptr = Other.haz_ptr;
        Other.haz_ptr = nullptr;
      }
    }
    chan = std::move(Other.chan);
    return *this;
  }

  /// Releases the token's hazard pointer and decrements the channel's shared
  /// reference count. When the last token for a channel is destroyed, the
  /// channel will also be destroyed. If the channel was not drained and any
  /// data remains in the channel, the destructor will also be called for each
  /// remaining data element.
  ~chan_tok() { free_hazard_ptr(); }
};

template <typename T, typename Config>
inline chan_tok<T, Config> make_channel() noexcept {
  auto chan = new channel<T, Config>();
  return chan_tok<T, Config>{std::shared_ptr<channel<T, Config>>(chan)};
}

} // namespace tmc
