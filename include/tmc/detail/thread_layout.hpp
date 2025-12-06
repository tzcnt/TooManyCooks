// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#ifdef TMC_USE_HWLOC
#include <hwloc.h>
#include <mutex>
#endif
#include <vector>

void print_cpu_set(hwloc_cpuset_t CpuSet);

namespace tmc {
namespace detail {
struct L3CacheSet {
  // The type of `l3cache` is hwloc_obj_t. Stored as void* so this type can be
  // used when hwloc is not enabled. This minimizes code duplication
  // elsewhere.
  void* l3cache;
  size_t group_size;
  std::vector<size_t> puIndexes;
};

struct ThreadCoreGroup {
  /* Elements populated by topology */
  // The type of `cpuset` is hwloc_cpuset_t. Stored as void* so this type can be
  // used when hwloc is not enabled. This minimizes code duplication
  // elsewhere.
  void* obj;         // for thread binding
  size_t index;      // index among all groups (including empty groups)
  size_t core_count; // number of owned cores, excluding children
  size_t cpu_kind;
  // If this cache also has sub-cache groups
  std::vector<ThreadCoreGroup> children;

  /* Elements populated by make_thread_core_groups */
  size_t group_size; // number of threads (may differ from cores)
  // for waking from an external thread. may be outside this group
  // uses OS index
  std::vector<size_t> puIndexes;
};
} // namespace detail

#ifdef TMC_USE_HWLOC
namespace topology {

// Public topology query API

// struct TopologyLLC {
//   unsigned logical_index;
//   std::vector<unsigned> pu_logical_indexes;
// };

// struct TopologyNUMA {
//   unsigned os_index;
//   std::vector<unsigned> pu_logical_indexes;
// };

struct CpuTopology {
  struct TopologyPU {
    std::vector<hwloc_obj_t> pus;
    hwloc_obj_t core = nullptr;
    hwloc_obj_t llc = nullptr;
    hwloc_obj_t numa = nullptr;
    size_t cpu_kind = 0;
    size_t parent_idx = 0;
  };
  std::vector<TopologyPU> cores;
  std::vector<detail::ThreadCoreGroup> caches;
  size_t coreCount = 0;
  size_t llcCount = 0;
  size_t numaCount = 0;

  // pu_count == pus.size()

  // Heterogeneous core information (P-cores vs E-cores)
  bool has_efficiency_cores = false;
  size_t performance_core_count = 0;
  size_t efficiency_core_count = 0;

  // TODO iterate over sub-PUs
  inline size_t pu_count() { return 0; }
  inline size_t core_count() { return cores.size(); }
  inline size_t llc_count() { return llcCount; }
  inline size_t numa_count() { return numaCount; }

  // NUMALatency exposed by hwloc (stored in System Locality Distance
  // Information Table) is not helpful if the system is not confirmed as NUMA
  // Use l3 cache groupings instead
  // TODO handle non-uniform core layouts (Intel/ARM hybrid architecture)
  // https://utcc.utoronto.ca/~cks/space/blog/linux/IntelHyperthreadingSurprise
  std::vector<tmc::detail::L3CacheSet> group_cores_by_l3c();

  std::vector<tmc::detail::ThreadCoreGroup>
  make_thread_core_groups(hwloc_cpuset_t Partition);

  bool is_sorted();
};

namespace detail {
struct topo_data {
  std::mutex lock;
  hwloc_topology_t hwloc;
  CpuTopology tmc;
  bool ready = false;
};
// Constructing a topology is pretty slow (100ms) and it's accessed
// infrequently. The mutex is needed for any user operations that access this,
// to populate it lazily. It should always be constructed at executor
// init() or sooner, so if the executor needs to query it afterward in a
// read-only fashion, a mutex is not needed.

inline topo_data g_topo;
CpuTopology query_internal(hwloc_topology_t& HwlocTopo);
hwloc_obj_t find_parent_of_type(hwloc_obj_t Start, hwloc_obj_type_t Type);
hwloc_obj_t find_parent_cache(hwloc_obj_t Start);
void make_cache_parent_group(
  hwloc_obj_t parent, std::vector<tmc::detail::ThreadCoreGroup>& caches,
  std::vector<hwloc_obj_t>& work, size_t shareStart, size_t shareEnd
);
} // namespace detail

/// Query the system CPU topology. Returns information about processing units
/// (PUs), L3 cache groups, and NUMA nodes. This function is only available
/// when TMC_USE_HWLOC is defined.
CpuTopology query();

class TopologyFilter {
public:
  // TODO refactor each of these into a substruct
  // std::vector<size_t> pu_indexes;
  std::vector<size_t> core_indexes;
  std::vector<size_t> llc_indexes;
  std::vector<size_t> numa_indexes;
  // bool pu_logical = false;
  bool core_logical = false;
  bool llc_logical = false;
  bool numa_logical = false;
  size_t p_e_core = 0;

  // void set_pu_indexes(std::vector<size_t> Indexes, bool Logical = true);
  void set_core_indexes(std::vector<size_t> Indexes, bool Logical = true);
  void set_llc_indexes(std::vector<size_t> Indexes, bool Logical = true);
  void set_numa_indexes(std::vector<size_t> Indexes, bool Logical = true);
  void set_p_e_cores(bool ECore);
  bool active() const;
};
} // namespace topology
#endif

namespace detail {
#ifdef TMC_USE_HWLOC

// Modifies GroupedCores according to the number of found cores and requested
// values. Also modifies Lasso to determine whether thread lassoing should be
// enabled.
// Returns the PU-to-thread-index mapping used by notify_n.
std::vector<size_t> adjust_thread_groups(
  size_t RequestedThreadCount, float RequestedOccupancy,
  std::vector<ThreadCoreGroup>& GroupedCores, bool& Lasso
);

// bind this thread to any of the cores that share l3 cache in this set
void bind_thread(hwloc_topology_t Topology, hwloc_cpuset_t SharedCores);

// Apply a partition cpuset to L3CacheSet groups by filtering their group_size
// to only count cores within the partition
void apply_partition_to_groups(
  hwloc_topology_t Topology, hwloc_cpuset_t Partition,
  std::vector<L3CacheSet>& GroupedCores
);

void* make_partition_cpuset(
  void* Topology, tmc::topology::CpuTopology& TmcTopo,
  topology::TopologyFilter& Filter
);

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
  std::vector<tmc::detail::ThreadCoreGroup> const& groupedCores
);

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
