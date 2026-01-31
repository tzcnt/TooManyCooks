// Copyright (c) 2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

// Implementation file for public topology API.
// Private topology API is in "tmc/detail/thread_layout.hpp".

#pragma once

// All of the definitions in this file require hwloc.
#ifdef TMC_USE_HWLOC

#include "tmc/detail/compat.hpp"
#include "tmc/detail/container_cpu_quota.hpp"
#include "tmc/detail/hwloc_unique_bitmap.hpp"
#include "tmc/detail/thread_layout.hpp"
#include "tmc/topology.hpp"

#include <hwloc.h>

#include <algorithm>
#include <cassert>
#include <iterator>
#include <vector>

namespace tmc {
namespace topology {

bool cpu_topology::is_hybrid() const { return cpu_kind_counts.size() > 1; }

size_t cpu_topology::pu_count() const {
  size_t count = 0;
  for (auto& group : groups) {
    count += group.core_indexes.size() * group.smt_level;
  }
  return count;
}

size_t cpu_topology::core_count() const {
  assert(!groups.empty());
  assert(!groups.back().core_indexes.empty());
  return groups.back().core_indexes.back() + 1;
}

size_t cpu_topology::group_count() const { return groups.size(); }

size_t cpu_topology::numa_count() const {
  assert(!groups.empty());
  return groups.back().numa_index + 1;
}

cpu_topology query() {
  hwloc_topology_t unused;
  tmc::topology::detail::Topology privateTopo = detail::query_internal(unused);
  auto flatGroups = tmc::topology::detail::flatten_groups(privateTopo.groups);

  cpu_topology result;
  result.groups.resize(flatGroups.size());
  for (size_t i = 0; i < flatGroups.size(); ++i) {
    auto& in = *flatGroups[i];
    auto& out = result.groups[i];
    assert(!in.cores.empty());

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
    out.cpu_kind = static_cast<cpu_kind::value>(TMC_ONE_BIT << in.cpu_kind);
    out.smt_level = in.cores[0].pus.size();
  }

  result.cpu_kind_counts = privateTopo.cpu_kind_counts;

  auto containerQuota = tmc::detail::query_container_cpu_quota();
  result.container_cpu_quota = containerQuota.is_container_limited()
                                 ? static_cast<float>(containerQuota.cpu_count)
                                 : 0.0f;

  return result;
}

void topology_filter::set_core_indexes(std::vector<size_t> Indexes) {
  core_indexes_ = Indexes;
  std::sort(core_indexes_.begin(), core_indexes_.end());
}

void topology_filter::set_group_indexes(std::vector<size_t> Indexes) {
  group_indexes_ = Indexes;
  std::sort(group_indexes_.begin(), group_indexes_.end());
}

void topology_filter::set_numa_indexes(std::vector<size_t> Indexes) {
  numa_indexes_ = Indexes;
  std::sort(numa_indexes_.begin(), numa_indexes_.end());
}

void topology_filter::set_cpu_kinds(tmc::topology::cpu_kind::value CpuKinds) {
  cpu_kinds_ = CpuKinds;
}

std::vector<size_t> const& topology_filter::core_indexes() const {
  return core_indexes_;
}

std::vector<size_t> const& topology_filter::group_indexes() const {
  return group_indexes_;
}

std::vector<size_t> const& topology_filter::numa_indexes() const {
  return numa_indexes_;
}

size_t topology_filter::cpu_kinds() const { return cpu_kinds_; }

namespace {
std::vector<size_t> resolve_filter_to_cores(
  topology_filter const& filter, cpu_topology const& topo
) {
  std::vector<size_t> allowedCores;
  bool numaEmpty = filter.numa_indexes().empty();
  bool groupsEmpty = filter.group_indexes().empty();
  bool coresEmpty = filter.core_indexes().empty();

  for (auto const& group : topo.groups) {
    bool numaAllowed =
      numaEmpty || std::binary_search(
                     filter.numa_indexes().begin(), filter.numa_indexes().end(),
                     group.numa_index
                   );
    bool groupAllowed =
      groupsEmpty || std::binary_search(
                       filter.group_indexes().begin(),
                       filter.group_indexes().end(), group.index
                     );
    if (!numaAllowed || !groupAllowed) {
      continue;
    }
    for (size_t coreIdx : group.core_indexes) {
      bool coreAllowed = coresEmpty || std::binary_search(
                                         filter.core_indexes().begin(),
                                         filter.core_indexes().end(), coreIdx
                                       );
      if (coreAllowed) {
        allowedCores.push_back(coreIdx);
      }
    }
  }
  // Output will be in sorted order since input is in sorted order
  return allowedCores;
}
} // namespace

topology_filter topology_filter::operator|(topology_filter const& rhs) const {
  cpu_topology topo = query();

  std::vector<size_t> lhsCores = resolve_filter_to_cores(*this, topo);
  std::vector<size_t> rhsCores = resolve_filter_to_cores(rhs, topo);

  // Create a new filter that only includes the effective cores union
  topology_filter result;
  std::set_union(
    lhsCores.begin(), lhsCores.end(), rhsCores.begin(), rhsCores.end(),
    std::back_inserter(result.core_indexes_)
  );
  result.cpu_kinds_ = cpu_kinds_ | rhs.cpu_kinds_;
  return result;
}

void pin_thread([[maybe_unused]] topology_filter const& Allowed) {
  hwloc_topology_t hwlocTopo;
  auto privateTopo = tmc::topology::detail::query_internal(hwlocTopo);
  tmc::topology::cpu_kind::value cpuKind;
  tmc::detail::hwloc_unique_bitmap partitionCpuset =
    tmc::detail::make_partition_cpuset(
      hwlocTopo, privateTopo, Allowed, cpuKind
    );
  tmc::detail::pin_thread(hwlocTopo, partitionCpuset, cpuKind);
}

} // namespace topology
} // namespace tmc

#endif
