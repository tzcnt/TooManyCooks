// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/compat.hpp"
#include "tmc/work_item.hpp"

#include <cassert>
#include <cstddef>
#include <cstring>
#include <new>

#if TMC_PLATFORM_BITS == 64
#include "tmc/detail/qu_chase_lev64.hpp"
#else
#include "tmc/detail/qu_chase_lev32.hpp"
#endif

// A collection of ThreadCount individual deques, each of which is "owned" by a
// single ex_cpu thread. This collection owns the lifecycle of the `deques`
// array, but the methods don't use it. Instead, threads have a TLS-cached steal
// order that they use to look for victims, which is passed in to the methods.

namespace tmc {
namespace detail {
struct qu_work_stealing {
  using cldq_t = chase_lev_deque<tmc::work_item>;
  cldq_t* deques;
  size_t threadCount;
  size_t tlsArrayOffset;

  qu_work_stealing(size_t ThreadCount, size_t CumulativeOffset)
      : threadCount{ThreadCount}, tlsArrayOffset{CumulativeOffset} {
    if (ThreadCount > 0) {
      deques = new chase_lev_deque<tmc::work_item>[ThreadCount];
    }
  }

  ~qu_work_stealing() {
    if (threadCount > 0) {
      delete[] deques;
    }
  }

  // Try to steal an item from one of the other worker threads' Chase-Lev
  // deques using the precalculated (TLS-cached) static iteration order. Returns
  // true if an item was stolen.
  //
  // Precondition: producers points to the second (cache) slot for this thread's
  // deque list on this priority.
  // Postcondition: producers is advanced to the beginning of the next
  // priority's slot block, ready to try_pop for this thread.
  TMC_FORCE_INLINE inline bool
  try_steal_in_order(tmc::work_item& item, cldq_t**& producers) {
    cldq_t** end = producers + threadCount;

    // producers[0] is the previously consumed-from (cached) deque for this
    // priority. If we didn't find work last time, it will be null.
    cldq_t** cacheProd = producers;
    if (*producers == nullptr) {
      ++producers;
    }

    // Check the remaining threads in the predefined order
    for (; producers != end; ++producers) {
      // Use a single pause to limit cross-core cache coherency traffic under
      // load.
      TMC_CPU_PAUSE();
      cldq_t* prod = *producers;
      if (prod->steal(item)) {
        // update prev_prod
        *cacheProd = prod;
        return true;
      }
    }

    *cacheProd = nullptr;
    return false;
  }

  // Per-priority TLS array layout: [self, cache, others...].
  // Skip both self and cache and start iterating from index 2.
  // Check if all of `others` are empty.
  TMC_FORCE_INLINE inline bool
  steal_empty(tmc::detail::chase_lev_deque<tmc::work_item>**& producersBase) {
    cldq_t** producers = producersBase + tlsArrayOffset + 1;
    size_t deqCount = threadCount;
    for (size_t d = 1; d < deqCount; ++d) {
      TMC_CPU_PAUSE();
      if (!producers[d]->empty()) {
        // update cache so runner finds this first
        producers[0] = producers[d];
        return false;
      }
    }
    return true;
  }
};
} // namespace detail
} // namespace tmc
