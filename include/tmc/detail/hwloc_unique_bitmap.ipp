// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/impl.hpp" // IWYU pragma: keep

// All of the definitions in this file require hwloc.
#ifdef TMC_USE_HWLOC
#include "tmc/detail/hwloc_unique_bitmap.hpp"
#include <hwloc.h>

#ifdef TMC_DEBUG_THREAD_CREATION
#include <cassert>
#include <cstdio>
#include <cstdlib>
#endif

namespace tmc {
namespace detail {

hwloc_unique_bitmap::hwloc_unique_bitmap() : obj{nullptr} {}
hwloc_unique_bitmap::hwloc_unique_bitmap(hwloc_bitmap_t From) : obj{From} {}
hwloc_unique_bitmap::hwloc_unique_bitmap(hwloc_unique_bitmap&& Other) {
  obj = Other.obj;
  Other.obj = nullptr;
}

hwloc_unique_bitmap&
hwloc_unique_bitmap::operator=(hwloc_unique_bitmap&& Other) {
  hwloc_bitmap_free(obj);
  obj = Other.obj;
  Other.obj = nullptr;
  return *this;
}

hwloc_unique_bitmap hwloc_unique_bitmap::clone() {
  return hwloc_bitmap_dup(obj);
}

hwloc_unique_bitmap::~hwloc_unique_bitmap() { hwloc_bitmap_free(obj); }

void hwloc_unique_bitmap::free() {
  hwloc_bitmap_free(obj);
  obj = nullptr;
}

hwloc_unique_bitmap::operator hwloc_bitmap_s*() { return obj; }

#ifdef TMC_DEBUG_THREAD_CREATION
void hwloc_unique_bitmap::print() {
  // freeing the string returned by hwloc_bitmap_asprintf() was erroring on
  // Windows. maybe an allocator mismatch between hwloc DLL and the application.
  // this version works, since the application controls the allocation
  int len = hwloc_bitmap_snprintf(nullptr, 0, obj);
  assert(len >= 0);
  size_t bufsz = static_cast<size_t>(len) + 1;
  char* buf = new char[bufsz];
  hwloc_bitmap_snprintf(buf, bufsz, obj);
  std::printf("%s\n", buf);
  delete[] buf;
}
#endif

} // namespace detail
} // namespace tmc
#endif
