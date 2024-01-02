#include "tmc/detail/thread_layout.hpp"
#include <vector>

namespace tmc {
namespace detail {

struct SubdivideNode {
  size_t lowIdx;
  size_t highIdx;
  size_t min; // inclusive
  size_t max; // exclusive
};

// Cut the range in half repeatedly.
void recursively_subdivide(std::vector<SubdivideNode>& Results) {
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
void enumerate_paths(
  const size_t Path, uint64_t DepthBit,
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

  uint64_t startPath = 0;
  SubdivideNode node = pathTree[0];
  size_t depth = 0;
  {
    while (node.lowIdx != 0) {
      if (StartGroup < pathTree[node.lowIdx].max) {
        node = pathTree[node.lowIdx];
      } else {
        startPath |= (1ULL << depth);
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
std::vector<L3CacheSet> group_cores_by_l3c(hwloc_topology_t& Topology) {
  // discover the cache groupings
  int l3CacheCount = hwloc_get_nbobjs_by_type(Topology, HWLOC_OBJ_L3CACHE);
  std::vector<L3CacheSet> coresByL3;
  coresByL3.reserve(static_cast<size_t>(l3CacheCount));

  // using DFS, group all cores by shared L3 cache
  hwloc_obj_t curr = hwloc_get_root_obj(Topology);
  // stack of our tree traversal. each level stores the current child index
  std::vector<size_t> childIdx(1);
  while (true) {
    if (curr->type == HWLOC_OBJ_L3CACHE && childIdx.back() == 0) {
      coresByL3.push_back({});
      coresByL3.back().l3cache = curr;
    }
    if (curr->type == HWLOC_OBJ_CORE || childIdx.back() >= curr->arity) {
      if (curr->type == HWLOC_OBJ_CORE) {
        // cores_by_l3c.back().cores.push_back(curr);
        coresByL3.back().group_size++;
      }
      // up a level
      childIdx.pop_back();
      if (childIdx.empty()) {
        break;
      }
      curr = curr->parent;
      // next child
      ++childIdx.back();
    } else {
      // down a level
      curr = curr->children[childIdx.back()];
      childIdx.push_back(0);
    }
  }
  return coresByL3;
}

void bind_thread(hwloc_topology_t Topology, hwloc_cpuset_t SharedCores) {
  if (hwloc_set_cpubind(Topology, SharedCores, HWLOC_CPUBIND_THREAD | HWLOC_CPUBIND_STRICT) == 0) {
  } else if (hwloc_set_cpubind(Topology, SharedCores, HWLOC_CPUBIND_THREAD) == 0) {
  } else {
#ifndef NDEBUG
    auto bitmapSize = hwloc_bitmap_nr_ulongs(SharedCores);
    std::vector<unsigned long> bitmapUlongs;
    bitmapUlongs.resize(bitmapSize);
    hwloc_bitmap_to_ulongs(SharedCores, bitmapSize, bitmapUlongs.data());
    std::vector<uint64_t> bitmaps;
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
        bitmaps.back() |= ((static_cast<uint64_t>(bitmapUlongs[b])) << 32);
        ++b;
      }
    }
    char* bitmapStr;
    hwloc_bitmap_asprintf(&bitmapStr, SharedCores);
    std::printf(
      "FAIL to lasso thread to %s aka %lx %lx\n", bitmapStr, bitmaps[1],
      bitmaps[0]
    );
    free(bitmapStr);
#endif
  }
}
#endif

} // namespace detail
} // namespace tmc