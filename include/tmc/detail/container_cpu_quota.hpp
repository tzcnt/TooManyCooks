// Copyright (c) 2025 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

namespace tmc {
namespace detail {

/// Status of container CPU quota detection
enum class container_cpu_status {
  /// No container CPU quota is configured (unlimited)
  UNLIMITED,

  /// CPU quota was successfully detected
  LIMITED,

  /// An error occurred during detection
  UNKNOWN
};

/// Result of container CPU quota detection
struct container_cpu_quota {
  /// The CPU quota as a floating point value.
  /// For example, 2.5 means 2.5 CPUs worth of quota.
  /// Only valid when status == LIMITED.
  double cpu_count;

  /// Status of the detection
  container_cpu_status status;

  /// Returns true if running in a container with CPU limits
  inline bool is_container_limited() const {
    return status == container_cpu_status::LIMITED;
  }
};

/// Query the container CPU quota from cgroups (v1 or v2).
/// This function detects whether the application is running in a container
/// with CPU limits and returns the effective CPU count.
///
/// Detection priority:
/// 1. cgroups v2 (unified hierarchy) - reads /sys/fs/cgroup/{path}/cpu.max
/// 2. cgroups v1 - reads cpu.cfs_quota_us and cpu.cfs_period_us
///
/// Returns container_cpu_status::UNLIMITED if:
/// - Not running on Linux
/// - Not running in a container with CPU limits
/// - CPU quota is set to "max" (unlimited)
///
/// The cpu_count field represents the CPU quota as a float.
/// For example, if quota=250000 and period=100000, cpu_count=2.5
container_cpu_quota query_container_cpu_quota();

} // namespace detail
} // namespace tmc

#ifdef TMC_IMPL
#include "tmc/detail/container_cpu_quota.ipp"
#endif
