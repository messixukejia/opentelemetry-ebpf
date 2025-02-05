// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include <collector/agent_log.h>
#include <collector/kernel/cgroup_prober.h>
#include <collector/kernel/fd_reader.h>
#include <collector/kernel/probe_handler.h>
#include <collector/kernel/proc_reader.h>
#include <common/host_info.h>

#include <fstream>
#include <iostream>
#include <set>
#include <stack>
#include <string>

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

CgroupProber::CgroupProber(
    ProbeHandler &probe_handler,
    ebpf::BPFModule &bpf_module,
    HostInfo const &host_info,
    std::function<void(void)> periodic_cb,
    std::function<void(std::string)> check_cb)
    : host_info_(host_info), close_dir_error_count_(0)
{
  // END
  ProbeAlternatives kill_kss_probe_alternatives{
      "kill css",
      {
          {"on_kill_css", "kill_css"},
          // Attaching probe to kill_css fails on some distros and kernel builds, for example Ubuntu Jammy.
          {"on_kill_css", "css_clear_dir"},
          // If the previous two fail try an alternative for kernel versions older than 3.12.
          {"on_cgroup_destroy_locked", "cgroup_destroy_locked"},
      }};
  probe_handler.start_probe(bpf_module, kill_kss_probe_alternatives);
  periodic_cb();

  // START
  ProbeAlternatives css_populate_dir_probe_alternatives{
      "css populate dir",
      {
          {"on_css_populate_dir", "css_populate_dir"},
          {"on_cgroup_populate_dir", "cgroup_populate_dir"},
      }};
  probe_handler.start_probe(bpf_module, css_populate_dir_probe_alternatives);
  periodic_cb();

  // check both cgroups v1 and v2 because it is possible for active cgroups to exist in both (hybrid mode)

  // EXISTING cgroups v1
  probe_handler.start_probe(bpf_module, "on_cgroup_clone_children_read", "cgroup_clone_children_read");
  probe_handler.start_probe(bpf_module, "on_cgroup_attach_task", "cgroup_attach_task");

  periodic_cb();
  check_cb("cgroup prober startup");

  // locate the cgroup v1 mount directory
  std::string cgroup_v1_mountpoint = find_cgroup_v1_mountpoint();

  if (!cgroup_v1_mountpoint.empty()) {
    // now iterate over cgroups and trigger cgroup_clone_children_read
    trigger_existing_cgroup_probe(cgroup_v1_mountpoint, "cgroup.clone_children", periodic_cb);
    check_cb("trigger_cgroup_clone_children_read()");
  }

  /* can remove existing now */
  probe_handler.cleanup_probe("cgroup_clone_children_read");

  // EXISTING cgroups v2
  static const std::string cgroup_v2_first_kernel_version("4.6");
  if (host_info_.kernel_version >= cgroup_v2_first_kernel_version) {
    probe_handler.start_probe(bpf_module, "on_cgroup_control", "cgroup_control");
    probe_handler.start_kretprobe(bpf_module, "onret_cgroup_control", "cgroup_control");

    // locate the cgroup v2 mount directory
    std::string cgroup_v2_mountpoint = find_cgroup_v2_mountpoint();

    if (!cgroup_v2_mountpoint.empty()) {
      // now iterate over cgroups and trigger cgroup_control
      trigger_existing_cgroup_probe(cgroup_v2_mountpoint, "cgroup.controllers", periodic_cb);
      check_cb("trigger_cgroup_control()");
    }

    /* can remove existing now */
    probe_handler.cleanup_kretprobe("cgroup_control");
    probe_handler.cleanup_probe("cgroup_control");
  }

  periodic_cb();
  check_cb("cgroup prober cleanup()");
}

void CgroupProber::trigger_existing_cgroup_probe(
    std::string const &cgroup_dir_name, std::string const &file_name, std::function<void(void)> periodic_cb)
{
  std::stack<std::string> dirs_stack;
  dirs_stack.emplace(cgroup_dir_name);
  while (!dirs_stack.empty()) {
    periodic_cb();
    // get the directory on the top of our stack
    std::string dir_name(dirs_stack.top());
    dirs_stack.pop();

    DIR *dir;
    dir = opendir(dir_name.c_str());
    if (!dir)
      continue;

    // trigger the cgroup existing probe for this directory
    std::string path = dir_name + "/" + file_name;
    LOG::debug_in(AgentLogKind::CGROUPS, "cgroup existing probe: path={}", path);
    std::ifstream file(path.c_str());
    if (file.fail()) {
      LOG::debug_in(AgentLogKind::CGROUPS, "   fail for path={}", path);
      int status = closedir(dir);
      if (status != 0) {
        close_dir_error_count_++;
      }
      continue;
    } else {
      LOG::debug_in(AgentLogKind::CGROUPS, "   success for path={}", path);
    }
    std::string line;
    std::getline(file, line);

    // iterate over the elements of this directory and add any
    // subdirectories to dirs_stack
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
      if (ent->d_type == DT_DIR) {
        // skip over "." and ".." entries in the directory
        if ((strcmp(ent->d_name, ".") == 0) || (strcmp(ent->d_name, "..") == 0))
          continue;

        dirs_stack.emplace(dir_name + "/" + ent->d_name);
      }
      periodic_cb();
    }
    int status = closedir(dir);
    if (status != 0) {
      close_dir_error_count_++;
    }
  }
}

static bool file_exists(std::string file_path)
{
  struct stat sb;

  if (stat(file_path.c_str(), &sb) == -1) {
    return false;
  }

  return S_ISREG(sb.st_mode);
}

static bool is_cgroup_v1_mountpoint(std::string dir_path)
{
  static const std::string file_name("/cgroup.clone_children");

  return file_exists(dir_path + file_name);
}

static bool is_cgroup_v2_mountpoint(std::string dir_path)
{
  static const std::string file_name("/cgroup.controllers");

  return file_exists(dir_path + file_name);
}

std::string CgroupProber::find_cgroup_v1_mountpoint()
{
  static const std::vector<std::string> cgroup_v1_mountpoints = {
      "/hostfs/sys/fs/cgroup/memory", "/hostfs/cgroup/memory", "/sys/fs/cgroup/memory", "/cgroup/memory"};

  for (auto const &mountpoint : cgroup_v1_mountpoints) {
    if (is_cgroup_v1_mountpoint(mountpoint)) {
      return mountpoint;
    }
  }

  return std::string();
}

std::string CgroupProber::find_cgroup_v2_mountpoint()
{
  static const std::vector<std::string> cgroup_v2_mountpoints = {
      "/hostfs/sys/fs/cgroup", "/sys/fs/cgroup"
  };

  for (auto const &mountpoint : cgroup_v2_mountpoints) {
    if (is_cgroup_v2_mountpoint(mountpoint)) {
      return mountpoint;
    }
  }

  return std::string();
}
