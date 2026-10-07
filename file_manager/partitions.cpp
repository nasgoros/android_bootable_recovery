// Copyright (C) 2026 The nasgorOS Project
// SPDX-License-Identifier: Apache-2.0
#include "partitions.h"

#include <errno.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include <android-base/logging.h>
#include <android-base/strings.h>
#include <fs_mgr.h>
#include <fs_mgr/roots.h>
#include <fstab/fstab.h>

#include "install/snapshot_utils.h"
#include "recovery_utils/roots.h"

namespace recovery::partitions {
namespace {
// Current mount of |mount_point| from /proc/mounts.
bool FindMount(const std::string& mount_point, android::fs_mgr::FstabEntry* out) {
  android::fs_mgr::Fstab mounts;
  if (!android::fs_mgr::ReadFstabFromFile("/proc/mounts", &mounts)) return false;
  bool found = false;
  // The last entry wins if something is stacked on the same mount point.
  for (const auto& entry : mounts) {
    if (entry.mount_point == mount_point) {
      *out = entry;
      found = true;
    }
  }
  return found;
}

std::string Errno(const std::string& what) {
  return what + ": " + strerror(errno);
}
}  // namespace

std::vector<Partition> List() {
  const std::vector<std::pair<std::string, std::string>> candidates{
      {"system", android::fs_mgr::GetSystemRoot()},
      {"system_ext", "/system_ext"},
      {"product", "/product"},
      {"vendor", "/vendor"},
      {"odm", "/odm"},
  };
  std::vector<Partition> result;
  for (const auto& [name, path] : candidates) {
    if (volume_for_mount_point(path) == nullptr) continue;
    result.push_back({name, path, "/mnt/" + name});
  }
  return result;
}

State GetState(const Partition& partition) {
  android::fs_mgr::FstabEntry entry;
  if (!FindMount(partition.mount_point, &entry)) return State::kUnmounted;
  return (entry.flags & MS_RDONLY) ? State::kReadOnly : State::kReadWrite;
}

const char* StateLabel(State state) {
  switch (state) {
    case State::kReadOnly: return "RO";
    case State::kReadWrite: return "RW";
    default: return "not mounted";
  }
}

bool Mount(const Partition& partition, bool read_write, std::string* error) {
  if (GetState(partition) == State::kUnmounted) {
    // Dynamic partitions (and Virtual A/B snapshots) must be mapped first.
    if (!logical_partitions_mapped() && !CreateSnapshotPartitions()) {
      *error = "Cannot map dynamic partitions";
      return false;
    }
    mkdir(partition.mount_point.c_str(), 0755);
    if (ensure_path_mounted_at(partition.fstab_path, partition.mount_point) != 0) {
      *error = "Cannot mount " + partition.name;
      return false;
    }
  }
  android::fs_mgr::FstabEntry mounted;
  if (!FindMount(partition.mount_point, &mounted)) {
    *error = partition.name + " not found in /proc/mounts";
    return false;
  }
  bool is_rw = (mounted.flags & MS_RDONLY) == 0;
  if (is_rw == read_write) return true;
  if (read_write && !fs_mgr_set_blk_ro(mounted.blk_device, false)) {
    *error = Errno("Cannot make " + mounted.blk_device + " writable");
    return false;
  }
  unsigned long flags = MS_REMOUNT | (read_write ? 0 : MS_RDONLY);
  if (mount(mounted.blk_device.c_str(), partition.mount_point.c_str(), mounted.fs_type.c_str(),
            flags, nullptr) != 0) {
    *error = Errno(std::string("Remount ") + (read_write ? "RW" : "RO") + " failed");
    return false;
  }
  LOG(INFO) << "Remounted " << partition.mount_point << (read_write ? " rw" : " ro");
  return true;
}

bool Unmount(const Partition& partition, std::string* error) {
  if (GetState(partition) == State::kUnmounted) return true;
  sync();
  if (umount(partition.mount_point.c_str()) != 0) {
    *error = Errno("Unmount " + partition.name + " failed (still in use?)");
    return false;
  }
  return true;
}
void UnmountAll() {
  for (const auto& partition : List()) {
    std::string error;
    if (!Unmount(partition, &error)) LOG(WARNING) << error;
  }
}
}  // namespace recovery::partitions
