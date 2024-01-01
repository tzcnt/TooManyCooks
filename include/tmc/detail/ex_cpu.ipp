// Implementation definition file for tmc::ex_cpu. This will be included
// anywhere TMC_IMPL is defined. If you prefer to manually separate compilation
// units, you can instead include this file directly in a CPP file.
#include "tmc/detail/qu_lockfree.hpp"
#include "tmc/ex_cpu.hpp"

namespace tmc {
void ex_cpu::notify_n(size_t Priority, size_t Count) {
// TODO set notified threads prev_prod (index 1) to this?
#ifdef _MSC_VER
  size_t workingThreadCount = static_cast<size_t>(
    __popcnt64(working_threads_bitset.load(std::memory_order_acquire))
  );
#else
  size_t workingThreadCount = static_cast<size_t>(
    __builtin_popcountll(working_threads_bitset.load(std::memory_order_acquire))
  );
#endif
  size_t sleepingThreadCount = thread_count() - workingThreadCount;
#ifdef TMC_PRIORITY_COUNT
  if constexpr (PRIORITY_COUNT > 1)
#else
  if (PRIORITY_COUNT > 1)
#endif
  {
    // if available threads can take all tasks, no need to interrupt
    if (sleepingThreadCount < Count && workingThreadCount != 0) {
      size_t interruptCount = 0;
      size_t interruptMax =
        std::min(workingThreadCount, Count - sleepingThreadCount);
      for (size_t prio = PRIORITY_COUNT - 1; prio > Priority; --prio) {
        size_t slot;
        uint64_t set =
          task_stopper_bitsets[prio].load(std::memory_order_acquire);
        while (set != 0) {
#ifdef _MSC_VER
          slot = static_cast<size_t>(_tzcnt_u64(set));
#else
          slot = static_cast<size_t>(__builtin_ctzll(set));
#endif
          set = set & ~(1ULL << slot);
          if (thread_states[slot].yield_priority.load(std::memory_order_relaxed) <= Priority) {
            continue;
          }
          auto oldPrio = thread_states[slot].yield_priority.exchange(
            Priority, std::memory_order_acq_rel
          );
          if (oldPrio < Priority) {
            // If the prior priority was higher than this one, put it back. This
            // is a race condition that is expected to occur very infrequently
            // if 2 tasks try to request the same thread to yield at the same
            // time.
            size_t restorePrio;
            do {
              restorePrio = oldPrio;
              oldPrio = thread_states[slot].yield_priority.exchange(
                restorePrio, std::memory_order_acq_rel
              );
            } while (oldPrio < restorePrio);
          }
          if (++interruptCount == interruptMax) {
            goto INTERRUPT_DONE;
          }
        }
      }
    }
  }

INTERRUPT_DONE:
  // As a performance optimization, only modify ready_task_cv when we know there
  // is at least 1 sleeping thread. This requires some extra care to prevent a
  // race with threads going to sleep.

  // TODO on Linux this tends to wake threads in a round-robin fashion.
  //   Prefer to wake more recently used threads instead (see folly::LifoSem)
  //   or wake neighbor and peer threads first
  if (sleepingThreadCount > 0) {
    ready_task_cv.fetch_add(1, std::memory_order_acq_rel);
    if (Count > 1) {
      ready_task_cv.notify_all();
    } else {
      ready_task_cv.notify_one();
    }
  }
  // if (count >= availableThreads) {
  // ready_task_cv.notify_all();
  //} else {
  //  for (size_t i = 0; i < count; ++i) {
  //    ready_task_cv.notify_one();
  //  }
  //}
}

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

#ifndef TMC_USE_MUTEXQ
void ex_cpu::init_queue_iteration_order(
  ThreadSetupData const& TData, size_t GroupIdx, size_t SubIdx, size_t Slot
) {
  std::vector<size_t> iterationOrder;
  iterationOrder.reserve(TData.total_size);
  // Calculate entire iteration order in advance and cache it.
  // The resulting order will be:
  // This thread
  // The previously consumed-from thread (dynamically updated)
  // Other threads in this thread's group
  // 1 thread from each other group (with same slot_off as this)
  // Remaining threads

  // This thread + other threads in this group
  {
    auto& group = TData.groups[GroupIdx];
    for (size_t off = 0; off < group.size; ++off) {
      size_t sidx = (SubIdx + off) % group.size;
      iterationOrder.push_back(sidx + group.start);
    }
  }

  auto groupOrder = get_group_iteration_order(TData.groups.size(), GroupIdx);
  assert(groupOrder.size() == TData.groups.size());

  // 1 peer thread from each other group (with same sub_idx as this)
  // groups may have different sizes, so use modulo
  for (size_t groupOff = 1; groupOff < groupOrder.size(); ++groupOff) {
    size_t gidx = groupOrder[groupOff];
    auto& group = TData.groups[gidx];
    size_t sidx = SubIdx % group.size;
    iterationOrder.push_back(sidx + group.start);
  }

  // Remaining threads from other groups (1 group at a time)
  for (size_t groupOff = 1; groupOff < groupOrder.size(); ++groupOff) {
    size_t gidx = groupOrder[groupOff];
    auto& group = TData.groups[gidx];
    for (size_t off = 1; off < group.size; ++off) {
      size_t sidx = (SubIdx + off) % group.size;
      iterationOrder.push_back(sidx + group.start);
    }
  }
  assert(iterationOrder.size() == TData.total_size);

  size_t dequeueCount = TData.total_size + 1;
  queue::details::ConcurrentQueueProducerTypelessBase** producers =
    new queue::details::
      ConcurrentQueueProducerTypelessBase*[PRIORITY_COUNT * dequeueCount];
  for (size_t prio = 0; prio < PRIORITY_COUNT; ++prio) {
    assert(Slot == iterationOrder[0]);
    size_t pidx = prio * dequeueCount;
    // pointer to this thread's producer
    producers[pidx] = &work_queues[prio].staticProducers[Slot];
    ++pidx;
    // pointer to previously consumed-from producer (initially also this
    // thread's producer)
    producers[pidx] = &work_queues[prio].staticProducers[Slot];
    ++pidx;

    for (size_t i = 1; i < TData.total_size; ++i) {
      task_queue_t::ExplicitProducer* prod =
        &work_queues[prio].staticProducers[iterationOrder[i]];
      producers[pidx] = prod;
      ++pidx;
    }
  }
  detail::this_thread::producers = producers;
}
#endif

void ex_cpu::init_thread_locals(size_t Slot) {
  detail::this_thread::executor = &type_erased_this;
  detail::this_thread::this_task = {
    .prio = 0, .yield_priority = &thread_states[Slot].yield_priority
  };
  detail::this_thread::thread_name =
    std::string("cpu thread ") + std::to_string(Slot);
}

void ex_cpu::clear_thread_locals() {
  detail::this_thread::executor = nullptr;
  detail::this_thread::this_task = {};
  detail::this_thread::thread_name.clear();
}

// returns true if no tasks were found (caller should wait on cv)
// returns false if thread stop requested (caller should exit)
bool ex_cpu::try_run_some(
  std::stop_token& ThreadStopToken, const size_t Slot, const size_t MinPriority,
  size_t& PrevPriority
) {
  while (true) {
  TOP:
    if (ThreadStopToken.stop_requested()) [[unlikely]] {
      return false;
    }
    size_t prio = 0;
    for (; prio <= MinPriority; ++prio) {
      work_item item;
      if (!work_queues[prio].try_dequeue_ex_cpu(item, prio)) {
        continue;
      }
#ifdef TMC_PRIORITY_COUNT
      if constexpr (PRIORITY_COUNT > 1)
#else
      if (PRIORITY_COUNT > 1)
#endif
      {
        if (prio != PrevPriority) {
          // TODO RACE if a higher prio asked us to yield, but then
          // got taken by another thread, and we resumed back on our previous
          // prio, yield_priority will not be reset
          detail::this_thread::this_task.yield_priority->store(
            prio, std::memory_order_release
          );
          if (PrevPriority != NO_TASK_RUNNING) {
            task_stopper_bitsets[PrevPriority].fetch_and(
              ~(1ULL << Slot), std::memory_order_acq_rel
            );
          }
          task_stopper_bitsets[prio].fetch_or(
            1ULL << Slot, std::memory_order_acq_rel
          );
          detail::this_thread::this_task.prio = prio;
          PrevPriority = prio;
        }
      }
      item();
      goto TOP;
    }
    return true;
  }
}

void ex_cpu::post(work_item&& Item, size_t Priority) {
  work_queues[Priority].enqueue_ex_cpu(std::move(Item), Priority);
  notify_n(Priority, 1);
}

detail::type_erased_executor* ex_cpu::type_erased() {
  return &type_erased_this;
}

// Default constructor does not call init() - you need to do it afterward
ex_cpu::ex_cpu()
    : type_erased_this(*this), thread_stoppers{}, thread_states{nullptr},
      task_stopper_bitsets{nullptr}, ready_task_cv{}, init_params{nullptr}
#ifndef TMC_PRIORITY_COUNT
      ,
      PRIORITY_COUNT{1}
#endif
{
}

#ifdef TMC_USE_HWLOC
void ex_cpu::bind_thread(
  hwloc_topology_t Topology, hwloc_cpuset_t SharedCores
) {
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

std::vector<ex_cpu::L3CacheSet>
ex_cpu::group_cores_by_l3c(hwloc_topology_t& Topology) {
  // discover the cache groupings
  int l3CacheCount = hwloc_get_nbobjs_by_type(Topology, HWLOC_OBJ_L3CACHE);
  std::vector<ex_cpu::L3CacheSet> coresByL3;
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
#endif

void ex_cpu::init() {
  if (is_initialized) {
    return;
  }
  is_initialized = true;
#ifndef TMC_PRIORITY_COUNT
  if (init_params != nullptr && init_params->priority_count != 0) {
    PRIORITY_COUNT = init_params->priority_count;
  }
  NO_TASK_RUNNING = PRIORITY_COUNT;
#endif
  task_stopper_bitsets = new std::atomic<uint64_t>[PRIORITY_COUNT];
  work_queues.resize(PRIORITY_COUNT);
  for (size_t i = 0; i < PRIORITY_COUNT; ++i) {
#ifndef TMC_USE_MUTEXQ
    work_queues.emplace_at(i, 32000);
#else
    work_queues.emplace_at(i);
#endif
  }
#ifndef TMC_USE_HWLOC
  if (init_params != nullptr && init_params->thread_count != 0) {
    threads.resize(init_params->thread_count);
  } else {
    // limited to 64 threads for now, due to use of uint64_t bitset
    size_t hwconc = std::thread::hardware_concurrency();
    if (hwconc > 64) {
      hwconc = 64;
    }
    threads.resize(hwconc);
  }
#else
  hwloc_topology_t topology;
  hwloc_topology_init(&topology);
  hwloc_topology_load(topology);
  auto groupedCores = group_cores_by_l3c(topology);
  bool lasso = true;
  size_t totalThreadCount = 0;
  size_t coreCount = 0;
  for (size_t i = 0; i < groupedCores.size(); ++i) {
    coreCount += groupedCores[i].group_size;
  }
  if (init_params == nullptr || (
    init_params->thread_count == 0
    && init_params->thread_occupancy >= -0.0001f
    && init_params->thread_occupancy <= 0.0001f
  )) {
    totalThreadCount = coreCount;
  } else {
    if (init_params->thread_count != 0) {
      float occupancy = static_cast<float>(init_params->thread_count) /
                        static_cast<float>(coreCount);
      totalThreadCount = init_params->thread_count;
      if (occupancy <= 0.5f) {
        // turn off thread-lasso capability and make everything one group
        groupedCores.resize(1);
        groupedCores[0].group_size = init_params->thread_count;
        lasso = false;
      } else if (coreCount > init_params->thread_count) {
        // Evenly reduce the size of groups until we hit the desired thread
        // count
        size_t i = groupedCores.size() - 1;
        while (coreCount > init_params->thread_count) {
          --groupedCores[i].group_size;
          --coreCount;
          if (i == 0) {
            i = groupedCores.size() - 1;
          } else {
            --i;
          }
        }
      } else if (coreCount < init_params->thread_count) {
        // Evenly increase the size of groups until we hit the desired thread
        // count
        size_t i = 0;
        while (coreCount < init_params->thread_count) {
          ++groupedCores[i].group_size;
          ++coreCount;
          ++i;
          if (i == groupedCores.size()) {
            i = 0;
          }
        }
      }
    } else { // init_params->thread_occupancy != 0
      for (size_t i = 0; i < groupedCores.size(); ++i) {
        size_t groupSize = static_cast<size_t>(
          static_cast<float>(groupedCores[i].group_size) *
          init_params->thread_occupancy
        );
        groupedCores[i].group_size = groupSize;
        totalThreadCount += groupSize;
      }
    }
  }
  threads.resize(totalThreadCount);
#endif
  assert(thread_count() != 0);
  // limited to 64 threads for now, due to use of uint64_t bitset
  assert(thread_count() <= 64);
  thread_states = new ThreadState[thread_count()];
  for (size_t i = 0; i < thread_count(); ++i) {
    thread_states[i].yield_priority = NO_TASK_RUNNING;
  }

  thread_stoppers.resize(thread_count());
  // All threads start in the "working" state
  working_threads_bitset.store(
    ((1ULL << (thread_count() - 1)) - 1) + (1ULL << (thread_count() - 1))
  );

#ifndef TMC_USE_MUTEXQ
  for (size_t prio = 0; prio < PRIORITY_COUNT; ++prio) {
    work_queues[prio].staticProducers =
      new task_queue_t::ExplicitProducer[thread_count()];
    for (size_t i = 0; i < thread_count(); ++i) {
      work_queues[prio].staticProducers[i].init(&work_queues[prio]);
    }
    work_queues[prio].dequeueProducerCount = thread_count() + 1;
  }
#endif
  std::atomic<int> initThreadsBarrier(static_cast<int>(thread_count()));
  std::atomic_thread_fence(std::memory_order_seq_cst);
  size_t slot = 0;
  size_t groupStart = 0;
#ifdef TMC_USE_HWLOC
  // copy elements of groupedCores into thread lambda capture
  // that will go out of scope at the end of this function
  ThreadSetupData tdata;
  tdata.total_size = thread_count();
  tdata.groups.resize(groupedCores.size());
  for (size_t i = 0; i < groupedCores.size(); ++i) {
    size_t groupSize = groupedCores[i].group_size;
    tdata.groups[i].size = groupSize;
    tdata.groups[i].start = groupStart;
    groupStart += groupSize;
  }
  for (size_t groupIdx = 0; groupIdx < groupedCores.size(); ++groupIdx) {
    auto& coreGroup = groupedCores[groupIdx];
    size_t groupSize = coreGroup.group_size;
    for (size_t subIdx = 0; subIdx < groupSize; ++subIdx) {
      auto sharedCores = hwloc_bitmap_dup(coreGroup.l3cache->cpuset);
#else
  // without HWLOC, treat everything as a single group
  ThreadSetupData tdata;
  tdata.total_size = thread_count();
  tdata.groups.push_back({0, thread_count()});
  size_t groupIdx = 0;
  while (slot < thread_count()) {
    size_t subIdx = slot;
#endif
      // TODO pull this out into a separate struct
      threads.emplace_at(
        slot,
        [
#ifdef TMC_USE_HWLOC
          topology, sharedCores, lasso,
#endif
          this, tdata, groupIdx, subIdx, slot,
          barrier = &initThreadsBarrier](std::stop_token thread_stop_token) {
          init_thread_locals(slot);
#ifdef TMC_USE_HWLOC
          if (lasso) {
            bind_thread(topology, sharedCores);
          }
          hwloc_bitmap_free(sharedCores);
#endif
#ifndef TMC_USE_MUTEXQ
          init_queue_iteration_order(tdata, groupIdx, subIdx, slot);
#endif
          barrier->fetch_sub(1);
          barrier->notify_all();
          size_t previousPrio = NO_TASK_RUNNING;
        TOP:
          auto cvValue = ready_task_cv.load(std::memory_order_acquire);
          while (try_run_some(
            thread_stop_token, slot, PRIORITY_COUNT - 1, previousPrio
          )) {
            auto newCvValue = ready_task_cv.load(std::memory_order_acquire);
            if (newCvValue != cvValue) {
              // more tasks have been posted, try again
              cvValue = newCvValue;
              continue;
            }
            // Because of dequeueOvercommit, when multiple threads try to
            // dequeue at once, they may all see the queue as empty
            // incorrectly. Empty() is more accurate
            for (size_t prio = 0; prio < PRIORITY_COUNT; ++prio) {
              if (!work_queues[prio].empty()) {
                goto TOP;
              }
            }

            // no waiting or in progress work found. wait until a task is
            // ready
            working_threads_bitset.fetch_and(~(1ULL << slot));

#ifdef TMC_PRIORITY_COUNT
            if constexpr (PRIORITY_COUNT > 1)
#else
          if (PRIORITY_COUNT > 1)
#endif
            {
              if (previousPrio != NO_TASK_RUNNING) {
                task_stopper_bitsets[previousPrio].fetch_and(
                  ~(1ULL << slot), std::memory_order_acq_rel
                );
              }
            }
            previousPrio = NO_TASK_RUNNING;

            // To mitigate a race condition with the calculation of
            // available threads in notify_n, we need to check the queue again
            // before going to sleep
            for (size_t prio = 0; prio < PRIORITY_COUNT; ++prio) {
              if (!work_queues[prio].empty()) {
                working_threads_bitset.fetch_or(1ULL << slot);
                goto TOP;
              }
            }
            ready_task_cv.wait(cvValue);
            working_threads_bitset.fetch_or(1ULL << slot);
            cvValue = ready_task_cv.load(std::memory_order_acquire);
          }

          // Thread stop has been requested (executor is shutting down)
          working_threads_bitset.fetch_and(~(1ULL << slot));
          clear_thread_locals();
#ifndef TMC_USE_MUTEXQ
          delete[] static_cast<
            queue::details::ConcurrentQueueProducerTypelessBase**>(
            detail::this_thread::producers
          );
          detail::this_thread::producers = nullptr;
#endif
        }
      );
      thread_stoppers.emplace_at(slot, threads[slot].get_stop_source());
#ifdef TMC_USE_HWLOC
      ++slot;
    }
  }
  auto barrierVal = initThreadsBarrier.load();
  while (barrierVal != 0) {
    initThreadsBarrier.wait(barrierVal);
    barrierVal = initThreadsBarrier.load();
  }
  hwloc_topology_destroy(topology);
#else
    ++slot;
  }
  auto barrierVal = initThreadsBarrier.load();
  while (barrierVal != 0) {
    initThreadsBarrier.wait(barrierVal);
    barrierVal = initThreadsBarrier.load();
  }
#endif
  if (init_params != nullptr) {
    delete init_params;
    init_params = nullptr;
  }
}

#ifndef TMC_PRIORITY_COUNT
ex_cpu& ex_cpu::set_priority_count(size_t PriorityCount) {
  assert(!is_initialized);
  if (init_params == nullptr) {
    init_params = new InitParams;
  }
  init_params->priority_count = PriorityCount;
  return *this;
}
size_t ex_cpu::priority_count() {
  assert(is_initialized);
  return PRIORITY_COUNT;
}
#endif
#ifdef TMC_USE_HWLOC
ex_cpu& ex_cpu::set_thread_occupancy(float ThreadOccupancy) {
  assert(!is_initialized);
  if (init_params == nullptr) {
    init_params = new InitParams;
  }
  init_params->thread_occupancy = ThreadOccupancy;
  return *this;
}
#endif

ex_cpu& ex_cpu::set_thread_count(size_t ThreadCount) {
  assert(!is_initialized);
  if (init_params == nullptr) {
    init_params = new InitParams;
  }
  init_params->thread_count = ThreadCount;
  return *this;
}
size_t ex_cpu::thread_count() {
  assert(is_initialized);
  return threads.size();
}

void ex_cpu::teardown() {
  if (!is_initialized) {
    return;
  }
  is_initialized = false;

  for (size_t i = 0; i < threads.size(); ++i) {
    thread_stoppers[i].request_stop();
  }
  ready_task_cv.fetch_add(1, std::memory_order_release);
  ready_task_cv.notify_all();
  for (size_t i = 0; i < threads.size(); ++i) {
    threads[i].join();
  }
  threads.clear();
  thread_stoppers.clear();

#ifndef TMC_USE_MUTEXQ
  for (size_t i = 0; i < work_queues.size(); ++i) {
    delete[] work_queues[i].staticProducers;
  }
#endif
  work_queues.clear();
  if (task_stopper_bitsets != nullptr) {
    delete[] task_stopper_bitsets;
  }
  if (thread_states != nullptr) {
    delete[] thread_states;
  }
}

ex_cpu::~ex_cpu() { teardown(); }

std::coroutine_handle<>
ex_cpu::task_enter_context(std::coroutine_handle<> Outer, size_t Priority) {
  if (detail::this_thread::executor == &type_erased_this) {
    return Outer;
  } else {
    post(std::move(Outer), Priority);
    return std::noop_coroutine();
  }
}

namespace detail {
tmc::task<void> client_main_awaiter(
  tmc::task<int> ClientMainTask, std::atomic<int>* ExitCode_out
) {
  ClientMainTask.resume_on(tmc::cpu_executor());
  int exitCode = co_await ClientMainTask;
  ExitCode_out->store(exitCode);
  ExitCode_out->notify_all();
}
} // namespace detail
int async_main(tmc::task<int> ClientMainTask) {
  // if the user already called init(), this will do nothing
  tmc::cpu_executor().init();
  std::atomic<int> exitCode(std::numeric_limits<int>::min());
  post(
    tmc::cpu_executor(), detail::client_main_awaiter(ClientMainTask, &exitCode),
    0
  );
  exitCode.wait(std::numeric_limits<int>::min());
  return exitCode.load();
}
} // namespace tmc
