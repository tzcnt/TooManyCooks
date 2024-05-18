// Copyright (c) 2023-2024 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#ifdef TMC_USE_HWLOC
#include <hwloc.h>
#endif
#include <vector>
namespace tmc {
namespace detail {
#ifdef TMC_USE_HWLOC
struct L3CacheSet {
  hwloc_obj_t l3cache;
  size_t group_size;
};
// NUMALatency exposed by hwloc (stored in System Locality Distance
// Information Table) is not helpful if the system is not confirmed as NUMA
// Use l3 cache groupings instead
// TODO handle non-uniform core layouts (Intel/ARM hybrid architecture)
// https://utcc.utoronto.ca/~cks/space/blog/linux/IntelHyperthreadingSurprise
std::vector<L3CacheSet> group_cores_by_l3c(hwloc_topology_t& Topology);

// bind this thread to any of the cores that share l3 cache in this set
void bind_thread(hwloc_topology_t Topology, hwloc_cpuset_t SharedCores);
#endif
struct ThreadGroupData {
  size_t start;
  size_t size;
};
struct ThreadSetupData {
  std::vector<ThreadGroupData> groups;
  size_t total_size;
};
std::vector<size_t>
get_group_iteration_order(size_t GroupCount, size_t StartGroup);
} // namespace detail
} // namespace tmc

#ifdef TMC_IMPL
#include "tmc/detail/thread_layout.ipp"
#endif
