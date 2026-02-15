// Copyright (c) 2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

// Forward definitions of hwloc struct types, so that APIs that take pointers to
// these types can be in headers without requiring the hwloc.h file.
// Usage of hwloc.h can then be constrained to the implementation TU.

#ifdef TMC_USE_HWLOC
struct hwloc_bitmap_s;
struct hwloc_obj;
struct hwloc_topology;
#endif

#if !defined(TMC_STANDALONE_COMPILATION_HWLOC) || defined(TMC_IMPL)
#include "tmc/detail/hwloc_forward_defs.ipp"
#endif
