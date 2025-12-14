// Copyright (c) 2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

// This entire file requires hwloc integration.
#ifdef TMC_USE_HWLOC
#include <vector>

namespace tmc {
namespace topology {
/// CPU kind types for hybrid architectures (P-cores vs E-cores).
/// CpuKind is a flags bitmap; you can OR together multiple flags to combine
/// them in a filter.
struct CpuKind {
  /// CPU kind types for hybrid architectures (P-cores vs E-cores).
  /// CpuKind is a flags bitmap; you can OR together multiple flags to combine
  /// them in a filter.
  enum value {
    PERFORMANCE = 1u, // P-Cores, or just regular cores
    EFFICIENCY1 = 2u, // E-Cores, Compact Cores, or Dense Cores
    EFFICIENCY2 = 4u, // Low Power E-Cores (e.g. Intel Meteor Lake)
    ALL = 7u,
  };
};

struct CoreGroup {
  /// Index of this group's NUMA node. Indexes start at 0 and count up.
  size_t numa_index;

  /// Index among all groups. Indexes start at 0 and count up.
  size_t index;

  /// Indexes of cores that are in this group. Indexes start at 0 in the first
  /// group, and count up. The index is global across all groups:
  /// `groups[0].core_indexes.back() + 1 == groups[1].core_indexes[0]`
  std::vector<size_t> core_indexes;

  /// All cores in this group will be of the same kind.
  CpuKind::value cpu_kind;

  /// SMT (hyperthreading) level of this group's CPU kind.
  /// If a core does not support SMT, this will be 1.
  /// Most consumer CPUs have SMT == 2.
  size_t smt_level;
};

/// The public API for the TMC CPU topology. It exposes a view of "core groups",
/// which are used internally by TMC to construct the work-stealing matrix.
/// Cores are partitioned into groups based on shared cache and CPU kind.
///
/// This is a "plain old data" type with no internal references.
struct CpuTopology {
  /// Groups are sorted so that all fields are in strictly increasing order.
  /// That is, `groups[i].field < groups[i+1].field`, for any field.
  ///
  /// There is one exception: if your system has multiple NUMA nodes *and*
  /// multiple CPU kinds, the NUMA node will be the major sort dimension.
  std::vector<CoreGroup> groups;

  /// Core counts, grouped by CPU kind.
  /// Index 0 is the number of P-cores.
  /// Index 1 (if it exists) is the number of E-cores.
  /// Index 2 (if it exists) is the number of LP E-cores.
  std::vector<size_t> cpu_kind_counts;

  /// Returns true if this machine has any efficiency cores.
  bool is_hybrid();

  /// The total number of logical processors (including SMT/hyperthreading).
  size_t pu_count();

  /// The total number of physical processors (not including
  /// SMT/hyperthreading).
  size_t core_count();

  /// The total number of core groups that TMC sees. These groups are based on
  /// shared caches and CPU kinds. For more detail on the group construction
  /// rules, see the documentation.
  size_t group_count();

  /// The total number of NUMA nodes.
  size_t numa_count();
};

/// Query the system CPU topology. Returns a copy of the topology; modifications
/// to the this copy will have no effect on other systems.
CpuTopology query();

class TopologyFilter {
public:
  std::vector<size_t> core_indexes;
  std::vector<size_t> group_indexes;
  std::vector<size_t> numa_indexes;
  size_t cpu_kinds =
    tmc::topology::CpuKind::PERFORMANCE | tmc::topology::CpuKind::EFFICIENCY1;

  void set_core_indexes(std::vector<size_t> Indexes);
  void set_group_indexes(std::vector<size_t> Indexes);
  void set_numa_indexes(std::vector<size_t> Indexes);
  // The default value is `(PERFORMANCE | EFFICIENCY1)`. `EFFICIENCY2` (LP
  // E-cores) are excluded by default, as they may not be suitable for general
  // purpose computing.
  void set_cpu_kinds(tmc::topology::CpuKind::value CpuKinds);
};

/// Binds the current thread to the set of hardware resources defined by the
/// provided filter. You don't need to call this on any TMC executor threads,
/// but you can call it on an external thread so that it will reside in the same
/// portion of the processor as an executor that it communicates with.
void bind_thread(TopologyFilter Allowed);
} // namespace topology
} // namespace tmc

#ifdef TMC_IMPL
#include "tmc/detail/topology.ipp"
#endif

#endif
