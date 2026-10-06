// Copyright (C) 2026 The nasgorOS Project
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <string>
#include <vector>

// Mounting of the OS partitions (system, product, ...) for manual changes from
// recovery. Partitions are mounted below /mnt/<name>, read-only or read-write.
namespace recovery::partitions {
struct Partition {
  std::string name;         // "system"
  std::string fstab_path;   // fstab mount point used to find the entry
  std::string mount_point;  // where recovery mounts it, e.g. /mnt/system
};
enum class State { kUnmounted, kReadOnly, kReadWrite };

// Partitions present in the recovery fstab.
std::vector<Partition> List();
State GetState(const Partition& partition);
const char* StateLabel(State state);
// Mounts (or remounts) the partition with the requested access.
bool Mount(const Partition& partition, bool read_write, std::string* error);
bool Unmount(const Partition& partition, std::string* error);
// Unmounts every partition mounted from this menu (before install, wipe, reboot).
void UnmountAll();
}  // namespace recovery::partitions
