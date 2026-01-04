// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#include "tmc/detail/compat.hpp"
#include "tmc/detail/container_cpu_quota.hpp"
#include "tmc/detail/thread_layout.hpp"

#include <cassert>
#include <vector>

#ifdef TMC_USE_HWLOC
#include "tmc/detail/hwloc_unique_bitmap.hpp"
#include "tmc/topology.hpp"

#include <hwloc.h>
#ifdef __APPLE__
#include <pthread/qos.h>
#include <sys/qos.h>
#endif
#endif

#ifdef TMC_DEBUG_THREAD_CREATION
#include <cstdio>
#endif

namespace tmc {
namespace detail {

struct SubdivideNode {
  size_t lowIdx;
  size_t highIdx;
  size_t min; // inclusive
  size_t max; // exclusive
};

// Cut the range in half repeatedly.
static void recursively_subdivide(std::vector<SubdivideNode>& Results) {
  size_t idx = Results.size() - 1;
  SubdivideNode node = Results[idx];
  if (node.max - node.min > 1) {
    size_t half = (node.max - node.min) / 2;
    size_t mid = node.max - half;

    Results[idx].lowIdx = Results.size();
    Results.emplace_back(0, 0, node.min, mid);
    recursively_subdivide(Results);

    Results[idx].highIdx = Results.size();
    Results.emplace_back(0, 0, mid, node.max);
    recursively_subdivide(Results);
  }
}

// Attempt to preserve the same path structure as the starting node, but branch
// off earlier in the step. Each time we branch earlier, restore the original
// path structure except for that branch. Keeping the original path structure
// produces a more balanced work stealing network / matrix.
static void enumerate_paths(
  const size_t Path, size_t DepthBit,
  const std::vector<SubdivideNode>& PathTree, const size_t NodeIdx,
  std::vector<size_t>& Results
) {
  const SubdivideNode& node = PathTree[NodeIdx];
  if (node.lowIdx == 0 && node.highIdx == 0) {
    Results.push_back(node.min);
    return;
  }
  if ((Path & DepthBit) == 0) {
    if (node.lowIdx != 0) {
      enumerate_paths(Path, DepthBit << 1, PathTree, node.lowIdx, Results);
    }
    if (node.highIdx != 0) {
      enumerate_paths(
        Path ^ DepthBit, DepthBit << 1, PathTree, node.highIdx, Results
      );
    }
  } else {
    if (node.highIdx != 0) {
      enumerate_paths(Path, DepthBit << 1, PathTree, node.highIdx, Results);
    }
    if (node.lowIdx != 0) {
      enumerate_paths(
        Path ^ DepthBit, DepthBit << 1, PathTree, node.lowIdx, Results
      );
    }
  }
}

// CPUs with large numbers of independent L3 caches (e.g. AMD EPYC) have
// multiple tiers of latency domains. For example, the EPYC 7742 has 16 L3
// caches. These are grouped into 4 quarters (0-3, 4-7, 8-11, 12-15), each
// connected to a different quarter of the I/O die.
//
// From https://www.anandtech.com/show/16529/amd-epyc-milan-review/4, see
// https://images.anandtech.com/doci/16315/Bounce-7742.png and, reading across
// the top row, observe the breakpoints at index 16, 32, and 48, each of which
// corresponds to a different I/O die quarter.
//
// The CPU/system/hwloc does not report these latency domains when in UMA mode,
// so the hierarchy appears to be flat. The best we can do is just recursively
// subdivide the L3 caches into groups of 2. This should also handle CPUs that
// have odd numbers of L3 caches. On the EPYC 7742, this improves performance on
// Skynet by ~15% (!), while having no impact on systems with lesser numbers of
// caches.
std::vector<size_t>
get_flat_group_iteration_order(size_t GroupCount, size_t StartGroup) {
  if (GroupCount == 0) {
    return std::vector<size_t>{};
  }

  // Recursively halve each subrange of indices, and store the tree in a vector
  std::vector<SubdivideNode> pathTree;
  pathTree.reserve(GroupCount * 2);
  pathTree.emplace_back(0, 0, 0, GroupCount);
  recursively_subdivide(pathTree);

  // Find the path to the start node through the tree.
  // Encode it as a bitmap, where 0 = turning toward the low half of the range,
  // 1 = turning toward the high half of the range, starting from the low bit of
  // the bitmap: Low, High, High, Low, High -> 0b10110

  // the high bits are padded with 0s (0b0...00010110), which solves the
  // case of a start node on the short (high) side of the tree

  size_t startPath = 0;
  SubdivideNode node = pathTree[0];
  size_t depth = 0;
  {
    while (node.lowIdx != 0) {
      if (StartGroup < pathTree[node.lowIdx].max) {
        node = pathTree[node.lowIdx];
      } else {
        startPath |= (TMC_ONE_BIT << depth);
        node = pathTree[node.highIdx];
      }
      ++depth;
    }
    assert(node.min == StartGroup);
  }

  // Using the starting node's path, enumerate all paths through the tree.
  std::vector<size_t> groupOrder;
  groupOrder.reserve(GroupCount);
  enumerate_paths(startPath, 1, pathTree, 0, groupOrder);
  return groupOrder;
}

#ifdef TMC_USE_HWLOC
namespace {
struct FilterProcessor {
  size_t offset;
  std::vector<size_t> const& indexes;
  void process_next(size_t Idx, bool& Include_out) {
    if (indexes.empty()) {
      return;
    }
    while (true) {
      if (offset >= indexes.size() || indexes[offset] > Idx) {
        Include_out = false;
        break;
      } else if (indexes[offset] < Idx) {
        ++offset;
      } else {
        // indexes are equal; include stays true
        break;
      }
    }
  }
};
} // namespace

// GroupedCores is an input/output parameter.
// Lasso is an output parameter.
void adjust_thread_groups(
  size_t RequestedThreadCount, std::vector<float> RequestedOccupancy,
  std::vector<tmc::topology::detail::CacheGroup*> flatGroups,
  topology::topology_filter const& Filter,
  topology::thread_packing_strategy Pack
) {

  FilterProcessor coreProc{0, Filter.core_indexes()};
  FilterProcessor llcProc{0, Filter.group_indexes()};
  FilterProcessor numaProc{0, Filter.numa_indexes()};

  // If disallowed cores/groups, remove them from the working set by setting
  // their group_size to 0
  for (size_t i = 0; i < flatGroups.size(); ++i) {
    auto& group = *flatGroups[i];
    if (0 == (Filter.cpu_kinds() & (TMC_ONE_BIT << group.cpu_kind))) {
      group.group_size = 0;
      continue;
    }
    // This assumes that cores, caches, and numa nodes across the entire
    // topology are sorted in ascending order
    bool include = true;
    // Use our (TMC) index for caches, rather than hwloc's logical_index
    // Because our caches may be of different levels
    llcProc.process_next(static_cast<size_t>(group.index), include);
    if (!include) {
      group.group_size = 0;
      continue;
    }
    for (size_t j = 0; j < group.cores.size(); ++j) {
      include = true;
      auto& core = group.cores[j];
      if (include && core.numa != nullptr) {
        numaProc.process_next(core.numa->logical_index, include);
      }
      if (!include) {
        group.group_size = 0;
        break;
      }

      include = true;
      coreProc.process_next(core.index, include);
      if (!include) {
        --group.group_size;
      }

      if (!include) {
        // We only need to remove from cores if we are editing a single
        // core. Other filters just set the entire group_size to 0.
        group.cores.erase(group.cores.begin() + static_cast<ptrdiff_t>(j));
        --j;
      }
    }
  }

  if (!RequestedOccupancy.empty()) {
    // If occupancy, set group_size of affected groups by multiplying the
    // original group_size (which will be 0 if disallowed)
    for (size_t i = 0; i < flatGroups.size(); ++i) {
      auto& group = *flatGroups[i];
      float occ = RequestedOccupancy[group.cpu_kind];
      if (group.group_size != 0 && occ != 1.0f) {
        auto newSize =
          static_cast<size_t>(occ * static_cast<float>(group.group_size));
        if (newSize == 0) {
          newSize = 1;
        }
        group.group_size = newSize;
      }
    }
  }

  // If running in a container with CPU limits, set thread count to quota, if a
  // specific number of threads was not requested
  if (RequestedThreadCount == 0) {
    auto containerQuota = tmc::detail::query_container_cpu_quota();
    if (containerQuota.is_container_limited()) {
      size_t containerLimit = static_cast<size_t>(containerQuota.cpu_count);
      if (containerLimit == 0) {
        containerLimit = 1;
      }
      RequestedThreadCount = containerLimit;
    }
  }

  // Count the number of threads so far
  size_t totalSize = 0;
  for (size_t i = 0; i < flatGroups.size(); ++i) {
    auto& group = *flatGroups[i];
    totalSize += group.group_size;
  }

#ifndef TMC_MORE_THREADS
  if ((RequestedThreadCount == 0 && totalSize > TMC_PLATFORM_BITS) ||
      RequestedThreadCount > TMC_PLATFORM_BITS) {
    RequestedThreadCount = TMC_PLATFORM_BITS;
  }
#endif

  if (RequestedThreadCount != 0) {
    if (totalSize < RequestedThreadCount) {
      // Add threads to P-cores up to SMT limit,
      // then distribute the remainder among P- and E-cores
      bool increasedSmt;
      do {
        increasedSmt = false;
        for (size_t i = 0; i < flatGroups.size(); ++i) {
          auto& group = *flatGroups[i];
          if (totalSize == RequestedThreadCount) {
            break;
          }
          if (group.group_size == 0) {
            continue;
          }
          size_t smtLevel = group.cores[0].pus.size();
          if (group.group_size < group.cores.size() * smtLevel) {
            ++group.group_size;
            ++totalSize;
            increasedSmt = true;
          }
        }
      } while (totalSize < RequestedThreadCount && increasedSmt == true);

      while (totalSize < RequestedThreadCount) {
        // SMT has been fully applied to all cores, but the user asked for even
        // more threads. Start distributing them evenly amongst all the groups.
        // Still, start with group 0 as that's where the P-cores are.
        for (size_t i = 0; i < flatGroups.size(); ++i) {
          auto& group = *flatGroups[i];
          if (totalSize == RequestedThreadCount) {
            break;
          }
          if (group.group_size == 0) {
            continue;
          }
          ++group.group_size;
          ++totalSize;
        }
      }
    }
    TMC_DISABLE_WARNING_SWITCH_DEFAULT_BEGIN
    switch (Pack) {
    case tmc::topology::thread_packing_strategy::PACK:
      if (totalSize > RequestedThreadCount) {
        // Remove threads from groups by iterating backward, completely
        // depleting each group before moving to the next
        for (size_t i = flatGroups.size() - 1; i != TMC_ALL_ONES; --i) {
          auto& group = *flatGroups[i];
          if (totalSize == RequestedThreadCount) {
            break;
          }
          auto diff = totalSize - RequestedThreadCount;
          if (diff >= group.group_size) {
            totalSize -= group.group_size;
            group.group_size = 0;
          } else {
            totalSize = RequestedThreadCount;
            group.group_size -= diff;
          }
        }
      }
      break;
    case tmc::topology::thread_packing_strategy::FAN:
      if (totalSize > RequestedThreadCount) {
        // Find the smallest non-empty group
        size_t smallest = TMC_ALL_ONES;
        for (size_t i = 0; i < flatGroups.size(); ++i) {
          auto& group = *flatGroups[i];
          if (group.group_size != 0 && group.group_size < smallest) {
            smallest = group.group_size;
          }
        }
        // Shrink down each group evenly until they are all equally sized
        while (totalSize > RequestedThreadCount) {
          bool removed = false;
          for (size_t i = 0; i < flatGroups.size(); ++i) {
            auto& group = *flatGroups[i];
            if (totalSize == RequestedThreadCount) {
              break;
            }
            if (group.group_size > smallest) {
              removed = true;
              --group.group_size;
              --totalSize;
            }
          }
          if (!removed) {
            break;
          }
        }
        // Shrink down each group, 1 by 1, iterating backward, as the last
        // groups are where the E-cores are (if they exist)
        while (totalSize > RequestedThreadCount) {
          for (size_t i = flatGroups.size() - 1; i != TMC_ALL_ONES; --i) {
            auto& group = *flatGroups[i];
            if (totalSize == RequestedThreadCount) {
              break;
            }
            if (group.group_size == 0) {
              continue;
            }
            --group.group_size;
            --totalSize;
          }
        }
      }

      break;
    }
    TMC_DISABLE_WARNING_SWITCH_DEFAULT_END
  }

  size_t start = 0;
  for (size_t i = 0; i < flatGroups.size(); ++i) {
    auto& group = *flatGroups[i];
    group.group_start = start;
    start += group.group_size;
  }
}

void pin_thread(
  [[maybe_unused]] hwloc_topology_t Topology,
  [[maybe_unused]] tmc::detail::hwloc_unique_bitmap& CpuSet,
  [[maybe_unused]] tmc::topology::cpu_kind::value Kind
) {
#ifdef __APPLE__
  // CPU binding doesn't work on MacOS, so we have to set the QoS class instead.
  // (user can override it with set_thread_init_hook)

  // Cast to size_t to clarify that the default case is necessary, since this is
  // really a bitflags that may contain multiple bindings.
  switch (static_cast<size_t>(Kind)) {
  case tmc::topology::cpu_kind::PERFORMANCE:
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    break;
  case tmc::topology::cpu_kind::EFFICIENCY1:
  case tmc::topology::cpu_kind::EFFICIENCY2:
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
    break;
  case tmc::topology::cpu_kind::ALL:
    break;
  default:
    break;
  }
#else
  if (0 == hwloc_set_cpubind(
             Topology, CpuSet, HWLOC_CPUBIND_THREAD | HWLOC_CPUBIND_STRICT
           )) {
  } else if (0 == hwloc_set_cpubind(Topology, CpuSet, HWLOC_CPUBIND_THREAD)) {
  } else {
#ifdef TMC_DEBUG_THREAD_CREATION
    std::printf("FAIL to lasso thread to cpuset bitmap: ");
    CpuSet.print();
#endif
  }
#endif
}

tmc::detail::hwloc_unique_bitmap make_partition_cpuset(
  void* HwlocTopo, tmc::topology::detail::Topology& TmcTopo,
  tmc::topology::topology_filter const& Filter,
  tmc::topology::cpu_kind::value& CpuKind_out
) {
  auto topo = static_cast<hwloc_topology_t>(HwlocTopo);
  auto flatGroups = tmc::topology::detail::flatten_groups(TmcTopo.groups);

  hwloc_unique_bitmap work = hwloc_bitmap_alloc();
  tmc::detail::hwloc_unique_bitmap finalCpuset =
    hwloc_bitmap_dup(hwloc_topology_get_allowed_cpuset(topo));

  auto& f = Filter;
  FilterProcessor coreProc{0, f.core_indexes()};
  FilterProcessor cacheProc{0, f.group_indexes()};
  FilterProcessor numaProc{0, f.numa_indexes()};
  for (size_t i = 0; i < flatGroups.size(); ++i) {
    auto& group = *flatGroups[i];
    bool include = true;
    // Use our (TMC) index for caches, rather than hwloc's logical_index
    // Because our caches may be of different levels
    cacheProc.process_next(static_cast<size_t>(group.index), include);
    if (!include) {
      // Exclude the individual cores of the cache and not the cache object's
      // cpuset, as we may have partitioned based on CpuKind, but the cache
      // object would contain the original cpuset with all the cores in it.
      for (size_t j = 0; j < group.cores.size(); ++j) {
        auto& core = group.cores[j];
        hwloc_bitmap_not(work, core.cpuset);
        hwloc_bitmap_and(finalCpuset, finalCpuset, work);
      }
    }
  }
  for (size_t i = 0; i < TmcTopo.cores.size(); ++i) {
    auto& core = TmcTopo.cores[i];
    bool include = true;
    if (0 == (Filter.cpu_kinds() & (TMC_ONE_BIT << core.cpu_kind))) {
      include = false;
    }
    if (include) {
      coreProc.process_next(core.index, include);
    }
    if (include && core.numa != nullptr) {
      numaProc.process_next(core.numa->logical_index, include);
    }

    if (!include) {
      hwloc_bitmap_not(work, core.cpuset);
      hwloc_bitmap_and(finalCpuset, finalCpuset, work);
    }
  }
  [[maybe_unused]] auto allowedPUCount = hwloc_bitmap_weight(finalCpuset);
  assert(
    allowedPUCount != 0 && "Partition resulted in 0 allowed processing units."
  );

  std::vector<tmc::detail::hwloc_unique_bitmap> kindCpuSets;
  size_t cpuKindCount = static_cast<size_t>(hwloc_cpukinds_get_nr(topo, 0));
  if (cpuKindCount == 0) {
    // VMs may not expose CPU kind info
    cpuKindCount = 1;
    kindCpuSets.resize(1);
    kindCpuSets[0] = hwloc_bitmap_dup(hwloc_topology_get_allowed_cpuset(topo));
  } else {
    kindCpuSets.resize(cpuKindCount);
    for (unsigned idx = 0; idx < static_cast<unsigned>(cpuKindCount); ++idx) {
      hwloc_bitmap_t cpuset = hwloc_bitmap_alloc();
      int efficiency;
      // Get the cpuset and info for this kind
      // hwloc's "efficiency value" actually means "performance"
      // this sorts kinds by increasing efficiency value (E-cores first)
      hwloc_cpukinds_get_info(
        topo, idx, cpuset, &efficiency, nullptr, nullptr, 0
      );

      // Reverse the ordering so P-cores are first
      kindCpuSets[cpuKindCount - 1 - idx] = cpuset;
    }
  }
  size_t cpuKindResult = 0;
  for (size_t i = 0; i < kindCpuSets.size(); ++i) {
    if (hwloc_bitmap_intersects(kindCpuSets[i], finalCpuset)) {
      cpuKindResult |= (TMC_ONE_BIT << i);
      break;
    }
  }
  CpuKind_out = static_cast<tmc::topology::cpu_kind::value>(cpuKindResult);

  return finalCpuset;
}

// Partially duplicates logic in tmc::topology::query()
tmc::topology::core_group
public_group_from_private(tmc::topology::detail::CacheGroup& Input) {
  tmc::topology::core_group result;

  if (Input.cores[0].numa != nullptr) {
    result.numa_index = Input.cores[0].numa->logical_index;
  } else {
    result.numa_index = 0;
  }
  result.index = static_cast<size_t>(Input.index);
  result.core_indexes.resize(Input.cores.size());
  for (size_t j = 0; j < Input.cores.size(); ++j) {
    result.core_indexes[j] = Input.cores[j].index;
  }
  result.cpu_kind =
    static_cast<tmc::topology::cpu_kind::value>(TMC_ONE_BIT << Input.cpu_kind);
  result.smt_level = Input.cores[0].pus.size();

  return result;
}

#endif

ThreadCacheGroupIterator::ThreadCacheGroupIterator(
  std::vector<tmc::topology::detail::CacheGroup>& GroupedCores,
  std::function<void(tmc::topology::detail::CacheGroup&)> Process
)
    : process_{Process} {
  states_.push_back(
    {0, GroupedCores,
     tmc::detail::get_flat_group_iteration_order(GroupedCores.size(), 0)}
  );
}
// Advances the iterator to the next element of the tree and invokes the
// process function on it. Returns false if no elements remain to visit.
bool ThreadCacheGroupIterator::next() {
  if (states_.empty()) {
    return false;
  }
  while (true) {
    auto& state = states_.back();
    size_t idx = state.order[state.orderIdx];
    auto& group = state.cores[idx];
    if (!group.children.empty()) {
      // recurse into the child
      states_.push_back(
        {0, group.children,
         tmc::detail::get_flat_group_iteration_order(group.children.size(), 0)}
      );
    } else {
      // The groups already have the correct global index
      process_(group);

      while (true) {
        ++states_.back().orderIdx;
        if (states_.back().orderIdx < states_.back().order.size()) {
          break;
        }
        states_.pop_back();
        if (states_.empty()) {
          break;
        }
      }
      return true;
    }
  }
}
void for_all_groups(
  std::vector<tmc::topology::detail::CacheGroup>& groups,
  std::function<void(tmc::topology::detail::CacheGroup&)> process
) {
  ThreadCacheGroupIterator iter(groups, process);
  while (iter.next()) {
  }
}

class WorkStealingMatrixIterator : public ThreadCacheGroupIterator {
public:
  // After calling next(), read this field to get the result.
  std::vector<tmc::topology::detail::CacheGroup> Output;

  WorkStealingMatrixIterator(
    std::vector<tmc::topology::detail::CacheGroup>& GroupedCores
  )
      : ThreadCacheGroupIterator(
          GroupedCores,
          [this](tmc::topology::detail::CacheGroup&) { get_group_order(); }
        ) {}

private:
  void get_group_order() {
    Output.clear();
    auto myState = states_;
    // Rewrite each level of the starting state stack to use our local
    // ordering. Iteration begins from our current index.
    for (size_t i = 0; i < myState.size(); ++i) {
      auto& state = myState[i];
      auto localStart = state.order[state.orderIdx];
      state.order = tmc::detail::get_flat_group_iteration_order(
        state.cores.size(), localStart
      );
      state.orderIdx = 0;
    }

    std::vector<size_t> startIndexes;
    startIndexes.resize(myState.size());
    for (size_t i = 0; i < startIndexes.size(); ++i) {
      startIndexes[i] = myState[i].order[0];
    }

    while (true) {
      auto& state = myState.back();
      size_t idx = state.order[state.orderIdx];
      auto& group = state.cores[idx];
      if (!group.children.empty()) {
        // recurse into the child
        auto depth = myState.size();
        size_t childStartIdx;
        if (depth < startIndexes.size()) {
          childStartIdx = startIndexes[depth] % group.children.size();
        } else {
          // Child is deeper than where we started... wrap around based on
          // current level
          childStartIdx = startIndexes.back() % group.children.size();
        }
        myState.push_back(
          {0, group.children,
           tmc::detail::get_flat_group_iteration_order(
             group.children.size(), childStartIdx
           )}
        );
      } else {
        // The groups already have the correct global index
        Output.push_back(group);

        while (true) {
          ++myState.back().orderIdx;
          if (myState.back().orderIdx < myState.back().order.size()) {
            break;
          }
          myState.pop_back();
          if (myState.empty()) {
            return;
          }
        }
      }
    }
  }
};

// A more complex work stealing matrix that distributes work more rapidly
// across core groups.
std::vector<size_t> get_lattice_matrix(
  std::vector<tmc::topology::detail::CacheGroup> const& hierarchy
) {
  assert(!hierarchy.empty());
  WorkStealingMatrixIterator iter(
    // This iter doesn't modify, but it's convenient to build on top of
    // general iterator class which might modify.
    const_cast<std::vector<tmc::topology::detail::CacheGroup>&>(hierarchy)
  );
  iter.next();
  auto& groupedCores = iter.Output;
  // groupedCores now contains the flattened hierachy (only leaf nodes)
  // in the order that they will be visited by group 0

  tmc::detail::ThreadSetupData TData;
  TData.total_size = 0;
  TData.groups.resize(groupedCores.size());
  size_t groupStart = 0;
  for (size_t i = 0; i < groupedCores.size(); ++i) {
    assert(static_cast<size_t>(groupedCores[i].index) == i);
    size_t groupSize = groupedCores[i].group_size;
    TData.groups[i].size = groupSize;
    TData.groups[i].start = groupStart;
    TData.groups[i].stolenFromIdx = 0;
    groupStart += groupSize;
  }
  TData.total_size = groupStart;

  size_t total = TData.total_size * TData.total_size;
  std::vector<size_t> forward;
  forward.reserve(total);

  // Iterate over each shared cache
  do {
    // Iterate over each core within this shared cache
    // group_size may be 0, in which case it will be skipped
    auto& coreGroup = groupedCores[0];
    size_t groupSize = coreGroup.group_size;
    for (size_t SubIdx = 0; SubIdx < groupSize; ++SubIdx) {
      // Calculate entire iteration order in advance and cache it.
      // The resulting order will be:
      // This thread
      // Other threads in this thread's group
      // 1 thread from each other group (with same slot_off as this)
      // Remaining threads

      // This thread + other threads in this group
      {
        auto& group = TData.groups[static_cast<size_t>(coreGroup.index)];
        for (size_t off = 0; off < group.size; ++off) {
          size_t sidx = (SubIdx + off) % group.size;
          size_t val = sidx + group.start;
          forward.push_back(val);
        }
      }

      // 1 peer thread from each other group (with same sub_idx as this)
      // groups may have different sizes, so use modulo
      for (size_t groupOff = 1; groupOff < groupedCores.size(); ++groupOff) {
        size_t gidx = static_cast<size_t>(groupedCores[groupOff].index);
        auto& group = TData.groups[gidx];
        if (group.size == 0) {
          continue;
        }
        size_t sidx;
        if (TData.groups[static_cast<size_t>(coreGroup.index)].size ==
            group.size) {
          sidx = SubIdx % group.size;
        } else {
          sidx = group.stolenFromIdx % group.size;
        }
        size_t val = sidx + group.start;
        forward.push_back(val);
      }

      // Remaining threads from other groups (1 group at a time)
      for (size_t groupOff = 1; groupOff < groupedCores.size(); ++groupOff) {
        size_t gidx = static_cast<size_t>(groupedCores[groupOff].index);
        auto& group = TData.groups[gidx];
        if (group.size == 0) {
          continue;
        }
        for (size_t off = 1; off < group.size; ++off) {
          size_t sidx;
          if (TData.groups[static_cast<size_t>(coreGroup.index)].size ==
              group.size) {
            sidx = (SubIdx + off) % group.size;
          } else {
            sidx = (group.stolenFromIdx + off) % group.size;
          }
          size_t val = sidx + group.start;
          forward.push_back(val);
        }
        if (TData.groups[static_cast<size_t>(coreGroup.index)].size !=
            group.size) {
          ++group.stolenFromIdx;
        }
      }
    }
  } while (iter.next());
  assert(forward.size() == TData.total_size * TData.total_size);
  return forward;
}

// A work-stealing matrix based on purely hierarchical behavior.
// Threads will always steal from the closest available NUCA peer.
std::vector<size_t> get_hierarchical_matrix(
  std::vector<tmc::topology::detail::CacheGroup> const& hierarchy
) {
  assert(!hierarchy.empty());
  WorkStealingMatrixIterator iter(
    // This iter doesn't modify, but it's convenient to build on top of
    // general iterator class which might modify.
    const_cast<std::vector<tmc::topology::detail::CacheGroup>&>(hierarchy)
  );
  iter.next();
  auto& groupedCores = iter.Output;

  tmc::detail::ThreadSetupData TData;
  TData.total_size = 0;
  TData.groups.resize(groupedCores.size());
  size_t groupStart = 0;
  for (size_t i = 0; i < groupedCores.size(); ++i) {
    size_t groupSize = groupedCores[i].group_size;
    TData.groups[i].size = groupSize;
    TData.groups[i].start = groupStart;
    TData.groups[i].stolenFromIdx = 0;
    groupStart += groupSize;
  }
  TData.total_size = groupStart;

  size_t total = TData.total_size * TData.total_size;
  std::vector<size_t> forward;
  forward.reserve(total);
  // Iterate over each shared cache
  do {
    // Iterate over each core within this shared cache
    // group_size may be 0, in which case it will be skipped
    auto& coreGroup = groupedCores[0];
    size_t groupSize = coreGroup.group_size;
    for (size_t SubIdx = 0; SubIdx < groupSize; ++SubIdx) {
      // Calculate entire iteration order in advance and cache it.
      // The resulting order will be:
      // This thread
      // Other threads in this thread's group
      // threads in each other group, with groups ordered by hierarchy

      for (size_t groupOff = 0; groupOff < groupedCores.size(); ++groupOff) {
        size_t gidx = static_cast<size_t>(groupedCores[groupOff].index);
        auto& group = TData.groups[gidx];
        if (group.size == 0) {
          continue;
        }
        for (size_t off = 0; off < group.size; ++off) {
          size_t sidx;
          if (TData.groups[static_cast<size_t>(coreGroup.index)].size ==
              group.size) {
            sidx = (SubIdx + off) % group.size;
          } else {
            sidx = (group.stolenFromIdx + off) % group.size;
          }
          size_t val = sidx + group.start;
          forward.push_back(val);
        }
        if (TData.groups[static_cast<size_t>(coreGroup.index)].size !=
            group.size) {
          ++group.stolenFromIdx;
        }
      }
    }
  } while (iter.next());
  assert(forward.size() == TData.total_size * TData.total_size);
  return forward;
}

// For each thread, find the threads that will search it to steal from
// soonest. These are the threads that should be woken first to steal from
// this thread.
std::vector<size_t>
invert_matrix(std::vector<size_t> const& InputMatrix, size_t N) {
  std::vector<size_t> output;
  output.resize(N * N);
  // Same as doing push_back to a vector<vector> but we know the fixed size in
  // advance. So just track the sub-index (col) of each nested vector (row).
  std::vector<size_t> outCols(N, 0);
  for (size_t col = 0; col < N; ++col) {
    for (size_t row = 0; row < N; ++row) {
      size_t val = InputMatrix[row * N];
      size_t outRow = InputMatrix[row * N + col];
      size_t outCol = outCols[outRow];
      output[outRow * N + outCol] = val;
      ++outCols[outRow];
    }
  }
  return output;
}

std::vector<size_t>
slice_matrix(std::vector<size_t> const& InputMatrix, size_t N, size_t Slot) {
  std::vector<size_t> output;
  output.resize(N);
  size_t base = Slot * N;
  for (size_t i = 0; i < N; ++i) {
    output[i] = InputMatrix[base + i];
  }
  return output;
}

} // namespace detail

namespace topology {

#ifdef TMC_USE_HWLOC
namespace detail {
std::vector<tmc::topology::detail::CacheGroup*>
flatten_groups(std::vector<tmc::topology::detail::CacheGroup>& Groups) {
  std::vector<tmc::topology::detail::CacheGroup*> flatGroups;
  tmc::detail::for_all_groups(
    Groups, [&flatGroups](tmc::topology::detail::CacheGroup& group) {
      flatGroups.push_back(&group);
    }
  );
  return flatGroups;
}

hwloc_obj_t find_parent_of_type(hwloc_obj_t Start, hwloc_obj_type_t Type) {
  hwloc_obj_t curr = Start->parent;
  while (curr != nullptr && curr->type != Type) {
    curr = curr->parent;
  }
  return curr;
}
hwloc_obj_t find_parent_cache(hwloc_obj_t Start) {
  hwloc_obj_t curr = Start;
  while (curr->parent != nullptr) {
    curr = curr->parent;
    if (curr->type >= HWLOC_OBJ_L1CACHE && curr->type <= HWLOC_OBJ_L5CACHE) {
      return curr;
    }
  }
  return Start;
}

void make_cache_parent_group(
  hwloc_obj_t parent, std::vector<tmc::topology::detail::CacheGroup>& caches,
  std::vector<hwloc_obj_t>& work, size_t shareStart, size_t shareEnd
) {
  tmc::topology::detail::CacheGroup newGroup{};
  newGroup.obj = parent;
  newGroup.index = -1;
  newGroup.cpu_kind = caches[shareStart].cpu_kind;
  for (size_t j = shareStart; j < shareEnd; ++j) {
    auto& child = caches[j];
    newGroup.children.push_back(child);
  }
  // Overwrite shareStart with the new group, and erase the children
  caches[shareStart] = newGroup;
  caches.erase(
    caches.begin() + static_cast<ptrdiff_t>(shareStart + 1),
    caches.begin() + static_cast<ptrdiff_t>(shareEnd)
  );

  // Also erase the working set elements (which are already rolled up to the
  // parent cache level)
  work.erase(
    work.begin() + static_cast<ptrdiff_t>(shareStart + 1),
    work.begin() + static_cast<ptrdiff_t>(shareEnd)
  );
}

void query_internal_parse(hwloc_topology_t& topo, detail::Topology& topology) {
  static_assert(
    HWLOC_API_VERSION >= 0x00020100 &&
    "libhwloc 2.1 or newer is required for CPU kind detection"
  );
  {
    std::vector<tmc::detail::hwloc_unique_bitmap> kindCpuSets;
    size_t cpuKindCount = static_cast<size_t>(hwloc_cpukinds_get_nr(topo, 0));
    if (cpuKindCount == 0) {
      // VMs may not expose CPU kind info
      cpuKindCount = 1;
      kindCpuSets.resize(1);
      kindCpuSets[0] =
        hwloc_bitmap_dup(hwloc_topology_get_allowed_cpuset(topo));
    } else {
      kindCpuSets.resize(cpuKindCount);
      for (unsigned idx = 0; idx < static_cast<unsigned>(cpuKindCount); ++idx) {
        hwloc_bitmap_t cpuset = hwloc_bitmap_alloc();
        int efficiency;
        // Get the cpuset and info for this kind
        // hwloc's "efficiency value" actually means "performance"
        // this sorts kinds by increasing efficiency value (E-cores first)
        hwloc_cpukinds_get_info(
          topo, idx, cpuset, &efficiency, nullptr, nullptr, 0
        );

        // Reverse the ordering so P-cores are first
        kindCpuSets[cpuKindCount - 1 - idx] = cpuset;
      }
    }

    size_t numaCount =
      static_cast<size_t>(hwloc_get_nbobjs_by_type(topo, HWLOC_OBJ_NUMANODE));
    if (numaCount == 0) {
      numaCount = 1;
    }

    std::vector<std::vector<std::vector<TopologyCore>>> coresByNumaAndKind;
    coresByNumaAndKind.resize(numaCount);
    for (size_t i = 0; i < coresByNumaAndKind.size(); ++i) {
      coresByNumaAndKind[i].resize(cpuKindCount);
    }

    hwloc_obj_t curr = hwloc_get_root_obj(topo);
    std::vector<size_t> childIdx(1, 0);

    // Traverse the tree to collect info about pus, cores, last-level caches,
    // and numa nodes. This has to be done instead of using
    // hwloc_get_nbobjs_by_type() because hybrid core machines may expose
    // irregular structures.
    TopologyCore* core = nullptr;
    while (true) {
      if (childIdx.back() == 0) {
        if (curr->type == HWLOC_OBJ_CORE) {
          size_t kind = 0;
          for (size_t i = 0; i < kindCpuSets.size(); ++i) {
            if (hwloc_bitmap_intersects(kindCpuSets[i], curr->cpuset)) {
              kind = i;
              break;
            }
          }

          hwloc_obj_t numa = nullptr;
          hwloc_obj_t cache = nullptr;

          hwloc_obj_t parent = curr->parent;
          TMC_DISABLE_WARNING_SWITCH_DEFAULT_BEGIN
          while (parent != nullptr) {
            switch (parent->type) {
            case HWLOC_OBJ_L1CACHE:
            case HWLOC_OBJ_L2CACHE:
            case HWLOC_OBJ_L3CACHE:
            case HWLOC_OBJ_L4CACHE:
            case HWLOC_OBJ_L5CACHE:
              // Since we're traversing up from the bottom, the first cache we
              // find will be the first-level cache. A later step will check if
              // adjacent cores share a cache and roll up to the last-level
              // cache.
              if (cache == nullptr) {
                cache = parent;
              }
              break;
            case HWLOC_OBJ_NUMANODE:
              numa = parent;
              break;
            case HWLOC_OBJ_CORE:
            case HWLOC_OBJ_MACHINE:
            case HWLOC_OBJ_PACKAGE:
            case HWLOC_OBJ_PU:
            case HWLOC_OBJ_L1ICACHE:
            case HWLOC_OBJ_L2ICACHE:
            case HWLOC_OBJ_L3ICACHE:
            case HWLOC_OBJ_GROUP:
            case HWLOC_OBJ_BRIDGE:
            case HWLOC_OBJ_PCI_DEVICE:
            case HWLOC_OBJ_OS_DEVICE:
            case HWLOC_OBJ_MISC:
            case HWLOC_OBJ_MEMCACHE:
            case HWLOC_OBJ_DIE:
            case HWLOC_OBJ_TYPE_MAX:
              break;
            }
            parent = parent->parent;
          }
          TMC_DISABLE_WARNING_SWITCH_DEFAULT_END

          size_t numaIdx = 0;
          if (numa != nullptr) {
            numaIdx = numa->logical_index;
          }
          coresByNumaAndKind[numaIdx][kind].emplace_back();
          core = &coresByNumaAndKind[numaIdx][kind].back();
          core->cpuset = curr->cpuset;
          core->cache = cache;
          core->numa = numa;
          core->cpu_kind = kind;
        } else if (curr->type == HWLOC_OBJ_PU) {
          // Assume we can use the "core" as the smallest unit, and that PUs
          // will always be descendants of cores. This doesn't work on IBM
          // PPC64.
          core->pus.push_back(curr);
        }
      }

      if (childIdx.back() >= curr->arity) {
        childIdx.pop_back();
        if (childIdx.empty()) {
          break;
        }
        curr = curr->parent;
        ++childIdx.back();
      } else {
        curr = curr->children[childIdx.back()];
        childIdx.push_back(0);
      }
    }

    size_t coreIdx = 0;
    topology.cpu_kind_counts.resize(cpuKindCount);
    for (size_t numaIdx = 0; numaIdx < coresByNumaAndKind.size(); ++numaIdx) {
      for (size_t kindIdx = 0; kindIdx < coresByNumaAndKind[numaIdx].size();
           ++kindIdx) {
        auto& kind = coresByNumaAndKind[numaIdx][kindIdx];
        topology.cpu_kind_counts[kindIdx] += kind.size();
        for (size_t k = 0; k < kind.size(); ++k) {
          // Windows/Mac put E-cores first, but Linux puts P-cores
          // first. Reindex the topology cores list in order by NUMA node and
          // CPU kind. This ensures that we get a consistent ordering on all
          // platforms.
          topology.cores.push_back(kind[k]);
          topology.cores.back().index = coreIdx;
          ++coreIdx;
        }
      }
    }
  }

  // We are going to loop over these enums. Make sure hwloc hasn't changed
  // them.
  static_assert(HWLOC_OBJ_L1CACHE + 1 == HWLOC_OBJ_L2CACHE);
  static_assert(HWLOC_OBJ_L2CACHE + 1 == HWLOC_OBJ_L3CACHE);
  static_assert(HWLOC_OBJ_L3CACHE + 1 == HWLOC_OBJ_L4CACHE);
  static_assert(HWLOC_OBJ_L4CACHE + 1 == HWLOC_OBJ_L5CACHE);

  // Rollup the caches to the first cache that serves multiple cores.
  // VMs may not expose cache info. If so, skip this section.
  bool hasNullCache = false;
  for (size_t i = 0; i < topology.cores.size(); ++i) {
    if (topology.cores[i].cache == nullptr) {
      hasNullCache = true;
      break;
    }
  }
  if (!hasNullCache) {
    hwloc_obj_type_t lowestCache = HWLOC_OBJ_TYPE_MAX;
    for (size_t i = 0; i < topology.cores.size(); ++i) {
      auto& core = topology.cores[i];
      if (core.cache->type < lowestCache) {
        lowestCache = core.cache->type;
      }
    }

    // This algorithm isn't robust but should work correctly for real CPUs.
    while (lowestCache <= HWLOC_OBJ_L5CACHE) {
      hwloc_obj_type_t nextLowestCache =
        static_cast<hwloc_obj_type_t>(static_cast<size_t>(lowestCache) + 1);
      hwloc_obj_t sharing = nullptr;
      for (size_t i = 0; i < topology.cores.size() - 1; ++i) {
        auto& curr = topology.cores[i];
        auto& next = topology.cores[i + 1];
        if (curr.cache->type != lowestCache) {
          continue;
        }
        if (next.cache != curr.cache) {
          if (sharing == nullptr) {
            auto parentCache = find_parent_of_type(curr.cache, nextLowestCache);
            if (parentCache != nullptr) {
              curr.cache = parentCache;
            }
          }
          sharing = nullptr;

          // Also handle the last core
          if (i == topology.cores.size() - 2) {
            auto parentCache = find_parent_of_type(next.cache, nextLowestCache);
            if (parentCache != nullptr) {
              next.cache = parentCache;
            }
          }
          continue;
        }

        // curr and next share this cache
        sharing = curr.cache;
      }
      lowestCache = nextLowestCache;
    }
  }

  // Construct the first-level cache ThreadCacheGroups.
  {
    hwloc_obj_t sharing = nullptr;
    size_t kind = TMC_ALL_ONES;
    for (size_t i = 0; i < topology.cores.size(); ++i) {
      auto& core = topology.cores[i];
      if (sharing != core.cache || kind != core.cpu_kind) {
        sharing = core.cache;
        kind = core.cpu_kind;
        topology.groups.push_back({});
        auto& group = topology.groups.back();
        group.index = static_cast<int>(topology.groups.size() - 1);
        group.obj = core.cache;
        group.group_size = 0;
        group.cpu_kind = core.cpu_kind;
        group.children = {};
      }
      auto& group = topology.groups.back();
      group.cores.push_back(core);
      // Just an initial value - can be adjusted later based on
      // set_thread_occupancy() or set_thread_count()
      group.group_size++;
    }
  }

  // Rollup the ThreadCacheGroups into parents to construct the group hierarchy.
  // Skip if there is no cache hierarchy.
  if (!hasNullCache) {
    std::vector<hwloc_obj_t> work;
    hwloc_obj_type_t lowestCache = HWLOC_OBJ_TYPE_MAX;
    for (size_t i = 0; i < topology.groups.size(); ++i) {
      auto& cache = topology.groups[i];
      auto obj = static_cast<hwloc_obj_t>(cache.obj);
      work.push_back(obj);
      if (obj->type < lowestCache) {
        lowestCache = obj->type;
      }
    }

    while (lowestCache <= HWLOC_OBJ_L5CACHE) {
      hwloc_obj_type_t nextLowestCache =
        static_cast<hwloc_obj_type_t>(static_cast<size_t>(lowestCache) + 1);

      hwloc_obj_t sharing = nullptr;
      size_t shareStart = TMC_ALL_ONES;
      for (size_t i = 0; i < topology.groups.size() - 1; ++i) {
        auto& curr = work[i];
        auto& next = work[i + 1];
        if (curr->type != lowestCache) {
          continue;
        }
        auto& currGroup = topology.groups[i];
        auto& nextGroup = topology.groups[i + 1];
        // Always treat different CpuKinds as different groups, even if they
        // share a cache.
        if (curr == next && currGroup.cpu_kind == nextGroup.cpu_kind) {
          if (sharing == nullptr) {
            shareStart = i;
            sharing = curr;
          }
        } else {
          if (sharing != nullptr) {
            assert(shareStart != TMC_ALL_ONES);
            make_cache_parent_group(
              sharing, topology.groups, work, shareStart, i + 1
            );
            // caches has been shrunk, so reset the iterator
            i = shareStart;
            shareStart = TMC_ALL_ONES;
            sharing = nullptr;
          }
        }
      }
      if (sharing != nullptr) {
        assert(shareStart != TMC_ALL_ONES);
        make_cache_parent_group(
          sharing, topology.groups, work, shareStart, topology.groups.size()
        );
      }

      assert(work.size() == topology.groups.size());
      for (size_t i = 0; i < work.size(); ++i) {
        auto& curr = work[i];
        if (curr->type != lowestCache) {
          continue;
        }
        curr = find_parent_cache(curr);
      }
      lowestCache = nextLowestCache;
    }
  }
}

detail::Topology query_internal(
  hwloc_topology_t& HwlocTopo, const char* XmlBuffer, size_t XmlBufferLen
) {
  [[maybe_unused]] int err;
  if (XmlBuffer != nullptr) {
    // Used for tests. Doesn't modify the global topo.
    err = hwloc_topology_init(&HwlocTopo);
    assert(err == 0);

    err = hwloc_topology_set_xmlbuffer(
      HwlocTopo, XmlBuffer, static_cast<int>(XmlBufferLen + 1)
    );
    assert(err == 0);

    err = hwloc_topology_load(HwlocTopo);
    assert(err == 0);

    detail::Topology result;
    query_internal_parse(HwlocTopo, result);
    return result;
  } else {
    std::scoped_lock<std::mutex> lg{tmc::topology::detail::g_topo.lock};
    if (tmc::topology::detail::g_topo.ready) {
      HwlocTopo = tmc::topology::detail::g_topo.hwloc;
      return tmc::topology::detail::g_topo.tmc;
    }

    err = hwloc_topology_init(&HwlocTopo);
    assert(err == 0);

    err = hwloc_topology_load(HwlocTopo);
    assert(err == 0);

    tmc::topology::detail::g_topo.ready = true;
    tmc::topology::detail::g_topo.hwloc = HwlocTopo;

    query_internal_parse(HwlocTopo, tmc::topology::detail::g_topo.tmc);
    return tmc::topology::detail::g_topo.tmc;
  }
}

} // namespace detail

#endif
} // namespace topology
} // namespace tmc
