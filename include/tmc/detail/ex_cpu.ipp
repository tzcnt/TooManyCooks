// Implementation definition file for ex_cpu. This will be included anywhere
// TMC_IMPL is defined.
// If you prefer to manually separate compilation units, you can instead include
// this file directly in a CPP file.
#include "tmc/ex_cpu.hpp"
namespace tmc {
void ex_cpu::notify_n(size_t priority, size_t count) {
  // TODO set notified threads prev_prod (index 1) to this
  // TODO wake neighbor and peer threads first
  // a lower number is a higher priority
  size_t working_thread_count = __builtin_popcountll(
      working_threads_bitset.load(std::memory_order_acquire));
  size_t available_threads = thread_count() - working_thread_count;
#ifdef TMC_PRIORITY_COUNT
  if constexpr (PRIORITY_COUNT > 1)
#else
  if (PRIORITY_COUNT > 1)
#endif
  {
    // if available threads can take all tasks, no need to interrupt
    if (available_threads < count && working_thread_count != 0) {
      size_t interrupt_count = 0;
      size_t max_interrupts =
          std::min(working_thread_count, count - available_threads);
      for (size_t prio = PRIORITY_COUNT - 1; prio > priority; --prio) {
        size_t slot;
        uint64_t set =
            task_stopper_bitsets[prio].load(std::memory_order_acquire);
        while (set != 0) {
          slot = __builtin_ctzll(set);
          set = set & ~(1ULL << slot);
          if (thread_states[slot].yield_priority.load(
                  std::memory_order_relaxed) <= priority) {
            continue;
          }
          auto old_prio = thread_states[slot].yield_priority.exchange(
              priority, std::memory_order_acq_rel);
          if (old_prio < priority) {
            // If the prior priority was higher than this one, put it back. This
            // is a race condition that is expected to occur very infrequently.
            thread_states[slot].yield_priority.exchange(
                old_prio, std::memory_order_acq_rel);
          }
          if (++interrupt_count == max_interrupts) {
            goto INTERRUPT_DONE;
          }
        }
      }
    }
  }

INTERRUPT_DONE:
  // TODO confirm this is 100% safe and we never fail to wake
  // If there is only 1 task in-flight this could hang the program
  // TODO on Linux this tends to wake threads in a round-robin fashion.
  // Prefer to wake more recently used threads instead
  if (available_threads > 0) {
    ready_task_cv.fetch_add(1, std::memory_order_acq_rel);
    if (count > 1) {
      ready_task_cv.notify_all();
    } else {
      ready_task_cv.notify_one();
    }
  }
  // if (count >= available_threads) {
  // ready_task_cv.notify_all();
  //} else {
  //  for (size_t i = 0; i < count; ++i) {
  //    ready_task_cv.notify_one();
  //  }
  //}
}

#ifndef TMC_USE_MUTEXQ
void ex_cpu::init_queue_iteration_order(thread_setup_data const &tdata,
                                        size_t group_idx, size_t sub_idx,
                                        size_t slot) {
  std::vector<size_t> iteration_order;
  iteration_order.reserve(tdata.total_size);
  // Calculate entire iteration order in advance and cache it.
  // The resulting order will be:
  // This thread
  // The previously consumed-from thread (dynamically updated)
  // Other threads in this thread's group
  // 1 thread from each other group (with same slot_off as this)
  // Remaining threads

  // This thread + other threads in this group
  auto &group = tdata.groups[group_idx];
  for (size_t off = 0; off < group.size; ++off) {
    size_t sidx = (sub_idx + off) % group.size;
    iteration_order.push_back(sidx + group.start);
  }

  // 1 peer thread from each other group (with same sub_idx as this)
  // groups may have different sizes, so use modulo
  for (size_t group_off = 1; group_off < tdata.groups.size(); ++group_off) {
    size_t gidx = (group_idx + group_off) % tdata.groups.size();
    auto &group = tdata.groups[gidx];
    size_t sidx = sub_idx % group.size;
    iteration_order.push_back(sidx + group.start);
  }

  // Remaining threads from other groups (1 group at a time)
  for (size_t group_off = 1; group_off < tdata.groups.size(); ++group_off) {
    size_t gidx = (group_idx + group_off) % tdata.groups.size();
    auto &group = tdata.groups[gidx];
    for (size_t off = 1; off < group.size; ++off) {
      size_t sidx = (sub_idx + off) % group.size;
      iteration_order.push_back(sidx + group.start);
    }
  }
  assert(iteration_order.size() == tdata.total_size);

  size_t dequeue_count = tdata.total_size + 1;
  task_queue_t::this_thread_producers =
      new tmc::queue::details::ConcurrentQueueProducerTypelessBase
          *[PRIORITY_COUNT * dequeue_count];
  for (size_t prio = 0; prio < PRIORITY_COUNT; ++prio) {
    assert(slot == iteration_order[0]);
    size_t pidx = prio * dequeue_count;
    // pointer to this thread's producer
    task_queue_t::this_thread_producers[pidx] =
        &work_queues[prio].staticProducers[slot];
    ++pidx;
    // pointer to previously consumed-from producer (initially also this
    // thread's producer)
    task_queue_t::this_thread_producers[pidx] =
        &work_queues[prio].staticProducers[slot];
    ++pidx;

    for (size_t i = 1; i < tdata.total_size; ++i) {
      task_queue_t::ExplicitProducer *prod =
          &work_queues[prio].staticProducers[iteration_order[i]];
      task_queue_t::this_thread_producers[pidx] = prod;
      ++pidx;
    }
  }
}
#endif

void ex_cpu::init_thread_locals(size_t slot) {
  detail::this_thread::executor = &type_erased_this;
  detail::this_thread::this_task = {
      .prio = 0, .yield_priority = &thread_states[slot].yield_priority};
  detail::this_thread::thread_name =
      std::string("cpu thread ") + std::to_string(slot);
}

void ex_cpu::clear_thread_locals() {
  detail::this_thread::executor = nullptr;
  detail::this_thread::this_task = {};
  detail::this_thread::thread_name.clear();
}

// returns true if no tasks were found (caller should wait on cv)
// returns false if thread stop requested (caller should exit)
bool ex_cpu::try_run_some(std::stop_token &thread_stop_token, const size_t slot,
                          const size_t minPriority, size_t &previousPrio) {
  while (true) {
  TOP:
    if (thread_stop_token.stop_requested()) [[unlikely]] {
      return false;
    }
    size_t prio = 0;
    bool task_found = false;
    for (; prio <= minPriority; ++prio) {
      work_item item;
      // try to dequeue from this thread's queue first, then check other threads
      if (work_queues[prio].try_dequeue_ex_cpu(item, prio)) {
#ifdef TMC_PRIORITY_COUNT
        if constexpr (PRIORITY_COUNT > 1)
#else
        if (PRIORITY_COUNT > 1)
#endif
        {
          if (prio != previousPrio) {
            thread_states[slot].yield_priority.store(prio,
                                                     std::memory_order_release);
            if (previousPrio != NO_TASK_RUNNING) {
              task_stopper_bitsets[previousPrio].fetch_and(
                  ~(1ULL << slot), std::memory_order_acq_rel);
            }
            task_stopper_bitsets[prio].fetch_or(1ULL << slot,
                                                std::memory_order_acq_rel);
            detail::this_thread::this_task.prio = prio;
            previousPrio = prio;
          }
        }
        item();
        goto TOP;
      }
    }
    return true;
  }
}

void ex_cpu::post_variant(work_item &&item, size_t priority) {
  work_queues[priority].enqueue_ex_cpu(std::move(item), priority);
  notify_n(priority, 1);
}

void ex_cpu::graceful_stop() {
  for (size_t i = 0; i < threads.size(); ++i) {
    thread_stoppers[i].request_stop();
  }
  ready_task_cv.fetch_add(1, std::memory_order_release);
  ready_task_cv.notify_all();
  for (size_t i = 0; i < threads.size(); ++i) {
    threads[i].join();
  }
  threads.clear();

  // drop this executor's tasks before returning
  // for (auto &queue : work_queues) {
  //   // these are just std::function, can drop them
  //   queue.clear();
  // }

  // stop accepting new tasks
  // block until queues empty
}

void ex_cpu::hard_stop() {
  graceful_stop();
  // TODO implement
  // request stop on running_task
  // after it's done, kill the rest
}

detail::type_erased_executor *ex_cpu::type_erased() {
  return &type_erased_this;
}

// Default constructor does not call init() - you need to do it afterward
ex_cpu::ex_cpu()
    : type_erased_this(*this), ready_task_cv{}, thread_stoppers{},
      task_stopper_bitsets{nullptr}, thread_states{nullptr},
      init_params{nullptr}
#ifndef TMC_PRIORITY_COUNT
      ,
      PRIORITY_COUNT{1}
#endif
{
}

#ifdef TMC_USE_HWLOC
void ex_cpu::bind_thread(hwloc_topology_t topology,
                         hwloc_cpuset_t shared_cores) {
  if (hwloc_set_cpubind(topology, shared_cores,
                        HWLOC_CPUBIND_THREAD | HWLOC_CPUBIND_STRICT) == 0) {
  } else if (hwloc_set_cpubind(topology, shared_cores, HWLOC_CPUBIND_THREAD) ==
             0) {
  } else {
#ifndef NDEBUG
    auto bitmapSize = hwloc_bitmap_nr_ulongs(shared_cores);
    std::vector<unsigned long> bitmap_ulongs;
    bitmap_ulongs.resize(bitmapSize);
    hwloc_bitmap_to_ulongs(shared_cores, bitmapSize, bitmap_ulongs.data());
    std::vector<uint64_t> bitmaps;
    if constexpr (sizeof(unsigned long) == 8) {
      bitmaps.resize(bitmap_ulongs.size());
      for (size_t b = 0; b < bitmap_ulongs.size(); ++b) {
        bitmaps[b] = bitmap_ulongs[b];
      }
    } else { // size is 4
      size_t b = 0;
      while (true) {
        if (b >= bitmap_ulongs.size()) {
          break;
        }
        bitmaps.push_back(bitmap_ulongs[b]);
        ++b;

        if (b >= bitmap_ulongs.size()) {
          break;
        }
        bitmaps.back() |= (((uint64_t)bitmap_ulongs[b]) << 32);
        ++b;
      }
    }
    char *bmapstr;
    hwloc_bitmap_asprintf(&bmapstr, shared_cores);
    std::printf("FAIL to lasso thread to %s aka %lx %lx\n", bmapstr, bitmaps[1],
                bitmaps[0]);
    free(bmapstr);
#endif
  }
}

std::vector<ex_cpu::l3_cache_set>
ex_cpu::group_cores_by_l3c(hwloc_topology_t &topology) {
  // discover the cache groupings
  int l3cache_count = hwloc_get_nbobjs_by_type(topology, HWLOC_OBJ_L3CACHE);
  std::vector<ex_cpu::l3_cache_set> cores_by_l3c;
  cores_by_l3c.reserve(l3cache_count);

  // using DFS, group all cores by shared L3 cache
  hwloc_obj_t curr = hwloc_get_root_obj(topology);
  int stackDepth = 0;
  // stack of our tree traversal. each level stores the current child index
  std::vector<size_t> childIdx(1);
  while (true) {
    if (curr->type == HWLOC_OBJ_L3CACHE && childIdx.back() == 0) {
      cores_by_l3c.push_back({});
      cores_by_l3c.back().l3cache = curr;
    }
    if (curr->type == HWLOC_OBJ_CORE || childIdx.back() >= curr->arity) {
      if (curr->type == HWLOC_OBJ_CORE) {
        // cores_by_l3c.back().cores.push_back(curr);
        cores_by_l3c.back().group_size++;
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
  return cores_by_l3c;
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
#ifndef TMC_USE_MUTEXQ
  work_queues.reserve(PRIORITY_COUNT);
  for (size_t i = 0; i < PRIORITY_COUNT; ++i) {
    work_queues.emplace_back(32000);
  }
#else
  work_queues = new task_queue_t[PRIORITY_COUNT];
#endif
#ifndef TMC_USE_HWLOC
  if (init_params != nullptr && init_params->thread_count != 0) {
    threads.resize(init_params->thread_count);
  }
#else
  hwloc_topology_t topology;
  hwloc_topology_init(&topology);
  hwloc_topology_load(topology);
  auto grouped_cores = group_cores_by_l3c(topology);
  bool lasso = true;
  auto total_thread_count = 0;
  size_t core_count = 0;
  for (size_t i = 0; i < grouped_cores.size(); ++i) {
    core_count += grouped_cores[i].group_size;
  }
  if (init_params == nullptr ||
      (init_params->thread_count == 0 && init_params->thread_occupancy == 0)) {
    total_thread_count = core_count;
  } else {
    if (init_params->thread_count != 0) {
      float occupancy = (float)init_params->thread_count / (float)core_count;
      total_thread_count = init_params->thread_count;
      if (occupancy <= 0.5f) {
        // turn off thread-lasso capability and make everything one group
        grouped_cores.resize(1);
        grouped_cores[0].group_size = init_params->thread_count;
        lasso = false;
      } else {
        // Evenly reduce the size of groups until we hit the desired thread
        // count
        size_t i = grouped_cores.size() - 1;
        while (core_count > init_params->thread_count) {
          grouped_cores[i].group_size--;
          --core_count;
          if (i == 0) {
            i = grouped_cores.size() - 1;
          } else {
            --i;
          }
        }
      }
    } else { // init_params->thread_occupancy != 0
      for (size_t i = 0; i < grouped_cores.size(); ++i) {
        size_t group_size =
            (float)grouped_cores[i].group_size * init_params->thread_occupancy;
        grouped_cores[i].group_size = group_size;
        total_thread_count += group_size;
      }
    }
  }
  threads.resize(total_thread_count);
#endif
  assert(thread_count() != 0);
  assert(thread_count() <= 64);
  thread_states = new thread_state[thread_count()];
  for (size_t i = 0; i < thread_count(); ++i) {
    thread_states[i].yield_priority = NO_TASK_RUNNING;
  }

  thread_stoppers.resize(thread_count());
  if (thread_count() != 0) {
    working_threads_bitset.store(((1ULL << (thread_count() - 1)) - 1) +
                                 (1ULL << (thread_count() - 1)));
  }

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
  std::atomic_thread_fence(std::memory_order_seq_cst);
  size_t slot = 0;
  size_t group_start = 0;
#ifdef TMC_USE_HWLOC
  // copy elements of grouped_cores into thread lambda capture
  // that will go out of scope at the end of this function
  thread_setup_data tdata;
  tdata.groups.resize(grouped_cores.size());
  for (size_t i = 0; i < grouped_cores.size(); ++i) {
    size_t group_size = grouped_cores[i].group_size;
    tdata.groups[i].size = group_size;
    tdata.groups[i].start = group_start;
    group_start += group_size;
  }
  tdata.total_size = group_start;
  for (size_t group_idx = 0; group_idx < grouped_cores.size(); ++group_idx) {
    auto &core_group = grouped_cores[group_idx];
    size_t group_size = core_group.group_size;
    for (size_t sub_idx = 0; sub_idx < group_size; ++sub_idx) {
      auto shared_cores = hwloc_bitmap_dup(core_group.l3cache->cpuset);
#else
  // without HWLOC, treat everything as a single group
  thread_setup_data tdata;
  tdata.groups.push_back({0, thread_count()});
  tdata.total_size = thread_count();
  size_t group_idx = 0;
  while (slot < thread_count()) {
    size_t sub_idx = slot;
#endif
      threads[slot] = std::jthread([
#ifdef TMC_USE_HWLOC

                                       topology, shared_cores, lasso,
#endif
                                       this, tdata, group_idx, sub_idx, slot](
                                       std::stop_token thread_stop_token) {
        init_thread_locals(slot);
#ifdef TMC_USE_HWLOC
        if (lasso) {
          bind_thread(topology, shared_cores);
        }
        hwloc_bitmap_free(shared_cores);
#endif
#ifndef TMC_USE_MUTEXQ
        init_queue_iteration_order(tdata, group_idx, sub_idx, slot);
#endif
        size_t previousPrio = NO_TASK_RUNNING;
      TOP:
        auto cv_value = ready_task_cv.load(std::memory_order_acquire);
        while (try_run_some(thread_stop_token, slot, PRIORITY_COUNT - 1,
                            previousPrio)) {
          auto new_cv_value = ready_task_cv.load(std::memory_order_acquire);
          if (new_cv_value != cv_value) {
            // more tasks have been posted, try again
            cv_value = new_cv_value;
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
                  ~(1ULL << slot), std::memory_order_acq_rel);
            }
          }
          previousPrio = NO_TASK_RUNNING;
          ready_task_cv.wait(cv_value);
          working_threads_bitset.fetch_or(1ULL << slot);
          cv_value = ready_task_cv.load(std::memory_order_acquire);
        }

        working_threads_bitset.fetch_and(~(1ULL << slot));
        clear_thread_locals();
#ifndef TMC_USE_MUTEXQ
        delete[] task_queue_t::this_thread_producers;
        task_queue_t::this_thread_producers = nullptr;
#endif
      });
      thread_stoppers[slot] = threads[slot].get_stop_source();
#ifdef TMC_USE_HWLOC
      ++slot;
    }
  }
  hwloc_topology_destroy(topology);
#else
    ++slot;
  }
#endif
  if (init_params != nullptr) {
    delete init_params;
    init_params = nullptr;
  }
}

#ifndef TMC_PRIORITY_COUNT
ex_cpu &ex_cpu::set_priority_count(size_t npriorities) {
  assert(!is_initialized);
  if (init_params == nullptr) {
    init_params = new InitParams;
  }
  init_params->priority_count = npriorities;
  return *this;
}
size_t ex_cpu::priority_count() {
  assert(is_initialized);
  return PRIORITY_COUNT;
}
#endif
#ifdef TMC_USE_HWLOC
ex_cpu &ex_cpu::set_thread_occupancy(float occupancy) {
  assert(!is_initialized);
  if (init_params == nullptr) {
    init_params = new InitParams;
  }
  init_params->thread_occupancy = occupancy;
  return *this;
}
#endif

ex_cpu &ex_cpu::set_thread_count(size_t nthreads) {
  assert(!is_initialized);
  if (init_params == nullptr) {
    init_params = new InitParams;
  }
  init_params->thread_count = nthreads;
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
  graceful_stop();
  threads.clear();
  thread_stoppers.clear();
#ifndef TMC_USE_MUTEXQ
  for (auto &queue : work_queues) {
    delete[] queue.staticProducers;
  }
  work_queues.clear();
#else
  delete[] work_queues;
#endif
  if (task_stopper_bitsets != nullptr) {
    delete[] task_stopper_bitsets;
  }
  if (thread_states != nullptr) {
    delete[] thread_states;
  }
}

ex_cpu::~ex_cpu() { teardown(); }

namespace detail {
tmc::task<void> client_main_awaiter(tmc::task<int> client_main,
                                    std::atomic<int> *exit_code_out) {
  client_main.resume_on(tmc::cpu_executor());
  int exit_code = co_await client_main;
  exit_code_out->store(exit_code);
  exit_code_out->notify_all();
}
} // namespace detail
int async_main(tmc::task<int> client_main) {
  // if the user already called init(), this will do nothing
  tmc::cpu_executor().init();
  std::atomic<int> exit_code(std::numeric_limits<int>::min());
  post(tmc::cpu_executor(),
       detail::client_main_awaiter(client_main, &exit_code), 0);
  exit_code.wait(std::numeric_limits<int>::min());
  return exit_code.load();
}
} // namespace tmc
