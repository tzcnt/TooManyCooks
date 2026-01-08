// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#ifdef TMC_USE_HWLOC
#include "tmc/detail/hwloc_forward_defs.hpp"
#endif
namespace tmc {
namespace detail {

// A unique_ptr-like wrapper over a hwloc_bitmap_t.
// If hwloc is not enabled, this is an empty struct.
struct hwloc_unique_bitmap {
#ifdef TMC_USE_HWLOC
  hwloc_bitmap_s* obj;

  hwloc_unique_bitmap();
  hwloc_unique_bitmap(hwloc_bitmap_s*);

  // Releases the bitmap on destruction. If null, nothing happens.
  ~hwloc_unique_bitmap();

  // Explicitly releases the bitmap early.
  void free();

  // No copy constructor
  hwloc_unique_bitmap(const hwloc_unique_bitmap& Other) = delete;
  hwloc_unique_bitmap& operator=(const hwloc_unique_bitmap& Other) = delete;

  // Explicit copy is allowed
  hwloc_unique_bitmap clone();

  // Can be moved, transferring ownership of the bitmap
  hwloc_unique_bitmap(hwloc_unique_bitmap&& Other);
  hwloc_unique_bitmap& operator=(hwloc_unique_bitmap&& Other);

  operator hwloc_bitmap_s*();

#ifdef TMC_DEBUG_THREAD_CREATION
  void print();
#endif

#endif
};
} // namespace detail
} // namespace tmc

#ifdef TMC_IMPL
#include "tmc/detail/hwloc_unique_bitmap.ipp"
#endif
