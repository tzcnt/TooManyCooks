// Copyright (c) 2023-2024 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

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
/// co_await or post_waitable().wait() on the root task of the benchmark).
inline size_t wait_for_all_threads_to_sleep(ex_cpu& CpuExecutor) {
  size_t waitCount = 0;

  int runThreads = 0;
  if (tmc::detail::this_thread::exec_is(CpuExecutor.type_erased())) {
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

    // There are many stages of "asleep" - after clearing its bit in
    // working_threads_bitset, each task will check all queues one more time,
    // then wait on ready_task_cv, which may yield to another OS thread, and
    // eventually put the entire CPU core into progressively deeper sleep
    // states. The goal is simply to make sure that the thread isn't in the
    // middle of try_dequeue_ex_cpu() when the next iteration of the benchmark
    // begins... perhaps a "benchmark mode" should be added to the executor
    // thread itself, where it will spin-wait rather than wait on ready_task_cv
    // once the queue is empty.

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
