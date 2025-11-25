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
// so the best we can do is just recursively subdivide the L3 caches into groups
// of 2. This should also handle CPUs that have odd numbers of L3 caches. On the
// EPYC 7742, this improves performance on Skynet by ~15% (!), while having no
// impact on systems with lesser numbers of caches.
std::vector<size_t>
get_group_iteration_order(size_t GroupCount, size_t StartGroup) {
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
  std::vector<L3CacheSet>& GroupedCores, bool& Lasso
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
  void* HwlocTopo, tmc::topology::CpuTopology TmcTopo,
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
  for (size_t i = 0; i < TmcTopo.pus.size(); ++i) {
    auto& pu = TmcTopo.pus[i];
    bool include = true;
    // if (include && f.pu_logical) {
    //   puProc.process_next(pu.pu->logical_index, include);
    // }
    if (include && f.core_logical && pu.core != nullptr) {
      coreProc.process_next(pu.core->logical_index, include);
    }
    if (include && f.llc_logical && pu.llc != nullptr) {
      llcProc.process_next(pu.llc->logical_index, include);
    }
    if (include && f.numa_logical && pu.numa != nullptr) {
      numaProc.process_next(pu.numa->logical_index, include);
    }
    // TODO: handle OS indexes afterward - they can be in any order and would
    // require sorting each time, by each index kind

    if (!include) {
      // hwloc cpuset bitmaps are based on the OS index
      hwloc_bitmap_clr(finalResult, static_cast<unsigned int>(pu.pu->os_index));
    } else {
      std::printf("%u ", pu.pu->logical_index);
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

      auto groupOrder =
        tmc::detail::get_group_iteration_order(TData.groups.size(), GroupIdx);
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

// A more complex work stealing matrix that distributes work more rapidly
// across core groups.
std::vector<size_t>
get_lattice_matrix(std::vector<L3CacheSet> const& groupedCores) {
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
      // 1 thread from each other group (with same slot_off as this)
      // Remaining threads

      // This thread + other threads in this group
      {
        auto& group = TData.groups[GroupIdx];
        for (size_t off = 0; off < group.size; ++off) {
          size_t sidx = (SubIdx + off) % group.size;
          size_t val = sidx + group.start;
          forward.push_back(val);
        }
      }

      auto groupOrder =
        tmc::detail::get_group_iteration_order(TData.groups.size(), GroupIdx);
      assert(groupOrder.size() == TData.groups.size());

      // 1 peer thread from each other group (with same sub_idx as this)
      // groups may have different sizes, so use modulo
      for (size_t groupOff = 1; groupOff < groupOrder.size(); ++groupOff) {
        size_t gidx = groupOrder[groupOff];
        auto& group = TData.groups[gidx];
        size_t sidx = SubIdx % group.size;
        size_t val = sidx + group.start;
        forward.push_back(val);
      }

      // Remaining threads from other groups (1 group at a time)
      for (size_t groupOff = 1; groupOff < groupOrder.size(); ++groupOff) {
        size_t gidx = groupOrder[groupOff];
        auto& group = TData.groups[gidx];
        for (size_t off = 1; off < group.size; ++off) {
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
namespace {
static bool is_sorted(CpuTopology& topology) {
  size_t puIdx = topology.pus[0].pu->logical_index;
  size_t coreIdx = 0;
  size_t llcIdx = 0;
  size_t numaIdx = 0;

  topology.coreCount = 0;
  topology.llcCount = 0;
  topology.numaCount = 0;
  auto set_non_null = [](hwloc_obj_t Obj, size_t& Idx, size_t& Count) {
    if (Obj != nullptr) {
      Idx = Obj->logical_index;
      Count = 1;
    }
  };
  set_non_null(topology.pus[0].core, coreIdx, topology.coreCount);
  set_non_null(topology.pus[0].llc, llcIdx, topology.llcCount);
  set_non_null(topology.pus[0].numa, numaIdx, topology.numaCount);

  size_t kindIdx = topology.pus[0].cpu_kind;

  for (size_t i = 1; i < topology.pus.size(); ++i) {
    auto& pu = topology.pus[i];
    if (pu.pu->logical_index < puIdx) {
      return false;
    }
    puIdx = pu.pu->logical_index;

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
    if (!ok(pu.core, coreIdx, topology.coreCount)) {
      return false;
    }
    if (!ok(pu.llc, llcIdx, topology.llcCount)) {
      return false;
    }
    if (!ok(pu.numa, numaIdx, topology.numaCount)) {
      return false;
    }

    // TODO kind sort order is reversed from regular sort order
    // does this matter?
    // if (pu.cpu_kind < kindIdx) {
    //   return false;
    // }
    kindIdx = pu.cpu_kind;
  }
  return true;
}
} // namespace
namespace detail {
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
      if (curr->type == HWLOC_OBJ_PU) {
        tmc::topology::detail::g_topo.tmc.pus.emplace_back();
        auto& pu = tmc::topology::detail::g_topo.tmc.pus.back();

        pu.pu = curr;

#if HWLOC_API_VERSION >= 0x00020100
        if (tmc::topology::detail::g_topo.tmc.has_efficiency_cores) {
          for (size_t i = 0; i < kindCpuSets.size(); ++i) {
            if (hwloc_bitmap_intersects(kindCpuSets[i], curr->cpuset)) {
              pu.cpu_kind = i;
            }
          }
        }
#else
        tmc::topology::detail::g_topo.tmc.performance_core_count++;
        pu.is_e_core = false;
#endif

        hwloc_obj_t parent = curr->parent;
        while (parent != nullptr) {
          switch (parent->type) {
          case HWLOC_OBJ_CORE:
            pu.core = parent;
            break;
          case HWLOC_OBJ_L1CACHE:
          case HWLOC_OBJ_L2CACHE:
          case HWLOC_OBJ_L3CACHE:
          case HWLOC_OBJ_L4CACHE:
          case HWLOC_OBJ_L5CACHE:
            // Since we're traversing up from the bottom, the last cache we find
            // will be the last-level cache.
            pu.llc = parent;
            break;
          case HWLOC_OBJ_NUMANODE:
            pu.numa = parent;
            break;
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

  assert(tmc::topology::is_sorted(topology));

  return topology;
}
} // namespace detail

CpuTopology query() {
  hwloc_topology_t unused;
  return tmc::topology::detail::query_internal(unused);
}

// Replacement for group_cores_by_l3c()
std::vector<tmc::detail::L3CacheSet> CpuTopology::group_cores_by_l3c() {
  // TODO this assumes the LLC are numbered in sequential order
  // (which they may not be on hybrid chips, unless you create your own logical
  // indexes)
  std::vector<tmc::detail::L3CacheSet> coresByLLC;
  coresByLLC.resize(pus.back().llc->logical_index + 1);

  for (size_t i = 0; i < pus.size(); ++i) {
    auto& pu = pus[i];
    size_t llcIdx = pu.llc->logical_index;
    coresByLLC[llcIdx].puIndexes.push_back(pu.pu->logical_index);
    coresByLLC[llcIdx].group_size++;
    coresByLLC[llcIdx].l3cache = pu.llc;
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
