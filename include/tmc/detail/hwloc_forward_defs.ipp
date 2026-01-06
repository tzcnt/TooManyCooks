// Copyright (c) 2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

// Forward definitions of hwloc struct types, so that APIs that take pointers to
// these types can be in public headers without depending on the <hwloc.h> file.

#ifdef TMC_USE_HWLOC
#include "tmc/detail/hwloc_forward_defs.hpp"

#include <hwloc.h>
#include <type_traits>

static_assert(std::is_same_v<hwloc_cpuset_t, hwloc_bitmap_s*>);
static_assert(std::is_same_v<hwloc_bitmap_t, hwloc_bitmap_s*>);
static_assert(std::is_same_v<hwloc_obj_t, hwloc_obj*>);
#endif
