#pragma once

#if defined(__x86_64__) || defined(_M_AMD64)
#include <immintrin.h>
#else
#include <arm_acle.h>
#endif

#include "tmc/ex_cpu.hpp"
namespace tmc {
namespace test {
/// The executor doesn't provide any condition variable for this, so just use a
/// spin loop. This is not designed to be efficient, but simply a way to make
/// sure that all of the threads are asleep before starting the next iteration
/// of a benchmark. To avoid having this spin-wait impact the benchmark itself,
/// this should only be called after the benchmark has completed (by using
/// post_waitable().wait() on the root task of the benchmark).
size_t wait_for_all_threads_to_sleep(ex_cpu& CpuExecutor) {
  size_t count = 0;
  // maybe this doesn't work? WTBS can be spuriously zero?
  // otherwise why does sleep_for behave very differently from mm_pause?
  while (CpuExecutor.working_threads_bitset.load(std::memory_order_acquire) != 0
  ) {
    // this doesn't seem to work properly - increasing this to 10x results in
    // 2x slowdown of the benchmark maybe because of core power state
    // transition?
    // or adding a single MM_PAUSE to the dequeue_ex_cpu loop also causes 2x
    // slowdown

    // on desktop - it works fine with either 1x, 10x, or 100x _mm_pause,
    // or sleep_for(1ms) or sleep_for(10ms) - the benchmark timing is very
    // consistent (no negative impact) in any scenarios, and it only needs to
    // sleep/wait about 5/1000 runs

    //     for (int i = 0; i < 100; ++i) {
    // #if defined(__x86_64__) || defined(_M_AMD64)
    //       _mm_pause();
    // #endif
    // #if defined(__arm__) || defined(_M_ARM) || defined(__aarch64__) || \
//   defined(__ARM_ACLE)
    //       __yield();
    // #endif
    //     }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ++count;
  }
  return count;
}
} // namespace test
} // namespace tmc