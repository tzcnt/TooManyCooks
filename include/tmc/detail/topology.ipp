// Copyright (c) 2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

// Implementation file for public topology API.
// Private topology API is in "tmc/detail/thread_layout.hpp".

#pragma once

#include "tmc/detail/compat.hpp"
#include "tmc/detail/thread_layout.hpp"
#include "tmc/topology.hpp"

#include <hwloc.h>

#include <algorithm>
#include <vector>

namespace tmc {
namespace topology {

bool CpuTopology::is_hybrid() { return cpu_kind_counts.size() > 1; }

size_t CpuTopology::pu_count() {
  size_t count = 0;
  for (auto& group : groups) {
    count += group.core_indexes.size() * group.smt_level;
  }
  return count;
}

size_t CpuTopology::core_count() {
  return groups.back().core_indexes.back() + 1;
}

size_t CpuTopology::group_count() { return groups.size(); }

size_t CpuTopology::numa_count() { return groups.back().numa_index + 1; }

CpuTopology query() {
  hwloc_topology_t unused;
  tmc::topology::detail::Topology privateTopo = detail::query_internal(unused);
  auto flatGroups = privateTopo.flatten();

  CpuTopology result;
  result.groups.resize(flatGroups.size());
  for (size_t i = 0; i < flatGroups.size(); ++i) {
    auto& in = *flatGroups[i];
    auto& out = result.groups[i];

    if (in.cores[0].numa != nullptr) {
      out.numa_index = in.cores[0].numa->logical_index;
    } else {
      out.numa_index = 0;
    }
    out.index = static_cast<size_t>(in.index);
    out.core_indexes.resize(in.cores.size());
    for (size_t j = 0; j < in.cores.size(); ++j) {
      out.core_indexes[j] = in.cores[j].index;
    }
    out.cpu_kind = static_cast<CpuKind::value>(TMC_ONE_BIT << in.cpu_kind);
    out.smt_level = in.cores[0].pus.size();
  }

  result.cpu_kind_counts = privateTopo.cpu_kind_counts;

  return result;
}

void TopologyFilter::set_core_indexes(std::vector<size_t> Indexes) {
  core_indexes = Indexes;
  std::sort(core_indexes.begin(), core_indexes.end());
}

void TopologyFilter::set_group_indexes(std::vector<size_t> Indexes) {
  group_indexes = Indexes;
  std::sort(group_indexes.begin(), group_indexes.end());
}

void TopologyFilter::set_numa_indexes(std::vector<size_t> Indexes) {
  numa_indexes = Indexes;
  std::sort(numa_indexes.begin(), numa_indexes.end());
}

void TopologyFilter::set_cpu_kinds(tmc::topology::CpuKind::value CpuKinds) {
  cpu_kinds = CpuKinds;
}

void bind_thread([[maybe_unused]] TopologyFilter Allowed) {
  // Apple doesn't support direct CPU binding.
  // User should set QoS class instead.
#ifndef __APPLE__
  hwloc_topology_t hwlocTopo;
  auto privateTopo = tmc::topology::detail::query_internal(hwlocTopo);
  auto partitionCpuset = static_cast<hwloc_cpuset_t>(
    tmc::detail::make_partition_cpuset(hwlocTopo, privateTopo, Allowed)
  );
  tmc::detail::bind_thread(hwlocTopo, partitionCpuset);
#endif
}

} // namespace topology
} // namespace tmc
