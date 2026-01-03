// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/topology.hpp"

#include <functional>
#include <vector>

namespace tmc {
/// Specifies how a multi-threaded executor should construct its work stealing
/// matrix.
enum class work_stealing_strategy {
  /// A hardware-aware work stealing matrix that maximizes the locality of
  /// work distribution across cores.
  HIERARCHY_MATRIX = 0,
  /// A hardware-aware work stealing matrix that maximizes the efficiency of
  /// work-finding across cores.
  LATTICE_MATRIX = 1,
};

namespace detail {

struct InitParams {
  size_t priority_count = 0;
  size_t thread_count = 0;
  size_t spins = 4;
  work_stealing_strategy strategy = work_stealing_strategy::HIERARCHY_MATRIX;
  std::vector<float> thread_occupancy = {};
  std::function<void(tmc::topology::thread_info)> thread_init_hook = nullptr;
  std::function<void(tmc::topology::thread_info)> thread_teardown_hook =
    nullptr;
#ifdef TMC_USE_HWLOC
  std::vector<tmc::topology::topology_filter> partitions = {};
  tmc::topology::thread_pinning_level pin =
    tmc::topology::thread_pinning_level::GROUP;
  tmc::topology::thread_packing_strategy pack =
    tmc::topology::thread_packing_strategy::PACK;

  // Used in conjunction with partitions by multi-threaded executors
  // to implement hybrid work steering
  struct PriorityRange {
    size_t begin;
    size_t end;
  };
  std::vector<PriorityRange> priority_ranges = {};

  void add_partition(tmc::topology::topology_filter const& Filter);
  void set_thread_pinning_level(tmc::topology::thread_pinning_level Pin);
  void set_thread_packing_strategy(tmc::topology::thread_packing_strategy Pack);
  void set_thread_occupancy(
    float ThreadOccupancy, tmc::topology::cpu_kind::value CpuKinds =
                             tmc::topology::cpu_kind::PERFORMANCE
  );

#endif

#ifndef TMC_PRIORITY_COUNT
  void set_priority_count(size_t PriorityCount);
#endif

  void set_thread_count(size_t ThreadCount);

  void set_thread_init_hook(std::function<void(size_t)> const& Hook);

  void set_thread_teardown_hook(std::function<void(size_t)> const& Hook);

  void set_thread_init_hook(
    std::function<void(tmc::topology::thread_info)> const& Hook
  );
  void set_thread_teardown_hook(
    std::function<void(tmc::topology::thread_info)> const& Hook
  );

  void set_spins(size_t Spins);

  void set_work_stealing_strategy(work_stealing_strategy Strategy);
};

} // namespace detail
} // namespace tmc

#ifdef TMC_IMPL
#include "tmc/detail/init_params.ipp"
#endif
