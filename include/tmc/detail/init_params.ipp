// Copyright (c) 2023-2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#include "tmc/detail/compat.hpp"
#include "tmc/detail/init_params.hpp"

#include <cassert>

#ifdef TMC_USE_HWLOC
#include "tmc/detail/bit_manip.hpp"
#endif

namespace tmc {
namespace detail {

#ifdef TMC_USE_HWLOC
void InitParams::set_thread_occupancy(
  float ThreadOccupancy, tmc::topology::cpu_kind::value CpuKinds
) {
  if (thread_occupancy.empty()) {
    thread_occupancy = {1.0f, 1.0f, 1.0f};
  }
  size_t input =
    CpuKinds & tmc::topology::cpu_kind::ALL; // mask off out-of-range values
  while (input != 0) {
    auto bitIdx = tmc::detail::tzcnt(input);
    input = tmc::detail::blsr(input);
    thread_occupancy[bitIdx] = ThreadOccupancy;
  }
}

void InitParams::add_partition(tmc::topology::topology_filter const& Filter) {
  partitions.push_back(Filter);
}

void InitParams::set_thread_pinning_level(
  tmc::topology::thread_pinning_level Level
) {
  pin = Level;
}

void InitParams::set_thread_packing_strategy(
  tmc::topology::thread_packing_strategy Strategy
) {
  pack = Strategy;
}
#endif

void InitParams::set_thread_count(size_t ThreadCount) {
#ifndef TMC_MORE_THREADS
  // limited to 32/64 threads due to use of size_t bitset
  assert(ThreadCount <= TMC_PLATFORM_BITS);
#endif
  thread_count = ThreadCount;
}

void InitParams::set_thread_init_hook(std::function<void(size_t)> const& Hook) {
  thread_init_hook = [Hook](tmc::topology::thread_info Info) {
    Hook(Info.index);
  };
}

void InitParams::set_thread_teardown_hook(
  std::function<void(size_t)> const& Hook
) {
  thread_teardown_hook = [Hook](tmc::topology::thread_info Info) {
    Hook(Info.index);
  };
}

void InitParams::set_thread_init_hook(
  std::function<void(tmc::topology::thread_info)> const& Hook
) {
  thread_init_hook = Hook;
}

void InitParams::set_thread_teardown_hook(
  std::function<void(tmc::topology::thread_info)> const& Hook
) {
  thread_teardown_hook = Hook;
}

void InitParams::set_spins(size_t Spins) { spins = Spins; }

void InitParams::set_work_stealing_strategy(work_stealing_strategy Strategy) {
  strategy = Strategy;
}

#ifndef TMC_PRIORITY_COUNT
void InitParams::set_priority_count(size_t PriorityCount) {
  assert(PriorityCount <= 16 && "The maximum number of priority levels is 16.");
  if (PriorityCount > 16) {
    PriorityCount = 16;
  }
  priority_count = PriorityCount;
}
#endif
} // namespace detail
} // namespace tmc
