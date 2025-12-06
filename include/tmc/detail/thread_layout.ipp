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
#include <vector>

void print_cpu_set(hwloc_cpuset_t CpuSet) {
  // If we fail to set the CPU affinity,
  // print an error message in debug build.
  auto bitmapSize = hwloc_bitmap_nr_ulongs(CpuSet);
  std::vector<unsigned long> bitmapUlongs;
  bitmapUlongs.resize(static_cast<size_t>(bitmapSize));
  hwloc_bitmap_to_ulongs(
    CpuSet, static_cast<unsigned int>(bitmapSize), bitmapUlongs.data()
  );
  std::vector<size_t> bitmaps;
  if constexpr (sizeof(unsigned long) == 8) {
    bitmaps.resize(bitmapUlongs.size());
    for (size_t b = 0; b < bitmapUlongs.size(); ++b) {
      bitmaps[b] = bitmapUlongs[b];
    }
  } else { // size is 4
    size_t b = 0;
    while (true) {
      if (b >= bitmapUlongs.size()) {
        break;
      }
      bitmaps.push_back(bitmapUlongs[b]);
      ++b;

      if (b >= bitmapUlongs.size()) {
        break;
      }
      bitmaps.back() |= ((static_cast<size_t>(bitmapUlongs[b])) << 32);
      ++b;
    }
  }
  char* bitmapStr;
  hwloc_bitmap_asprintf(&bitmapStr, CpuSet);
  std::printf("bitmap: %s ", bitmapStr);
  for (size_t i = bitmaps.size() - 1; i != TMC_ALL_ONES; --i) {
    std::printf("%lx ", bitmaps[i]);
  }
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
std::vector<size_t> adjust_thread_groups(
  size_t RequestedThreadCount, float RequestedOccupancy,
  std::vector<tmc::detail::ThreadCoreGroup>& GroupedCores, bool& Lasso
) {
  // GroupedCores is an input/output parameter
  // Lasso is an output parameter
  Lasso = true;
  size_t threadCount = 0;
  size_t coreCount = 0;
  for (size_t i = 0; i < GroupedCores.size(); ++i) {
    coreCount += GroupedCores[i].group_size;
  }
  if (RequestedThreadCount != 0) {
    threadCount = RequestedThreadCount;
  } else if (RequestedOccupancy > .0001f) {
    threadCount =
      static_cast<size_t>(RequestedOccupancy * static_cast<float>(coreCount));
  } else {
    threadCount = coreCount;
  }
  if (threadCount > TMC_PLATFORM_BITS) {
    threadCount = TMC_PLATFORM_BITS;
  }
  if (threadCount == 0) {
    threadCount = 1;
  }
  float occupancy =
    static_cast<float>(threadCount) / static_cast<float>(coreCount);

  if (coreCount > threadCount) {
    // Evenly reduce the size of groups until we hit the desired thread
    // count
    size_t i = GroupedCores.size() - 1;
    while (threadCount < coreCount) {

      // handle irregular processor configurations
      while (GroupedCores[i].group_size == 0) {
        if (i == 0) {
          i = GroupedCores.size() - 1;
        } else {
          --i;
        }
      }

      --GroupedCores[i].group_size;
      --coreCount;
      if (i == 0) {
        i = GroupedCores.size() - 1;
      } else {
        --i;
      }
    }
  } else if (coreCount < threadCount) {
    // Evenly increase the size of groups until we hit the desired thread
    // count
    size_t i = 0;
    while (coreCount < threadCount) {
      ++GroupedCores[i].group_size;
      ++coreCount;
      ++i;
      if (i == GroupedCores.size()) {
        i = 0;
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
  for (size_t i = 0; i < GroupedCores.size(); ++i) {
    for (size_t puIdx : GroupedCores[i].puIndexes) {
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
  for (; gidx < GroupedCores.size(); ++gidx) {
    if (GroupedCores[gidx].group_size != 0) {
      break;
    }
  }
  for (size_t i = 0; i < gidx; ++i) {
    auto& pids = GroupedCores[i].puIndexes;
    for (size_t j = 0; j < pids.size(); ++j) {
      puToThreadMapping[pids[j]] = 0;
    }
  }

  // Assign PUs from non-empty groups to the same group.
  // Assign PUs from empty groups to the previous group.
  for (; gidx < GroupedCores.size(); ++gidx) {
    if (GroupedCores[gidx].group_size != 0) {
      tid += sz;
      sz = GroupedCores[gidx].group_size;
    }
    auto& pids = GroupedCores[gidx].puIndexes;
    for (size_t j = 0; j < pids.size(); ++j) {
      puToThreadMapping[pids[j]] = tid;
    }
  }

  float threshold = 0.5f;
  if (GroupedCores.size() >= 4) {
    // Machines with a large number of independent L3 caches benefit from thread
    // lassoing at a lower threshold. A slight tweak helps those systems, even
    // at this low occupancy. However, the low occupancy results in threads
    // spread across every L3 cache under the current design. A better solution
    // would be to pack the threads together - but we shouldn't choose where to
    // pack them arbitrarily. https://github.com/tzcnt/TooManyCooks/issues/58
    // would enable the user to select where the threads are to be packed, which
    // represents the long-term solution for this.

    // For now, just slightly lower the threshold on these machines.
    threshold = 0.4f;
  }
  if (occupancy <= threshold) {
    // Turn off thread-lasso capability and make everything one group.
    // This needs to be done after the PU to thread mapping, since the puIndexes
    // are also stored on the groups which are deleted by this operation.
    GroupedCores.resize(1);
    GroupedCores[0].group_size = threadCount;
  }
  if (GroupedCores.size() == 1) {
    // On devices with only one L3 cache (or no L3 cache), we will only get
    // a single group, so there's no need to lasso.
    Lasso = false;
  }
  return puToThreadMapping;
}

void bind_thread(hwloc_topology_t Topology, hwloc_cpuset_t CpuSet) {
  if (hwloc_set_cpubind(
        Topology, CpuSet, HWLOC_CPUBIND_THREAD | HWLOC_CPUBIND_STRICT
      ) == 0) {
  } else if (hwloc_set_cpubind(Topology, CpuSet, HWLOC_CPUBIND_THREAD) == 0) {
  } else {
#ifndef NDEBUG
    std::printf("FAIL to lasso thread to ");
    print_cpu_set(CpuSet);
#endif
  }
}

namespace {
struct FilterProcessor {
  size_t offset;
  std::vector<size_t>& indexes;
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

void* make_partition_cpuset(
  void* HwlocTopo, tmc::topology::CpuTopology& TmcTopo,
  tmc::topology::TopologyFilter& Filter
) {
  hwloc_cpuset_t finalResult = hwloc_bitmap_dup(
    hwloc_topology_get_allowed_cpuset(static_cast<hwloc_topology_t>(HwlocTopo))
  );
  std::printf("all weight: %d\n", hwloc_bitmap_weight(finalResult));
  // hwloc_cpuset_t result = hwloc_bitmap_alloc();

  auto& f = Filter;
  // FilterProcessor puProc{0, f.pu_indexes};
  FilterProcessor coreProc{0, f.core_indexes};
  FilterProcessor llcProc{0, f.llc_indexes};
  FilterProcessor numaProc{0, f.numa_indexes};
  std::printf("included: ");
  for (size_t i = 0; i < TmcTopo.cores.size(); ++i) {
    auto& core = TmcTopo.cores[i];
    bool include = true;
    // if (include && f.pu_logical) {
    //   puProc.process_next(pu.pu->logical_index, include);
    // }
    if (include && f.core_logical && core.core != nullptr) {
      coreProc.process_next(core.core->logical_index, include);
    }
    if (include && f.llc_logical && core.llc != nullptr) {
      llcProc.process_next(core.llc->logical_index, include);
    }
    if (include && f.numa_logical && core.numa != nullptr) {
      numaProc.process_next(core.numa->logical_index, include);
    }
    // TODO: handle OS indexes afterward - they can be in any order and would
    // require sorting each time, by each index kind

    if (!include) {
      // hwloc cpuset bitmaps are based on the OS index
      hwloc_bitmap_and(finalResult, finalResult, core.core->cpuset);
      // hwloc_bitmap_clr(finalResult, static_cast<unsigned
      // int>(pu.pu->os_index));
    } else {
      std::printf("%u ", core.core->logical_index);
    }
  }
  std::printf("\n");
  std::printf("bitmap weight: %d\n", hwloc_bitmap_weight(finalResult));
  print_cpu_set(finalResult);

  return finalResult;
}

// TODO instead making groups first, then applying partitions, then placing
// threads
//
// make partition, then iterate over pus and create threads/groups dynamically
void apply_partition_to_groups(
  hwloc_topology_t Topology, hwloc_cpuset_t Partition,
  std::vector<L3CacheSet>& GroupedCores
) {
  // For each L3 group, count how many cores are within the partition
  for (auto& group : GroupedCores) {
    size_t coreCount = 0;

    // Traverse the L3 cache object's descendants to count cores
    hwloc_obj_t l3Obj = static_cast<hwloc_obj_t>(group.l3cache);
    if (l3Obj != nullptr) {
      hwloc_obj_t curr = l3Obj;
      std::vector<size_t> childIdx(1, 0);

      while (true) {
        if (childIdx.back() == 0 && curr->type == HWLOC_OBJ_CORE) {
          // Check if this core intersects with the partition
          if (hwloc_bitmap_intersects(curr->cpuset, Partition)) {
            std::printf(
              "intersected %u weight: %d\n", curr->logical_index,
              hwloc_bitmap_weight(curr->cpuset)
            );
            print_cpu_set(curr->cpuset);
            ++coreCount;
          }
        }

        if (childIdx.back() >= curr->arity) {
          childIdx.pop_back();
          if (childIdx.empty() || curr == l3Obj) {
            break;
          }
          curr = curr->parent;
          ++childIdx.back();
        } else {
          curr = curr->children[childIdx.back()];
          childIdx.push_back(0);
        }
      }
    } else {
      // If there's no L3 cache object, count all cores in the partition
      int coreObjCount = hwloc_get_nbobjs_by_type(Topology, HWLOC_OBJ_CORE);
      for (int i = 0; i < coreObjCount; ++i) {
        hwloc_obj_t core = hwloc_get_obj_by_type(Topology, HWLOC_OBJ_CORE, i);
        if (hwloc_bitmap_intersects(core->cpuset, Partition)) {
          std::printf(
            "core intersected %d weight: %d\n", i,
            hwloc_bitmap_weight(core->cpuset)
          );
          print_cpu_set(core->cpuset);
          ++coreCount;
        }
      }
    }

    group.group_size = coreCount;
  }
}
#endif

// A work-stealing matrix based on purely hierarchical behavior.
// Threads will always steal from the closest available NUCA peer.
std::vector<size_t>
get_hierarchical_matrix(std::vector<L3CacheSet> const& groupedCores) {
  tmc::detail::ThreadSetupData TData;
  TData.total_size = 0;
  TData.groups.resize(groupedCores.size());
  size_t groupStart = 0;
  for (size_t i = 0; i < groupedCores.size(); ++i) {
    size_t groupSize = groupedCores[i].group_size;
    TData.groups[i].size = groupSize;
    TData.groups[i].start = groupStart;
    groupStart += groupSize;
  }
  TData.total_size = groupStart;

  size_t total = TData.total_size * TData.total_size;
  std::vector<size_t> forward;
  forward.reserve(total);

  for (size_t GroupIdx = 0; GroupIdx < groupedCores.size(); ++GroupIdx) {
    auto& coreGroup = groupedCores[GroupIdx];
    size_t groupSize = coreGroup.group_size;
    for (size_t SubIdx = 0; SubIdx < groupSize; ++SubIdx) {
      // Calculate entire iteration order in advance and cache it.
      // The resulting order will be:
      // This thread
      // Other threads in this thread's group
      // threads in each other group, with groups ordered by hierarchy

      auto groupOrder = tmc::detail::get_flat_group_iteration_order(
        TData.groups.size(), GroupIdx
      );
      assert(groupOrder.size() == TData.groups.size());

      for (size_t groupOff = 0; groupOff < groupOrder.size(); ++groupOff) {
        size_t gidx = groupOrder[groupOff];
        auto& group = TData.groups[gidx];
        for (size_t off = 0; off < group.size; ++off) {
          size_t sidx = (SubIdx + off) % group.size;
          size_t val = sidx + group.start;
          forward.push_back(val);
        }
      }
    }
  }
  assert(forward.size() == TData.total_size * TData.total_size);
  return forward;
}

struct tree_group_iterator {
  struct state {
    size_t orderIdx;
    std::vector<tmc::detail::ThreadCoreGroup> const& cores;
    std::vector<size_t> order;
  };
  std::vector<state> states_;

  tree_group_iterator(
    std::vector<tmc::detail::ThreadCoreGroup> const& GroupedCores
  ) {
    states_.push_back(
      {0, GroupedCores,
       tmc::detail::get_flat_group_iteration_order(GroupedCores.size(), 0)}
    );
  }

  void get_group_order(std::vector<tmc::detail::ThreadCoreGroup>& Output) {
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

  bool next_group_order(std::vector<tmc::detail::ThreadCoreGroup>& Output) {
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
           tmc::detail::get_flat_group_iteration_order(
             group.children.size(), 0
           )}
        );
      } else {
        // The groups already have the correct global index
        get_group_order(Output);

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
};

// A more complex work stealing matrix that distributes work more rapidly
// across core groups.
std::vector<size_t>
get_lattice_matrix(std::vector<tmc::detail::ThreadCoreGroup> const& hierarchy) {
  assert(!hierarchy.empty());
  tree_group_iterator iter(hierarchy);
  std::vector<tmc::detail::ThreadCoreGroup> groupedCores;
  iter.next_group_order(groupedCores);
  // groupedCores now contains the flattened hierachy (only leaf nodes)
  // in the order that they will be visited by group 0

  tmc::detail::ThreadSetupData TData;
  TData.total_size = 0;
  TData.groups.resize(groupedCores.size());
  size_t groupStart = 0;
  for (size_t i = 0; i < groupedCores.size(); ++i) {
    assert(groupedCores[i].index == i);
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
    // size_t GroupIdx = groupOrder[0];
    //  TODO recurse into groups with children

    assert(groupedCores.size() == TData.groups.size());

    // Iterate over each core within this shared cache
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
        auto& group = TData.groups[coreGroup.index];
        for (size_t off = 0; off < group.size; ++off) {
          size_t sidx = (SubIdx + off) % group.size;
          size_t val = sidx + group.start;
          forward.push_back(val);
        }
      }

      // TODO only use stolenFromIdx if

      // 1 peer thread from each other group (with same sub_idx as this)
      // groups may have different sizes, so use modulo
      for (size_t groupOff = 1; groupOff < groupedCores.size(); ++groupOff) {
        size_t gidx = groupedCores[groupOff].index;
        auto& group = TData.groups[gidx];
        size_t sidx;
        if (TData.groups[coreGroup.index].size == group.size) {
          sidx = SubIdx % group.size;
        } else {
          sidx = group.stolenFromIdx % group.size;
        }
        size_t val = sidx + group.start;
        forward.push_back(val);
      }

      // Remaining threads from other groups (1 group at a time)
      for (size_t groupOff = 1; groupOff < groupedCores.size(); ++groupOff) {
        size_t gidx = groupedCores[groupOff].index;
        auto& group = TData.groups[gidx];
        for (size_t off = 1; off < group.size; ++off) {
          size_t sidx;
          if (TData.groups[coreGroup.index].size == group.size) {
            sidx = (SubIdx + off) % group.size;
          } else {
            sidx = (group.stolenFromIdx + off) % group.size;
          }
          size_t val = sidx + group.start;
          forward.push_back(val);
        }
        if (TData.groups[coreGroup.index].size != group.size) {
          ++group.stolenFromIdx;
        }
      }
    }
  } while (iter.next_group_order(groupedCores));
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
  set_non_null(cores[0].llc, llcIdx, llcCount);
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
    if (!ok(core.llc, llcIdx, llcCount)) {
      return false;
    }
    if (!ok(core.numa, numaIdx, numaCount)) {
      return false;
    }

    // TODO kind sort order is reversed from regular sort order
    // does this matter?
    // if (pu.cpu_kind < kindIdx) {
    //   return false;
    // }
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
  hwloc_obj_t parent, std::vector<tmc::detail::ThreadCoreGroup>& caches,
  std::vector<hwloc_obj_t>& work, size_t shareStart, size_t shareEnd
) {
  tmc::detail::ThreadCoreGroup newGroup{};
  newGroup.obj = parent;
  newGroup.index = caches[shareStart].index;
  newGroup.cpu_kind = caches[shareStart].cpu_kind;
  newGroup.core_count = 0;
  for (size_t j = shareStart; j < shareEnd; ++j) {
    auto& child = caches[j];
    newGroup.children.push_back(child);
    newGroup.core_count += child.core_count;
    for (auto& pu : child.puIndexes) {
      newGroup.puIndexes.push_back(pu);
    }
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
    int cpuKindCount = hwloc_cpukinds_get_nr(topo, 0);
    if (cpuKindCount > 1) {
      tmc::topology::detail::g_topo.tmc.has_efficiency_cores = true;
      for (unsigned idx = 0; idx < static_cast<unsigned>(cpuKindCount); ++idx) {

        hwloc_bitmap_t cpuset = hwloc_bitmap_alloc();
        int efficiency;
        // Get the cpuset and info for this kind
        // hwloc's "efficiency value" actually means "performance"
        // this sorts kinds by increasing efficiency value (E-cores first)
        // this may be the reverse of the regular sorting
        hwloc_cpukinds_get_info(
          topo, idx, cpuset, &efficiency, nullptr, nullptr, 0
        );

        std::printf("kind %u efficiency %d\n", idx, efficiency);
        kindCpuSets.push_back(cpuset);
      }
    }
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
          if (topology.has_efficiency_cores) {
            for (size_t i = 0; i < kindCpuSets.size(); ++i) {
              if (hwloc_bitmap_intersects(kindCpuSets[i], curr->cpuset)) {
                // TODO partition L3 cache with multiple CPUkinds into separate
                // caches / groups
                core.cpu_kind = i;
              }
            }
          }
#else
          topology.performance_core_count++;
          core.cpu_kind = 0;
#endif

          hwloc_obj_t parent = curr->parent;
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
              if (core.llc == nullptr) {
                core.llc = parent;
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

  assert(topology.is_sorted());

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
      if (core.llc->type < lowestCache) {
        lowestCache = core.llc->type;
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
        if (curr.llc->type != lowestCache) {
          continue;
        }
        if (next.llc != curr.llc) {
          if (sharing == nullptr) {
            auto parentCache = find_parent_of_type(curr.llc, nextLowestCache);
            if (parentCache != nullptr) {
              curr.llc = parentCache;
            }
          }
          sharing = nullptr;

          // Also handle the last core
          if (i == topology.cores.size() - 2) {
            auto parentCache = find_parent_of_type(next.llc, nextLowestCache);
            if (parentCache != nullptr) {
              next.llc = parentCache;
            }
          }
          continue;
        }

        // curr and next share this cache
        sharing = curr.llc;
      }
      lowestCache = nextLowestCache;
    }
  }

  // Construct the first-level cache ThreadCoreGroups.
  {
    hwloc_obj_t sharing = nullptr;
    for (size_t i = 0; i < topology.cores.size(); ++i) {
      auto& core = topology.cores[i];
      if (sharing != core.llc) {
        sharing = core.llc;
        topology.caches.push_back({});
        auto& c = topology.caches.back();
        c.index = topology.caches.size() - 1;
        c.obj = core.llc;
        c.core_count = 0;
        c.cpu_kind = core.cpu_kind;
        c.children = {};
      }
      auto& c = topology.caches.back();
      c.core_count++;
      for (auto& pu : core.pus) {
        // OS index is used for waking from an external thread
        c.puIndexes.push_back(pu->os_index);
      }
      // Just an initial value - can be adjusted later based on
      // set_thread_occupancy() or set_thread_count()
      c.group_size = c.core_count;
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

std::vector<tmc::detail::ThreadCoreGroup>
CpuTopology::make_thread_core_groups(hwloc_cpuset_t Partition) {
  assert(this->is_sorted());
  std::vector<TopologyPU> allowedCores;
  size_t idx = TMC_ALL_ONES;
  for (size_t i = 0; i < this->cores.size(); ++i) {
    auto& pu = cores[i];
    // Only push_back the first PU per core
    if (pu.core->logical_index != idx &&
        hwloc_bitmap_intersects(pu.core->cpuset, Partition)) {
    }
    allowedCores.push_back(pu);
  }

  //  check occupancy
  //  if set, create thread count based on cpuset and occupancy
  //  a cpuset isn't what we want - we want a core set
  //  also allow users to set occupancy for diff core levels

  // check thread count
  // if set, start filling L3s from the bottom - PACK strategy
  // later add FAN strategy

  // otherwise, just fill all of the L3s
  std::vector<tmc::detail::ThreadCoreGroup> groups;
  idx = TMC_ALL_ONES;
  for (size_t i = 0; i < allowedCores.size(); ++i) {
    auto& pu = allowedCores[i];
    if (pu.llc->logical_index != idx) {
      groups.emplace_back(
        pu.llc->cpuset, i, 0, pu.cpu_kind,
        std::vector<tmc::detail::ThreadCoreGroup>{}, 0, std::vector<size_t>{}
      );
    }
    groups.back().group_size++;
  }

  // TODO this assumes the LLC are numbered in sequential order
  // (which they may not be on hybrid chips, unless you create your own logical
  // indexes)
  std::vector<tmc::detail::ThreadCoreGroup> coresByLLC;
  coresByLLC.resize(cores.back().llc->logical_index + 1);

  for (size_t i = 0; i < cores.size(); ++i) {
    auto& core = cores[i];
    size_t llcIdx = core.llc->logical_index;
    coresByLLC[llcIdx].group_size++;
    coresByLLC[llcIdx].obj = core.llc;
    for (auto pu : core.pus) {
      coresByLLC[llcIdx].puIndexes.push_back(pu->logical_index);
    }
  }
  return coresByLLC;
}

std::vector<tmc::detail::L3CacheSet> CpuTopology::group_cores_by_l3c() {
  // TODO this assumes the LLC are numbered in sequential order
  // (which they may not be on hybrid chips, unless you create your own logical
  // indexes)
  std::vector<tmc::detail::L3CacheSet> coresByLLC;
  coresByLLC.resize(cores.back().llc->logical_index + 1);

  for (size_t i = 0; i < cores.size(); ++i) {
    auto& core = cores[i];
    size_t llcIdx = core.llc->logical_index;
    coresByLLC[llcIdx].group_size++;
    coresByLLC[llcIdx].l3cache = core.llc;
    for (auto pu : core.pus) {
      coresByLLC[llcIdx].puIndexes.push_back(pu->logical_index);
    }
  }
  return coresByLLC;
}
#endif

// void TopologyFilter::set_pu_indexes(std::vector<size_t> Indexes, bool
// Logical) {
//   pu_indexes = Indexes;
//   pu_logical = Logical;
//   std::sort(pu_indexes.begin(), pu_indexes.end());
// }
void TopologyFilter::set_core_indexes(
  std::vector<size_t> Indexes, bool Logical
) {
  core_indexes = Indexes;
  core_logical = Logical;
  std::sort(core_indexes.begin(), core_indexes.end());
}
void TopologyFilter::set_llc_indexes(
  std::vector<size_t> Indexes, bool Logical
) {
  llc_indexes = Indexes;
  llc_logical = Logical;
  std::sort(llc_indexes.begin(), llc_indexes.end());
}
void TopologyFilter::set_numa_indexes(
  std::vector<size_t> Indexes, bool Logical
) {
  numa_indexes = Indexes;
  numa_logical = Logical;
  std::sort(numa_indexes.begin(), numa_indexes.end());
}
bool TopologyFilter::active() const {
  return
    // !pu_indexes.empty() ||
    !core_indexes.empty() || !llc_indexes.empty() || !numa_indexes.empty();
}
void TopologyFilter::set_p_e_cores(bool ECore) {
  if (ECore) {
    p_e_core = 2;
  } else {
    p_e_core = 1;
  }
}
} // namespace topology

} // namespace tmc
