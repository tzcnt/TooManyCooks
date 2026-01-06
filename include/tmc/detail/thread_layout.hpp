// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#ifdef TMC_USE_HWLOC
#include "tmc/detail/hwloc_forward_defs.hpp"
#include "tmc/detail/hwloc_unique_bitmap.hpp"
#include <mutex>
#endif

#include <functional>
#include <vector>

namespace tmc {
namespace topology {
namespace detail {

struct TopologyCore {
#ifdef TMC_USE_HWLOC
  std::vector<hwloc_obj*> pus;
  // TMC's index that ensures consistent ordering across platforms. Not the same
  // as core's logical_index or os_index.
  size_t index;
  hwloc_bitmap_s* cpuset = nullptr;
  // If hwloc is enabled, this will be a `hwloc_obj*` that points to the hwloc
  // object that is the nearest shared parent cache of this core. Otherwise,
  // this will be nullptr.
  hwloc_obj* cache = nullptr;
  // If hwloc is enabled, this will be a `hwloc_obj*` that points to the hwloc
  // object that is the NUMA node that owns this core. Otherwise,
  // this will be nullptr.
  hwloc_obj* numa = nullptr;
  size_t cpu_kind = 0;
#endif
};
struct CacheGroup {
  // If hwloc is enabled, this will be a `hwloc_obj*` that points to the hwloc
  // cache object for this group. Otherwise, this will be nullptr.
  void* obj;

  // TMC's index that ensures consistent ordering across platforms. Not the same
  // as core's logical_index or os_index. This index is among all groups
  // (including empty groups). If this is not a leaf node, index will be -1.
  int index;

  size_t cpu_kind;

  // If this cache also has sub-cache groups
  std::vector<CacheGroup> children;

  // Directly owned cores (not including those in child groups)
  std::vector<TopologyCore> cores;

  // Number of threads in this group. Will be 0 if this is not a leaf node.
  // Initially, 1 thread per core. May be modified afterward.
  size_t group_size;

  // Start index of the first thread in this group among all threads.
  // This includes threads that don't share the same partition.
  size_t group_start;
};

#ifdef TMC_USE_HWLOC
// Private topology type - contains more info than tmc::topology::CpuTopology
struct Topology {
  std::vector<TopologyCore> cores;
  // This is a hierarchy view, not a flat view
  std::vector<CacheGroup> groups;

  // Heterogeneous core information (P-cores vs E-cores)
  // Index 0 is P-cores
  // Index 1 (if it exists) is E-cores
  // Index 2 (if it exists) is LP E-cores
  std::vector<size_t> cpu_kind_counts;
  inline bool is_hybrid() { return cpu_kind_counts.size() > 1; }
};

// Returns a flattened view of the groups (leaf nodes only).
// Pointers are back into the provided Groups hierarchy.
std::vector<tmc::topology::detail::CacheGroup*>
flatten_groups(std::vector<tmc::topology::detail::CacheGroup>& Groups);

struct topo_data {
  std::mutex lock;
  hwloc_topology* hwloc;
  detail::Topology tmc;
  bool ready = false;
};
// Constructing a topology is pretty slow (10ms) and it's accessed
// infrequently. The mutex is needed for any user operations that access this,
// to populate it lazily. It should always be constructed at executor
// init() or sooner, so if the executor needs to query it afterward in a
// read-only fashion, a mutex is not needed.
inline topo_data g_topo;

// XmlBuffer option is for unit tests
detail::Topology query_internal(
  hwloc_topology*& HwlocTopo, const char* XmlBuffer = nullptr,
  size_t XmlBufferLen = 0
);

void query_internal_parse(
  hwloc_topology*& HwlocTopo, detail::Topology& Topo_out
);

hwloc_obj* find_parent_cache(hwloc_obj* Start);
void make_cache_parent_group(
  hwloc_obj* parent, std::vector<tmc::topology::detail::CacheGroup>& caches,
  std::vector<hwloc_obj*>& work, size_t shareStart, size_t shareEnd
);
#endif
} // namespace detail
} // namespace topology

namespace detail {
struct ThreadCacheGroupIterator {
  struct state {
    size_t orderIdx;
    std::vector<tmc::topology::detail::CacheGroup>& cores;
    std::vector<size_t> order;
  };
  std::vector<state> states_;
  std::function<void(tmc::topology::detail::CacheGroup&)> process_;
  ThreadCacheGroupIterator(
    std::vector<tmc::topology::detail::CacheGroup>&,
    std::function<void(tmc::topology::detail::CacheGroup&)>
  );
  bool next();
};

void for_all_groups(
  std::vector<tmc::topology::detail::CacheGroup>&,
  std::function<void(tmc::topology::detail::CacheGroup&)>
);
} // namespace detail
} // namespace tmc

#ifdef TMC_USE_HWLOC
// There is an include order dependency between the public header and this
// private header file. Fixing it would require splitting up the public header,
// which makes it harder to read for end-users. ...or we can just include it in
// the middle of this file...
#include "tmc/topology.hpp"
#endif

namespace tmc {
namespace detail {
#ifdef TMC_USE_HWLOC

// Modifies GroupedCores according to the number of found cores and requested
// values. Also modifies Lasso to determine whether thread lassoing should be
// enabled.
// Returns the PU-to-thread-index mapping used by notify_n.
void adjust_thread_groups(
  size_t RequestedThreadCount, std::vector<float> RequestedOccupancy,
  std::vector<tmc::topology::detail::CacheGroup*> flatGroups,
  topology::topology_filter const& Filter,
  topology::thread_packing_strategy Pack
);

// Bind this thread to any core in the CpuSet.
// On MacOS, will attempt to set thread QoS based on the Kind instead.
// If multiple  Kinds are specified, no QoS will be set.
void pin_thread(
  hwloc_topology* Topology, tmc::detail::hwloc_unique_bitmap& CpuSet,
  tmc::topology::cpu_kind::value Kind
);

// Used by single-threaded executors to simplify thread pinning
tmc::detail::hwloc_unique_bitmap make_partition_cpuset(
  void* HwlocTopo, tmc::topology::detail::Topology& TmcTopo,
  topology::topology_filter const& Filter,
  // Used by single-threaded executors to detect CpuKind if a thread has been
  // pinned to a single core kind. Otherwise, may return multiple kinds OR'ed
  // together. This is needed to support Apple QoS binding... otherwise we
  // could just use the cpuset return value.
  tmc::topology::cpu_kind::value& CpuKind_out
);

tmc::topology::core_group
public_group_from_private(tmc::topology::detail::CacheGroup& Input);

#endif
struct ThreadGroupData {
  size_t start;
  size_t size;
  // Maintain this index across groups, so when multiple smaller groups steal
  // from a larger group, the load is spread evenly (instead of being
  // concentrated on the lower indexes)
  size_t stolenFromIdx;
};
struct ThreadSetupData {
  std::vector<ThreadGroupData> groups;
  size_t total_size;
};
std::vector<size_t>
get_flat_group_iteration_order(size_t GroupCount, size_t StartGroup);

// These functions relate to the work-stealing matrixes used by ex_cpu.
//   get_*_matrix are algorithms to produce a forward matrix, which is used
//   for
// work stealing. It lets you answer the question "Given thread 3, in what
// order should I look to steal from other threads?"
//   invert_matrix produces an inverted matrix from the forward matrix.
// It is used for thread waking. It lets you answer the question, "Given work
// is ready in thread 3's queue, in what order should I look to wake threads
// to steal that work?" Ideally the thread that is woken is one that will
// check thread 3's queue early in its steal order, and by corollary will be
// in a nearby L3 cache.

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

std::vector<size_t> get_lattice_matrix(
  std::vector<tmc::topology::detail::CacheGroup> const& groupedCores
);

std::vector<size_t> get_hierarchical_matrix(
  std::vector<tmc::topology::detail::CacheGroup> const& groupedCores
);

std::vector<size_t>
invert_matrix(std::vector<size_t> const& InputMatrix, size_t N);

std::vector<size_t>
slice_matrix(std::vector<size_t> const& InputMatrix, size_t N, size_t Slot);

} // namespace detail
} // namespace tmc

#ifdef TMC_IMPL
#include "tmc/detail/thread_layout.ipp"
#endif
