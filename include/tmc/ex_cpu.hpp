#pragma once
#include <hwloc/bitmap.h>
#include <limits>
#ifdef TMC_USE_MUTEXQ
#include "tmc/detail/qu_mutex.hpp"
#else
#include "tmc/detail/qu_lockfree.h"
#endif
#include "tmc/task.hpp"
#ifndef TMC_NO_POST_FUNC
#include "tmc/detail/coro_functor.hpp"
#endif
#ifdef TMC_USE_HWLOC
#include "hwloc.h"
#endif
#include "tmc/detail/thread_locals.hpp"
#include <array>
#include <atomic>
#include <compare>
#include <concepts>
#include <condition_variable>
#include <functional>
#include <string>
#include <type_traits>
#if defined(__x86_64__)
#include <immintrin.h>
#else
#include <arm_acle.h>
#endif
#include <mutex>
#include <stop_token>
#include <thread>
namespace tmc {

struct alignas(64) thread_state {
  std::atomic<size_t> yield_priority;
};

struct InitParams {
  size_t priority_count = 0;
  size_t thread_count = 0;
};

class ex_cpu {
#ifdef TMC_USE_MUTEXQ
  using task_queue_t = detail::MutexQueue<work_item>;
#else
  using task_queue_t = tmc::queue::ConcurrentQueue<work_item>;
#endif
  std::vector<task_queue_t> work_waiting; // size() == PRIORITY_COUNT
  std::vector<std::jthread> threads;      // size() == thread_count()
  std::mutex threads_mutex;               // protects access to threads
  tmc::detail::type_erased_executor type_erased_this;
  // stop_sources that correspond to this pool's threads
  std::vector<std::stop_source> thread_stoppers;
  // TODO maybe shrink this by 1? we don't need to stop prio 0 tasks
  thread_state *thread_states;                 // array of size thread_count()
  std::atomic<uint64_t> *task_stopper_bitsets; // array of size PRIORITY_COUNT
  std::atomic<int> ready_task_cv;              // monotonic counter
  std::atomic<uint64_t> working_threads_bitset;

  bool is_initialized = false;
  InitParams *init_params; // accessed only during init()
  // capitalized variables are constant while ex_cpu is initialized & running
#ifdef TMC_PRIORITY_COUNT
  static constexpr size_t PRIORITY_COUNT = TMC_PRIORITY_COUNT;
  static constexpr size_t NO_TASK_RUNNING = TMC_PRIORITY_COUNT;
#else
  size_t PRIORITY_COUNT;
  size_t NO_TASK_RUNNING;
#endif

  void notify_n(size_t priority, size_t count);

private:
  void init_thread_locals(size_t slot);
#ifndef TMC_USE_MUTEXQ
  void init_queue_iteration_order(size_t slot, size_t group_start,
                                  size_t group_size, size_t total_size);
#endif
  void clear_thread_locals();
#ifdef TMC_USE_HWLOC
  struct l3_cache_set {
    hwloc_obj_t l3cache;
    std::vector<hwloc_obj_t> cores;
  };
  // NUMALatency exposed by hwloc (stored in System Locality Distance
  // Information Table) is not helpful if the system is not confirmed as NUMA
  // Use l3 cache groupings instead
  // TODO handle non-uniform core layouts (Intel/ARM hybrid architecture)
  // https://utcc.utoronto.ca/~cks/space/blog/linux/IntelHyperthreadingSurprise
  std::vector<l3_cache_set> group_cores_by_l3c(hwloc_topology_t &topology);

  // bind this thread to any of the cores that share l3 cache in this set
  void bind_thread(hwloc_topology_t topology, hwloc_cpuset_t shared_cores);
#endif

  // returns true if no tasks were found (caller should wait on cv)
  // returns false if thread stop requested (caller should exit)
  bool try_run_some(std::stop_token &thread_stop_token, const size_t slot,
                    const size_t minPriority, size_t &previousPrio);

public:
  // Builder func to set the number of threads before calling init().
  // The default is 0, which will cause init() to automatically create 1 thread
  // per physical core.
  ex_cpu &set_thread_count(size_t nthreads);
#ifndef TMC_PRIORITY_COUNT
  // Builder func to set the number of priority levels before calling init().
  // The default is 1.
  ex_cpu &set_priority_count(size_t npriorities);
#endif
  // Gets the number of worker threads. Only useful after init() has been
  // called.
  size_t thread_count();
  // Gets the number of priority levels. Only useful after init() has been
  // called.
  size_t priority_count();
  // Initializes the executor. If you want to customize the behavior, call the
  // set_X() functions before calling init(). By default, uses hwloc to
  // automatically generate threads, and creates 1 (or TMC_PRIORITY_COUNT)
  // priority levels.
  void init();
  // Stops the executor, joins the worker threads, and destroys resources.
  // Restores the executor to an uninitialized state. After calling teardown(),
  // you may call set_X() to reconfigure the executor and call init() again.
  void teardown();
  // After constructing, you must call init() before use.
  ex_cpu();
  ~ex_cpu();

  // not movable or copyable due to type_erased_this pointer being accessible by
  // child threads
  ex_cpu &operator=(const ex_cpu &other) = delete;
  ex_cpu(const ex_cpu &other) = delete;
  ex_cpu &operator=(const ex_cpu &&other) = delete;
  ex_cpu(const ex_cpu &&other) = delete;

  void post_variant(work_item &&item, size_t priority);

  void graceful_stop();
  void hard_stop();

  detail::type_erased_executor *type_erased();

  template <typename It>
  void post_bulk(It items, size_t priority, size_t count) {
#ifdef TMC_USE_MUTEXQ
    work_waiting[priority].enqueue_bulk_ex_cpu(items, count);
#else
    work_waiting[priority].enqueue_bulk_ex_cpu(items, count, priority);
#endif
    notify_n(priority, count);
  }
};

namespace detail {
inline ex_cpu g_ex_cpu;
} // namespace detail

constexpr ex_cpu &cpu_executor() { return detail::g_ex_cpu; }
namespace detail {
tmc::task<void> client_main_awaiter(tmc::task<int> client_main,
                                    std::atomic<int> *exit_code_out);
}
int async_main(tmc::task<int> client_main);
} // namespace tmc

#ifdef TMC_IMPL
#include "tmc/detail/ex_cpu.ipp"
#endif
