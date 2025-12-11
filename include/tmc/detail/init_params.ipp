// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#include "tmc/detail/bit_manip.hpp"
#include "tmc/detail/init_params.hpp"

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

void InitParams::set_topology_filter(
  tmc::topology::TopologyFilter const& Filter
) {
  partition = Filter;
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
