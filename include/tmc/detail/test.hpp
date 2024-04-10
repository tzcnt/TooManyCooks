#pragma once

#include "tmc/detail/thread_locals.hpp"
#include "tmc/ex_cpu.hpp"

#if defined(__x86_64__) || defined(_M_AMD64)
#include <immintrin.h>
#else
#include <arm_acle.h>
#endif

namespace tmc {
namespace test {
/// The executor doesn't provide any condition variable for this, so just use a
/// spin loop. This is not designed to be efficient, but simply a way to make
/// sure that all of the threads are asleep before starting the next iteration
/// of a benchmark. To avoid having this spin-wait impact the benchmark itself,
/// this should only be called after the benchmark has completed (by using
/// post_waitable().wait() on the root task of the benchmark).
size_t wait_for_all_threads_to_sleep(ex_cpu& CpuExecutor) {
  size_t waitCount = 0;

  size_t runThreads = 0;
  if (detail::this_thread::executor == CpuExecutor.type_erased()) {
    // if this function is being called from an executor thread, we will never
    // see ALL threads sleeping - at least this one will always be running
    runThreads = 1;
  }

  while (__builtin_popcountll(
           CpuExecutor.working_threads_bitset.load(std::memory_order_acquire)
         ) > runThreads) {

    // on desktop 5950X or laptop 5900HX - this works fine with either 1x, 10x,
    // or 100x _mm_pause, or sleep_for(1ms) or sleep_for(10ms) - the benchmark
    // timing is very consistent (no negative impact) in any scenarios, and it
    // only needs to sleep/wait about 5/1000 runs

    // on server EPYC 7742, not the same - more pauses / longer sleep = slower
    // benchmark (even though sleep time is not part of the benchmark time). I
    // believe this is because the server CPUs go to sleep if you wait long
    // enough, and have long transition latency to wake back up. The work is
    // also more poorly distributed on server CPUs causing some threads to
    // finish much sooner than others. Using `cpupower frequency-set -g
    // performance` mitigates this somewhat. It also has to wait almost every
    // loop iteration (995/1000 runs).

    for (int i = 0; i < 4; ++i) {
#if defined(__x86_64__) || defined(_M_AMD64)
      _mm_pause();
#endif
#if defined(__arm__) || defined(_M_ARM) || defined(__aarch64__) ||             \
  defined(__ARM_ACLE)
      __yield();
#endif
    }
    // std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ++waitCount;
  }
  return waitCount;
}
} // namespace test
} // namespace tmc