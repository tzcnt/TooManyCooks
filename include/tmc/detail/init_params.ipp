// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#include "tmc/detail/bit_manip.hpp"
#include "tmc/detail/init_params.hpp"

#include <cassert>

namespace tmc {
namespace detail {

#ifdef TMC_USE_HWLOC
void InitParams::set_thread_occupancy(
  float ThreadOccupancy, tmc::topology::CpuKind::value CpuKinds
) {
  if (thread_occupancy.empty()) {
    thread_occupancy = {1.0f, 1.0f, 1.0f};
  }
  size_t input =
    CpuKinds & tmc::topology::CpuKind::ALL; // mask off out-of-range values
  while (input != 0) {
    auto bitIdx = tmc::detail::tzcnt(input);
    input = tmc::detail::blsr(input);
    thread_occupancy[bitIdx] = ThreadOccupancy;
  }
}

void InitParams::add_partition(tmc::topology::TopologyFilter const& Filter) {
  partitions.push_back(Filter);
}

void InitParams::set_thread_pinning_level(
  tmc::topology::ThreadPinningLevel Level
) {
  pin = Level;
}

void InitParams::set_thread_packing_strategy(
  tmc::topology::ThreadPackingStrategy Strategy
) {
  pack = Strategy;
}
#endif

void InitParams::set_thread_count(size_t ThreadCount) {
  // limited to 32/64 threads for now, due to use of size_t bitset
  assert(ThreadCount <= TMC_PLATFORM_BITS);
  thread_count = ThreadCount;
}

void InitParams::set_thread_init_hook(std::function<void(size_t)> const& Hook) {
  thread_init_hook = Hook;
}

void InitParams::set_thread_teardown_hook(
  std::function<void(size_t)> const& Hook
) {
  thread_teardown_hook = Hook;
}

void InitParams::set_spins(size_t Spins) { spins = Spins; }

void InitParams::set_work_stealing_strategy(WorkStealingStrategy Strategy) {
  work_stealing_strategy = Strategy;
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
