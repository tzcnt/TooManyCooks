// Copyright (c) 2023-2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/topology.hpp"

#include <functional>
#include <vector>

namespace tmc {
namespace detail {

struct InitParams {
  size_t priority_count = 0;
  size_t thread_count = 0;
  size_t spins = 4;
  std::vector<float> thread_occupancy = {};
  std::function<void(size_t)> thread_init_hook = nullptr;
  std::function<void(size_t)> thread_teardown_hook = nullptr;
#ifdef TMC_USE_HWLOC
  std::vector<tmc::topology::TopologyFilter> partitions = {};
  void add_partition(tmc::topology::TopologyFilter const& Filter);

  // Used in conjunction with partitions by multi-threaded executors
  // to implement hybrid work steering
  struct PriorityRange {
    size_t begin;
    size_t end;
  };
  std::vector<PriorityRange> priority_ranges = {};

  tmc::topology::ThreadPinningLevel pin =
    tmc::topology::ThreadPinningLevel::GROUP;
  void set_thread_pinning_level(tmc::topology::ThreadPinningLevel Pin);

  tmc::topology::ThreadPackingStrategy pack =
    tmc::topology::ThreadPackingStrategy::PACK;
  void set_thread_packing_strategy(tmc::topology::ThreadPackingStrategy Pack);

  void set_thread_occupancy(
    float ThreadOccupancy,
    tmc::topology::CpuKind::value CpuKinds = tmc::topology::CpuKind::PERFORMANCE
  );
#endif

#ifndef TMC_PRIORITY_COUNT
  void set_priority_count(size_t PriorityCount);
#endif

  void set_thread_count(size_t ThreadCount);

  /// Hook will be invoked at the startup of each thread owned by this executor,
  /// and passed the ordinal index (0..thread_count()-1) of the thread.
  void set_thread_init_hook(std::function<void(size_t)> const& Hook);

  /// Hook will be invoked before destruction of each thread owned by this
  /// executor, and passed the ordinal index (0..thread_count()-1) of the
  /// thread.
  void set_thread_teardown_hook(std::function<void(size_t)> const& Hook);

  void set_spins(size_t Spins);
};

} // namespace detail
} // namespace tmc

#ifdef TMC_IMPL
#include "tmc/detail/init_params.ipp"
#endif
