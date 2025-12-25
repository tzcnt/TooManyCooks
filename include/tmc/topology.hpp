// Copyright (c) 2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

// TMC_USE_HWLOC must be enabled to make use of the data types in this file.

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

/// Specifies whether threads should be pinned/bound to specific cores, groups,
/// or NUMA nodes.
enum class ThreadPinningLevel {
  /// Threads will be pinned to individual physical cores. This is useful for
  /// applications where threads have exclusive access to cores.
  CORE,

  /// Threads may run on any core in their group. This prevent threads from
  /// being migrated across last-level caches, but allows flexibility in
  /// placement within that cache. This is optimal for interactive applications
  /// that run in the presence of external threads that may compete for the same
  /// execution resources.
  GROUP,

  /// Threads may run on any core in their NUMA node.
  NUMA,

  /// Threads may be moved freely by the OS.
  NONE
};

/// Specifiese how threads should be allocated when the thread occupancy is less
/// than the full system. This will only have any effect if `set_thread_count()`
/// is called with a number less than the count of physical cores in the system.
enum class ThreadPackingStrategy {
  /// Threads will be packed next to each other to maximize locality. Threads
  /// will be allocated at the low core indexes of the executor (core
  /// 0,1,2...).
  /// This optimizes for inter-thread work-stealing efficiency, at the expense
  /// of individual thread last-level cache space.
  PACK,

  /// Threads will be spread equally among the available thread groups in the
  /// executor. This will negatively impact work-stealing latency between
  /// groups, but allows individual threads to have more exclusive access to
  /// their own last-level cache.
  FAN
};

struct CoreGroup {
  /// Index of this group's NUMA node. Indexes start at 0 and count up.
  size_t numa_index;

  /// Index among all groups on this machine. Indexes start at 0 and count up.
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

/// Data passed into the callback that was provided to `set_thread_init_hook()`
/// and `set_thread_teardown_hook()`. Contains information about this
/// thread, and the thread group that it runs on.
struct ThreadInfo {
  /// The core group that this thread is part of.
  CoreGroup group;

  /// The index of this thread among all threads in its executor. Ranges from 0
  /// to thread_count() - 1.
  size_t index;

  /// The index of this thread among all threads in its group. Ranges from 0
  /// to thread_count() - 1.
  size_t index_within_group;
};

#ifdef TMC_USE_HWLOC
/// The public API for the TMC CPU topology. It exposes a view of "core groups",
/// which are used internally by TMC to construct the work-stealing matrix.
/// Cores are partitioned into groups based on shared cache and CPU kind.
///
/// This is a "plain old data" type with no internal or external references.
struct CpuTopology {
  /// Groups are sorted so that all fields are in strictly increasing order.
  /// That is, `groups[i].field < groups[i+1].field`, for any field.
  ///
  /// This means that Performance cores always come first in this ordering. This
  /// may differ from your OS ordering (some OS put Efficiency cores first).
  ///
  /// There is one exception: if your system has multiple NUMA nodes *and*
  /// multiple CPU kinds, the NUMA node will be the major sort dimension.
  std::vector<CoreGroup> groups;

  /// Core counts, grouped by CPU kind.
  /// Index 0 is the number of P-cores, or homogeneous cores.
  /// Index 1 (if it exists) is the number of E-cores.
  /// Index 2 (if it exists) is the number of LP E-cores.
  std::vector<size_t> cpu_kind_counts;

  /// Returns true if this machine has more than one CPU kind.
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
  std::vector<size_t> core_indexes_;
  std::vector<size_t> group_indexes_;
  std::vector<size_t> numa_indexes_;
  size_t cpu_kinds_ =
    tmc::topology::CpuKind::PERFORMANCE | tmc::topology::CpuKind::EFFICIENCY1;

public:
  /// Set the allowed core indexes.
  void set_core_indexes(std::vector<size_t> Indexes);

  /// Set the allowed group indexes.
  void set_group_indexes(std::vector<size_t> Indexes);

  /// Set the allowed NUMA indexes.
  void set_numa_indexes(std::vector<size_t> Indexes);

  // Set the allowed CPU kinds. The default value
  // is `(PERFORMANCE | EFFICIENCY1)`. `EFFICIENCY2` (LP E-cores) are excluded
  // by default, as they may not be suitable for general purpose computing.
  void set_cpu_kinds(tmc::topology::CpuKind::value CpuKinds);

  /// OR together two filters to produce a filter that allows elements that
  /// match any filter.
  TopologyFilter operator|(TopologyFilter const& rhs);

  /// Gets the allowed core indexes.
  std::vector<size_t> const& core_indexes() const;

  /// Gets the allowed group indexes.
  std::vector<size_t> const& group_indexes() const;

  /// Gets the allowed NUMA indexes.
  std::vector<size_t> const& numa_indexes() const;

  /// Gets the allowed CPU kinds. This is a bitmap that may combine multiple
  /// CpuKind values.
  size_t cpu_kinds() const;
};

/// Pins the current thread to the set of hardware resources defined by the
/// provided filter. You don't need to call this on any TMC executor threads,
/// but you can call it on an external thread so that it will reside in the same
/// portion of the processor as an executor that it communicates with.
///
/// On Apple platforms, direct thread pinning is not allowed. This will set the
/// QoS class based on the cpu_kind of the allowed resources instead. If the
/// allowed resources span multiple cpu_kinds, QoS will not be set.
void pin_thread(TopologyFilter Allowed);

#endif

} // namespace topology
} // namespace tmc

#ifdef TMC_IMPL
#include "tmc/detail/topology.ipp"
#endif
