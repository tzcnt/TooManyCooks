// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#ifdef TMC_USE_HWLOC
#include "tmc/detail/hwloc_unique_bitmap.hpp"
#include <hwloc.h>

#ifdef TMC_DEBUG_THREAD_CREATION
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

hwloc_unique_bitmap::operator hwloc_bitmap_t() { return obj; }

#ifdef TMC_DEBUG_THREAD_CREATION
void hwloc_unique_bitmap::print() {
  char* bitmapStr;
  hwloc_bitmap_asprintf(&bitmapStr, obj);
  std::printf("%s\n", bitmapStr);
  std::free(bitmapStr);
}
#endif

} // namespace detail
} // namespace tmc
#endif
