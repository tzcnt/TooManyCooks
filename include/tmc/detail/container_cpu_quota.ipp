// Copyright (c) 2026 Logan McDougall
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE or copy at http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "tmc/detail/impl.hpp" // IWYU pragma: keep

#include "tmc/detail/container_cpu_quota.hpp"

#ifdef __linux__
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#endif

namespace tmc {
namespace detail {

#ifdef __linux__
namespace {

struct mount_point {
  std::string mount_point_path;
  std::string root;
  std::string fs_type;
  std::string super_options;
};

struct cgroup_subsys {
  int id;
  std::string subsystems;
  std::string path;
};

std::vector<std::string> split_string(const std::string& s, char delim) {
  std::vector<std::string> result;
  std::stringstream ss(s);
  std::string item;
  while (std::getline(ss, item, delim)) {
    result.push_back(item);
  }
  return result;
}

std::vector<cgroup_subsys> parse_proc_cgroup(const std::string& path) {
  std::vector<cgroup_subsys> result;
  std::ifstream file(path);
  if (!file.is_open()) {
    return result;
  }

  std::string line;
  while (std::getline(file, line)) {
    auto fields = split_string(line, ':');
    if (fields.size() != 3) {
      continue;
    }
    cgroup_subsys subsys;
    subsys.id = std::atoi(fields[0].c_str());
    subsys.subsystems = fields[1];
    subsys.path = fields[2];
    result.push_back(subsys);
  }
  return result;
}

std::vector<mount_point> parse_mountinfo(const std::string& path) {
  std::vector<mount_point> result;
  std::ifstream file(path);
  if (!file.is_open()) {
    return result;
  }

  std::string line;
  while (std::getline(file, line)) {
    auto fields = split_string(line, ' ');
    if (fields.size() < 10) {
      continue;
    }

    mount_point mp;
    mp.root = fields[3];
    mp.mount_point_path = fields[4];

    size_t separator_idx = 0;
    for (size_t i = 6; i < fields.size(); ++i) {
      if (fields[i] == "-") {
        separator_idx = i;
        break;
      }
    }
    if (separator_idx == 0 || separator_idx + 2 >= fields.size()) {
      continue;
    }
    mp.fs_type = fields[separator_idx + 1];
    mp.super_options = fields[separator_idx + 3];
    result.push_back(mp);
  }
  return result;
}

bool is_cgroups_v2() {
  auto mounts = parse_mountinfo("/proc/self/mountinfo");
  for (const auto& mp : mounts) {
    if (mp.fs_type == "cgroup2" && mp.mount_point_path == "/sys/fs/cgroup") {
      return true;
    }
  }
  return false;
}

std::string find_cgroup_v2_path() {
  auto subsystems = parse_proc_cgroup("/proc/self/cgroup");
  for (const auto& subsys : subsystems) {
    if (subsys.id == 0) {
      return subsys.path;
    }
  }
  return "";
}

std::string find_cgroup_v1_path() {
  auto subsystems = parse_proc_cgroup("/proc/self/cgroup");
  auto mounts = parse_mountinfo("/proc/self/mountinfo");

  std::string cgroup_path;
  for (const auto& subsys : subsystems) {
    if (subsys.subsystems.find("cpu") != std::string::npos) {
      cgroup_path = subsys.path;
      break;
    }
  }
  if (cgroup_path.empty()) {
    return "";
  }

  for (const auto& mp : mounts) {
    if (mp.fs_type != "cgroup") {
      continue;
    }
    if (mp.super_options.find("cpu") != std::string::npos) {
      if (cgroup_path.find(mp.root) == 0) {
        std::string relative_path = cgroup_path.substr(mp.root.length());
        if (relative_path.empty()) {
          relative_path = "/";
        }
        return mp.mount_point_path + relative_path;
      }
      return mp.mount_point_path + cgroup_path;
    }
  }
  return "";
}

int64_t read_int_from_file(const std::string& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    return -1;
  }
  int64_t value;
  if (!(file >> value)) {
    return -1;
  }
  return value;
}

container_cpu_quota query_cgroups_v2() {
  container_cpu_quota result;
  result.cpu_count = 0.0;
  result.status = container_cpu_status::UNLIMITED;

  std::string cgroup_path = find_cgroup_v2_path();
  std::string cpu_max_path = "/sys/fs/cgroup" + cgroup_path + "/cpu.max";

  std::ifstream file(cpu_max_path);
  if (!file.is_open()) {
    return result;
  }

  std::string line;
  if (!std::getline(file, line)) {
    return result;
  }

  auto fields = split_string(line, ' ');
  if (fields.empty() || fields.size() > 2) {
    result.status = container_cpu_status::UNKNOWN;
    return result;
  }

  if (fields[0] == "max") {
    return result;
  }

  int64_t max_quota;
  try {
    max_quota = std::stoll(fields[0]);
  } catch (...) {
    result.status = container_cpu_status::UNKNOWN;
    return result;
  }

  int64_t period = 100000;
  if (fields.size() == 2) {
    try {
      period = std::stoll(fields[1]);
    } catch (...) {
      result.status = container_cpu_status::UNKNOWN;
      return result;
    }
    if (period == 0) {
      result.status = container_cpu_status::UNKNOWN;
      return result;
    }
  }

  result.cpu_count =
    static_cast<double>(max_quota) / static_cast<double>(period);
  result.status = container_cpu_status::LIMITED;
  return result;
}

container_cpu_quota query_cgroups_v1() {
  container_cpu_quota result;
  result.cpu_count = 0.0;
  result.status = container_cpu_status::UNLIMITED;

  std::string cgroup_path = find_cgroup_v1_path();
  if (cgroup_path.empty()) {
    return result;
  }

  std::string quota_path = cgroup_path + "/cpu.cfs_quota_us";
  std::string period_path = cgroup_path + "/cpu.cfs_period_us";

  int64_t quota = read_int_from_file(quota_path);
  int64_t period = read_int_from_file(period_path);

  if (quota <= 0) {
    return result;
  }
  if (period <= 0) {
    return result;
  }

  result.cpu_count = static_cast<double>(quota) / static_cast<double>(period);
  result.status = container_cpu_status::LIMITED;
  return result;
}

} // namespace
#endif

container_cpu_quota query_container_cpu_quota() {
#ifdef __linux__
  if (is_cgroups_v2()) {
    return query_cgroups_v2();
  }
  return query_cgroups_v1();
#else
  container_cpu_quota result;
  result.cpu_count = 0.0;
  result.status = container_cpu_status::UNLIMITED;
  return result;
#endif
}

} // namespace detail
} // namespace tmc
