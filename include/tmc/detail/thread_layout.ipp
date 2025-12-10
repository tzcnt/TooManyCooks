// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#include "tmc/detail/compat.hpp"
#include "tmc/detail/thread_layout.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <hwloc.h>
#include <hwloc/bitmap.h>
#include <vector>

void print_cpu_set(hwloc_cpuset_t CpuSet) {
  // If we fail to set the CPU affinity,
  // print an error message in debug build.
  char* bitmapStr;
  hwloc_bitmap_asprintf(&bitmapStr, CpuSet);
  std::printf("bitmap: %s ", bitmapStr);
  std::printf("\n");
  free(bitmapStr);
}

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

void get_pu_indexes_impl(
  tmc::topology::ThreadCoreGroup const& Group, std::vector<size_t>& Indexes_out
) {
  for (size_t i = 0; i < Group.cores.size(); ++i) {
    auto& core = Group.cores[i];
    for (size_t j = 0; j < core.pus.size(); ++j) {
      // We use OS indexes for waking from an external thread
      Indexes_out.push_back(core.pus[j]->os_index);
    }
  }
  for (size_t i = 0; i < Group.children.size(); ++i) {
    get_pu_indexes_impl(Group.children[i], Indexes_out);
  }
}

std::vector<size_t>
get_pu_indexes(tmc::topology::ThreadCoreGroup const& Group) {
  std::vector<size_t> puIndexes;
  get_pu_indexes_impl(Group, puIndexes);
  return puIndexes;
}
} // namespace

// GroupedCores is an input/output parameter.
// Lasso is an output parameter.
std::vector<size_t> adjust_thread_groups(
  size_t RequestedThreadCount, std::vector<float> RequestedOccupancy,
  std::vector<tmc::topology::ThreadCoreGroup*> flatGroups,
  topology::TopologyFilter const& Filter, bool& Lasso
) {

  if (Filter.active()) {
    FilterProcessor coreProc{0, Filter.core_indexes};
    FilterProcessor llcProc{0, Filter.cache_indexes};
    FilterProcessor numaProc{0, Filter.numa_indexes};

    // if disallowed cores/groups, remove them from the working set by setting
    // their group_size to 0
    for (size_t i = 0; i < flatGroups.size(); ++i) {
      auto& group = *flatGroups[i];
      if (0 == (Filter.cpu_kinds & (TMC_ONE_BIT << group.cpu_kind))) {
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
        // TODO move numa to top level of hierarchy as its own node
        if (include && core.numa != nullptr) {
          numaProc.process_next(core.numa->logical_index, include);
        }
        if (!include) {
          group.group_size = 0;
          break;
        }

        include = true;
        coreProc.process_next(core.core->logical_index, include);
        if (!include) {
          --group.group_size;
        }

        if (!include) {
          // We only need to remove from cores if we are editing a single
          // core. Other filters just set the entire group_size to 0.
          group.cores.erase(group.cores.begin() + static_cast<ptrdiff_t>(j));
          --j;
          // hwloc cpuset bitmaps are based on the OS index
          // hwloc_bitmap_and(finalResult, finalResult, core.core->cpuset);
          // hwloc_bitmap_clr(finalResult, static_cast<unsigned
          // int>(pu.pu->os_index));
        }
      }
    }
  }

  if (!RequestedOccupancy.empty()) {
    // if occupancy, set group_size of affected groups by multiplying the
    // original group_size (which will be 1 if allowed or 0 if disallowed)
    for (size_t i = 0; i < flatGroups.size(); ++i) {
      auto& group = *flatGroups[i];
      float occ = RequestedOccupancy[group.cpu_kind];
      if (occ != 1.0f) {
        group.group_size =
          static_cast<size_t>(occ * static_cast<float>(group.group_size));
      }
    }
  }

  // Count the number of threads so far and ensure it is within limits
  size_t totalSize = 0;
  for (size_t i = 0; i < flatGroups.size(); ++i) {
    auto& group = *flatGroups[i];
    totalSize += group.group_size;
  }
  if ((RequestedThreadCount == 0 && totalSize > TMC_PLATFORM_BITS) ||
      RequestedThreadCount > TMC_PLATFORM_BITS) {
    RequestedThreadCount = TMC_PLATFORM_BITS;
  }

  // TODO how to handle a filter / request combo that ends up with 0 threads?

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

      // With .set_thread_count(8) there are 4/2/2 threads in 3 groups
      // it would be preferable for there to be 6/2 threads in 2 groups
      // This should be configurable (FAN or PACK strategy), but the default
      // strategy should be PACK first...
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
    while (totalSize > RequestedThreadCount) {
      // Remove threads from groups by iterating backward, as the last groups
      // are where the E-cores are (if they exist)
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

  // Precalculate a rough mapping of PUs (logical cores) to thread indexes
  // This is only used when all executor threads are sleeping, and an
  // external thread submits work, which wakes the first executor thread.
  // By finding the PU that executor thread is running on, we can then try to
  // wake a nearby executor thread.
  std::vector<size_t> puToThreadMapping;
  size_t maxPuIdx = 0;
  for (size_t i = 0; i < flatGroups.size(); ++i) {
    auto& group = *flatGroups[i];
    auto puIndexes = get_pu_indexes(group);
    for (size_t puIdx : puIndexes) {
      if (puIdx > maxPuIdx) {
        maxPuIdx = puIdx;
      }
    }
  }
  puToThreadMapping.resize(maxPuIdx + 1);
  size_t tid = 0;
  size_t sz = 0;
  size_t gidx = 0;

  // Assign PUs from empty groups to the first non-empty group
  for (; gidx < flatGroups.size(); ++gidx) {
    auto& group = *flatGroups[gidx];
    if (group.group_size != 0) {
      break;
    }
  }
  for (size_t i = 0; i < gidx; ++i) {
    auto& group = *flatGroups[i];
    auto puIndexes = get_pu_indexes(group);
    for (size_t j = 0; j < puIndexes.size(); ++j) {
      puToThreadMapping[puIndexes[j]] = 0;
    }
  }

  // Assign PUs from non-empty groups to the same group.
  // Assign PUs from empty groups to the previous group.
  for (; gidx < flatGroups.size(); ++gidx) {
    auto& group = *flatGroups[gidx];
    if (group.group_size != 0) {
      tid += sz;
      sz = group.group_size;
    }
    auto puIndexes = get_pu_indexes(group);
    for (size_t j = 0; j < puIndexes.size(); ++j) {
      puToThreadMapping[puIndexes[j]] = tid;
    }
  }

  // TODO handle pinning
  // Force lassoing when partitioning is active (even with single group)
  Lasso = true;

  // float threshold = 0.5f;
  // if (GroupedCores.size() >= 4) {
  //   // Machines with a large number of independent L3 caches benefit from
  //   thread
  //   // lassoing at a lower threshold. A slight tweak helps those systems,
  //   even
  //   // at this low occupancy. However, the low occupancy results in threads
  //   // spread across every L3 cache under the current design. A better
  //   solution
  //   // would be to pack the threads together - but we shouldn't choose where
  //   to
  //   // pack them arbitrarily. https://github.com/tzcnt/TooManyCooks/issues/58
  //   // would enable the user to select where the threads are to be packed,
  //   which
  //   // represents the long-term solution for this.

  //   // For now, just slightly lower the threshold on these machines.
  //   threshold = 0.4f;
  // }
  // if (occupancy <= threshold) {
  //   // Turn off thread-lasso capability and make everything one group.
  //   // This needs to be done after the PU to thread mapping, since the
  //   puIndexes
  //   // are also stored on the groups which are deleted by this operation.
  //   GroupedCores.resize(1);
  //   GroupedCores[0].group_size = threadCount;
  // }
  // if (GroupedCores.size() == 1) {
  //   // On devices with only one L3 cache (or no L3 cache), we will only get
  //   // a single group, so there's no need to lasso.
  //   Lasso = false;
  // }
  return puToThreadMapping;
}

void bind_thread(hwloc_topology_t Topology, hwloc_cpuset_t CpuSet) {
  if (0 == hwloc_set_cpubind(
             Topology, CpuSet, HWLOC_CPUBIND_THREAD | HWLOC_CPUBIND_STRICT
           )) {
  } else if (0 == hwloc_set_cpubind(Topology, CpuSet, HWLOC_CPUBIND_THREAD)) {
  } else {
#ifndef NDEBUG
    std::printf("FAIL to lasso thread to ");
    print_cpu_set(CpuSet);
#endif
  }
}

// TODO make this work off of groups instead of cores
// (use Filter section from adjust_thread_groups)
void* make_partition_cpuset(
  void* HwlocTopo, tmc::topology::CpuTopology& TmcTopo,
  tmc::topology::TopologyFilter& Filter
) {
  std::vector<tmc::topology::ThreadCoreGroup*> flatGroups;
  tmc::detail::for_all_groups(
    TmcTopo.caches, [&flatGroups](tmc::topology::ThreadCoreGroup& group) {
      flatGroups.push_back(&group);
    }
  );

  hwloc_cpuset_t work = hwloc_bitmap_alloc();
  hwloc_cpuset_t finalResult = hwloc_bitmap_dup(
    hwloc_topology_get_allowed_cpuset(static_cast<hwloc_topology_t>(HwlocTopo))
  );
  std::printf("all weight: %d\n", hwloc_bitmap_weight(finalResult));
  // hwloc_cpuset_t result = hwloc_bitmap_alloc();

  auto& f = Filter;
  // FilterProcessor puProc{0, f.pu_indexes};
  FilterProcessor coreProc{0, f.core_indexes};
  FilterProcessor llcProc{0, f.cache_indexes};
  FilterProcessor numaProc{0, f.numa_indexes};
  // std::printf("included: ");
  for (size_t i = 0; i < flatGroups.size(); ++i) {
    auto& group = *flatGroups[i];
    bool include = true;
    // Use our (TMC) index for caches, rather than hwloc's logical_index
    // Because our caches may be of different levels
    llcProc.process_next(static_cast<size_t>(group.index), include);
    if (!include) {
      // Exclude the individual cores of the cache and not the cache object's
      // cpuset, as we may have partitioned based on CpuKind, but the cache
      // object would contain the original cpuset with all the cores in it.
      for (size_t j = 0; j < group.cores.size(); ++j) {
        auto& core = group.cores[j];
        // std::printf("excluding\n");
        // print_cpu_set(static_cast<hwloc_obj_t>(core.core)->cpuset);
        hwloc_bitmap_not(work, static_cast<hwloc_obj_t>(core.core)->cpuset);
        // std::printf("NOTted\n");
        // print_cpu_set(work);
        // std::printf("before\n");
        // print_cpu_set(finalResult);
        hwloc_bitmap_and(finalResult, finalResult, work);
        // std::printf("after\n");
        // print_cpu_set(finalResult);
        // std::printf("\n");
      }
    }
  }
  for (size_t i = 0; i < TmcTopo.cores.size(); ++i) {
    auto& core = TmcTopo.cores[i];
    bool include = true;
    if (0 == (Filter.cpu_kinds & (TMC_ONE_BIT << core.cpu_kind))) {
      include = false;
    }
    // if (include && f.pu_logical) {
    //   puProc.process_next(pu.pu->logical_index, include);
    // }
    if (include && core.core != nullptr) {
      coreProc.process_next(core.core->logical_index, include);
    }
    if (include && core.numa != nullptr) {
      numaProc.process_next(core.numa->logical_index, include);
    }

    if (!include) {
      hwloc_bitmap_not(work, core.core->cpuset);
      hwloc_bitmap_and(finalResult, finalResult, work);
      // hwloc cpuset bitmaps are based on the OS index
      // hwloc_bitmap_clr(finalResult, static_cast<unsigned
      // int>(pu.pu->os_index));
    }
    // else {
    //   std::printf("%u ", core.core->logical_index);
    // }
  }
  auto allowedPUCount = hwloc_bitmap_weight(finalResult);
  assert(
    allowedPUCount != 0 && "Partition resulted in 0 allowed processing units."
  );
  // std::printf("\n");
  std::printf("bitmap weight: %d\n", allowedPUCount);
  print_cpu_set(finalResult);

  hwloc_bitmap_free(work);
  return finalResult;
}
#endif

ThreadCoreGroupIterator::ThreadCoreGroupIterator(
  std::vector<tmc::topology::ThreadCoreGroup>& GroupedCores,
  std::function<void(tmc::topology::ThreadCoreGroup&)> Process
)
    : process_{Process} {
  states_.push_back(
    {0, GroupedCores,
     tmc::detail::get_flat_group_iteration_order(GroupedCores.size(), 0)}
  );
}
// Advances the iterator to the next element of the tree and invokes the process
// function on it. Returns false if no elements remain to visit.
bool ThreadCoreGroupIterator::next() {
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
  std::vector<tmc::topology::ThreadCoreGroup>& groups,
  std::function<void(tmc::topology::ThreadCoreGroup&)> process
) {
  ThreadCoreGroupIterator iter(groups, process);
  while (iter.next()) {
  }
}

class WorkStealingMatrixIterator : public ThreadCoreGroupIterator {
public:
  // After calling next(), read this field to get the result.
  std::vector<tmc::topology::ThreadCoreGroup> Output;

  WorkStealingMatrixIterator(
    std::vector<tmc::topology::ThreadCoreGroup>& GroupedCores
  )
      : ThreadCoreGroupIterator(
          GroupedCores,
          [this](tmc::topology::ThreadCoreGroup&) { get_group_order(); }
        ) {}

private:
  void get_group_order() {
    Output.clear();
    auto myState = states_;
    // Rewrite each level of the starting state stack to use our local ordering.
    // Iteration begins from our current index.
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

        // TODO wrap based on total index rather than lowest level index?
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
  std::vector<tmc::topology::ThreadCoreGroup> const& hierarchy
) {
  assert(!hierarchy.empty());
  WorkStealingMatrixIterator iter(
    // This iter doesn't modify, but it's convenient to build on top of general
    // iterator class which might modify.
    const_cast<std::vector<tmc::topology::ThreadCoreGroup>&>(hierarchy)
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
  std::vector<tmc::topology::ThreadCoreGroup> const& hierarchy
) {
  assert(!hierarchy.empty());
  WorkStealingMatrixIterator iter(
    // This iter doesn't modify, but it's convenient to build on top of general
    // iterator class which might modify.
    const_cast<std::vector<tmc::topology::ThreadCoreGroup>&>(hierarchy)
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

// For each thread, find the threads that will search it to steal from soonest.
// These are the threads that should be woken first to steal from this thread.
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
bool CpuTopology::is_sorted() {
  size_t coreIdx = cores[0].core->logical_index;
  size_t llcIdx = 0;
  size_t numaIdx = 0;

  coreCount = 0;
  llcCount = 0;
  numaCount = 0;
  auto set_non_null = [](hwloc_obj_t Obj, size_t& Idx, size_t& Count) {
    if (Obj != nullptr) {
      Idx = Obj->logical_index;
      Count = 1;
    }
  };
  set_non_null(cores[0].core, coreIdx, coreCount);
  set_non_null(cores[0].cache, llcIdx, llcCount);
  set_non_null(cores[0].numa, numaIdx, numaCount);

  size_t kindIdx = cores[0].cpu_kind;

  for (size_t i = 1; i < cores.size(); ++i) {
    auto& core = cores[i];

    auto ok = [](hwloc_obj_t Obj, size_t& Idx, size_t& Count) {
      if (Obj != nullptr) {
        if (Obj->logical_index < Idx) {
          return false;
        }
        if (Obj->logical_index != Idx) {
          Idx = Obj->logical_index;
          ++Count;
        }
      }
      return true;
    };
    if (!ok(core.core, coreIdx, coreCount)) {
      return false;
    }
    if (!ok(core.cache, llcIdx, llcCount)) {
      return false;
    }
    if (!ok(core.numa, numaIdx, numaCount)) {
      return false;
    }

    if (core.cpu_kind < kindIdx) {
      return false;
    }
    kindIdx = core.cpu_kind;
  }
  return true;
}
namespace detail {
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
  hwloc_obj_t parent, std::vector<tmc::topology::ThreadCoreGroup>& caches,
  std::vector<hwloc_obj_t>& work, size_t shareStart, size_t shareEnd
) {
  tmc::topology::ThreadCoreGroup newGroup{};
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

CpuTopology query_internal(hwloc_topology_t& HwlocTopo) {
  std::scoped_lock<std::mutex> lg{tmc::topology::detail::g_topo.lock};
  if (tmc::topology::detail::g_topo.ready) {
    HwlocTopo = tmc::topology::detail::g_topo.hwloc;
    return tmc::topology::detail::g_topo.tmc;
  }

  tmc::topology::detail::g_topo.ready = true;
  tmc::topology::CpuTopology& topology = tmc::topology::detail::g_topo.tmc;

  hwloc_topology_t topo;
  hwloc_topology_init(&topo);
  hwloc_topology_load(topo);
  tmc::topology::detail::g_topo.hwloc = topo;
  HwlocTopo = topo;
  {
// Detect heterogeneous cores (P-cores vs E-cores)
// Requires hwloc 2.1+
#if HWLOC_API_VERSION >= 0x00020100
    std::vector<hwloc_cpuset_t> kindCpuSets;
    size_t cpuKindCount = static_cast<size_t>(hwloc_cpukinds_get_nr(topo, 0));
    topology.cpu_kind_counts.resize(cpuKindCount);
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
#else
    tmc::topology::detail::g_topo.tmc.cpu_kind_counts.resize(1);
#endif

    hwloc_obj_t curr = hwloc_get_root_obj(topo);
    std::vector<size_t> childIdx(1, 0);

    // Traverse the tree to collect info about pus, cores, last-level caches,
    // and numa nodes. This has to be done instead of using
    // hwloc_get_nbobjs_by_type() because hybrid core machines may expose
    // irregular structures.
    while (true) {
      if (childIdx.back() == 0) {
        if (curr->type == HWLOC_OBJ_CORE) {
          topology.cores.emplace_back();
          auto& core = topology.cores.back();
          core.core = curr;

#if HWLOC_API_VERSION >= 0x00020100
          for (size_t i = 0; i < kindCpuSets.size(); ++i) {
            if (hwloc_bitmap_intersects(kindCpuSets[i], curr->cpuset)) {
              core.cpu_kind = i;
              ++topology.cpu_kind_counts[i];
              break;
            }
          }
#else
          core.cpu_kind = 0;
          ++topology.cpu_kind_counts[0];
#endif

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
              if (core.cache == nullptr) {
                core.cache = parent;
              }
              break;
            case HWLOC_OBJ_NUMANODE:
              core.numa = parent;
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
        } else if (curr->type == HWLOC_OBJ_PU) {
          // Assume we can use the "core" as the smallest unit, and that PUs
          // will always be descendants of cores. This doesn't work on IBM
          // PPC64.
          auto& core = topology.cores.back();
          core.pus.push_back(curr);
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

#if HWLOC_API_VERSION >= 0x00020100
    for (auto cpuset : kindCpuSets) {
      hwloc_bitmap_free(cpuset);
    }
#endif
  }

  // TODO the is_sorted call has side effects and must be called...
  [[maybe_unused]] bool ok = topology.is_sorted();
  assert(ok);

  // We are going to loop over these enums. Make sure hwloc hasn't changed
  // them.
  static_assert(HWLOC_OBJ_L1CACHE + 1 == HWLOC_OBJ_L2CACHE);
  static_assert(HWLOC_OBJ_L2CACHE + 1 == HWLOC_OBJ_L3CACHE);
  static_assert(HWLOC_OBJ_L3CACHE + 1 == HWLOC_OBJ_L4CACHE);
  static_assert(HWLOC_OBJ_L4CACHE + 1 == HWLOC_OBJ_L5CACHE);

  // Rollup the caches to the first cache that serves multiple cores.
  {
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

  // Construct the first-level cache ThreadCoreGroups.
  {
    hwloc_obj_t sharing = nullptr;
    for (size_t i = 0; i < topology.cores.size(); ++i) {
      auto& core = topology.cores[i];
      if (sharing != core.cache) {
        sharing = core.cache;
        topology.caches.push_back({});
        auto& c = topology.caches.back();
        c.index = static_cast<int>(topology.caches.size() - 1);
        c.obj = core.cache;
        c.group_size = 0;
        c.cpu_kind = core.cpu_kind;
        c.children = {};
      }
      auto& c = topology.caches.back();
      c.cores.push_back(core);
      // Just an initial value - can be adjusted later based on
      // set_thread_occupancy() or set_thread_count()
      c.group_size++;
    }
  }

  // Rollup the ThreadCoreGroups into parents to construct the group hierarchy.
  {
    std::vector<hwloc_obj_t> work;
    hwloc_obj_type_t lowestCache = HWLOC_OBJ_TYPE_MAX;
    for (size_t i = 0; i < topology.caches.size(); ++i) {
      auto& cache = topology.caches[i];
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
      for (size_t i = 0; i < topology.caches.size() - 1; ++i) {
        auto& curr = work[i];
        auto& next = work[i + 1];
        if (curr->type != lowestCache) {
          continue;
        }
        auto& currGroup = topology.caches[i];
        auto& nextGroup = topology.caches[i + 1];
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
              sharing, topology.caches, work, shareStart, i + 1
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
          sharing, topology.caches, work, shareStart, topology.caches.size()
        );
      }

      assert(work.size() == topology.caches.size());
      for (size_t i = 0; i < work.size(); ++i) {
        auto& curr = work[i];
        if (curr->type != lowestCache) {
          continue;
        }
        curr = find_parent_cache(curr);
      }
      lowestCache = nextLowestCache;

      // hwloc_obj_t sharing = nullptr;
      // for (size_t i = 0; i < topology.caches.size() - 1; ++i) {
      //   auto& curr = topology.caches[i];
      //   auto& next = topology.caches[i + 1];
      //   auto currObj = static_cast<hwloc_obj_t>(curr.obj);
      //   auto nextObj = static_cast<hwloc_obj_t>(next.obj);
      //   if (currObj->type != lowestCache) {
      //     continue;
      //   }
      //   // Rollup this cache to the same level as next cache
      //   while (currObj != nullptr && static_cast<size_t>(currObj->type) <
      //                                  static_cast<size_t>(nextObj->type)) {
      //     currObj = find_parent_cache(currObj);
      //   }

      //   if (nextObj != currObj) {
      //     if (sharing == nullptr) {
      //       auto parentCache = find_parent_of_type(currObj, nextLowestCache);
      //       if (parentCache != nullptr) {
      //         currObj = parentCache;
      //       }
      //     }
      //     sharing = nullptr;

      //     // Also handle the last core
      //     if (i == topology.caches.size() - 2) {
      //       auto parentCache = find_parent_of_type(nextObj, nextLowestCache);
      //       if (parentCache != nullptr) {
      //         nextObj = parentCache;
      //       }
      //     }
      //     continue;
      //   }

      //   // curr and next share this cache
      //   sharing = currObj;
      // }
      // lowestCache = nextLowestCache;
    }
  }

  return topology;
}
} // namespace detail

CpuTopology query() {
  hwloc_topology_t unused;
  return tmc::topology::detail::query_internal(unused);
}

#endif

// TODO: return *this for all of these
// Handle lvalue and rvalue this
void TopologyFilter::set_core_indexes(std::vector<size_t> Indexes) {
  core_indexes = Indexes;
  std::sort(core_indexes.begin(), core_indexes.end());
}
void TopologyFilter::set_cache_indexes(std::vector<size_t> Indexes) {
  cache_indexes = Indexes;
  std::sort(cache_indexes.begin(), cache_indexes.end());
}
void TopologyFilter::set_numa_indexes(std::vector<size_t> Indexes) {
  numa_indexes = Indexes;
  std::sort(numa_indexes.begin(), numa_indexes.end());
}
bool TopologyFilter::active() const {
  return !core_indexes.empty() || !cache_indexes.empty() ||
         !numa_indexes.empty() || cpu_kinds != tmc::topology::CpuKind::ALL;
}
void TopologyFilter::set_cpu_kinds(tmc::topology::CpuKind::value CpuKinds) {
  cpu_kinds = CpuKinds;
}
} // namespace topology

} // namespace tmc
