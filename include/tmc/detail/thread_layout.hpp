// Copyright (c) 2023-2025 Logan McDougall
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
struct L3CacheSet {
  // The type of `l3cache` is hwloc_obj_t. Stored as void* so this type can be
  // used when hwloc is not enabled. This minimizes code duplication elsewhere.
  void* l3cache;
  size_t group_size;
  std::vector<size_t> puIndexes;
};
#ifdef TMC_USE_HWLOC
// NUMALatency exposed by hwloc (stored in System Locality Distance
// Information Table) is not helpful if the system is not confirmed as NUMA
// Use l3 cache groupings instead
// TODO handle non-uniform core layouts (Intel/ARM hybrid architecture)
// https://utcc.utoronto.ca/~cks/space/blog/linux/IntelHyperthreadingSurprise
std::vector<L3CacheSet> group_cores_by_l3c(hwloc_topology_t& Topology);

// Modifies GroupedCores according to the number of found cores and requested
// values. Also modifies Lasso to determine whether thread lassoing should be
// enabled.
// Returns the PU-to-thread-index mapping used by notify_n.
std::vector<size_t> adjust_thread_groups(
  size_t RequestedThreadCount, float RequestedOccupancy,
  std::vector<L3CacheSet>& GroupedCores, bool& Lasso
);

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

// These functions relate to the work-stealing matrixes used by ex_cpu.
//   get_*_matrix are algorithms to produce a forward matrix, which is used for
// work stealing. It lets you answer the question "Given thread 3, in what order
// should I look to steal from other threads?"
//   invert_matrix produces an inverted matrix from the forward matrix.
// It is used for thread waking. It lets you answer the question, "Given work is
// ready in thread 3's queue, in what order should I look to wake threads to
// steal that work?" Ideally the thread that is woken is one that will check
// thread 3's queue early in its steal order, and by corollary will be in a
// nearby L3 cache.

// Given the following forward (steal) matrix:
// 0,1,2,3,4,5,6,7
// 1,2,3,0,5,6,7,4
// 2,3,0,1,6,7,4,5
// 3,0,1,2,7,4,5,6
// 4,5,6,7,0,1,2,3
// 5,6,7,4,1,2,3,0
// 6,7,4,5,2,3,0,1
// 7,4,5,6,3,0,1,2

// The resulting inverted (waker) matrix is:
// 0,3,2,1,4,7,6,5
// 1,0,3,2,5,4,7,6
// 2,1,0,3,6,5,4,7
// 3,2,1,0,7,6,5,4
// 4,7,6,5,0,3,2,1
// 5,4,7,6,1,0,3,2
// 6,5,4,7,2,1,0,3
// 7,6,5,4,3,2,1,0

std::vector<size_t>
get_lattice_matrix(std::vector<L3CacheSet> const& groupedCores);

std::vector<size_t>
get_hierarchical_matrix(std::vector<L3CacheSet> const& groupedCores);

std::vector<size_t>
invert_matrix(std::vector<size_t> const& InputMatrix, size_t N);

std::vector<size_t>
slice_matrix(std::vector<size_t> const& InputMatrix, size_t N, size_t Slot);

} // namespace detail
} // namespace tmc

#ifdef TMC_IMPL
#include "tmc/detail/thread_layout.ipp"
#endif
