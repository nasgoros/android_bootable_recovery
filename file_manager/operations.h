// Copyright (C) 2026 The nasgorOS Project
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <sys/stat.h>
#include <string>
#include <vector>

namespace recovery::files {
struct Entry {
  std::string name;
  struct stat info {};
};
// All paths are relative to an opened storage root. Never follows symlinks or
// traverses a child mount. Roots themselves cannot be removed or renamed.
class Storage {
 public:
  explicit Storage(const std::string& path);
  ~Storage();
  Storage(const Storage&) = delete;
  Storage& operator=(const Storage&) = delete;
  bool valid() const { return fd_ >= 0; }
  bool List(const std::string& path, std::vector<Entry>* entries, std::string* error) const;
  bool Stat(const std::string& path, struct stat* info, std::string* error) const;
  bool Remove(const std::string& path, std::string* error) const;
  bool Move(const std::string& from, const Storage& target, const std::string& to,
            std::string* error) const;
  static bool ValidName(const std::string& name);
 private:
  int OpenDirectory(const std::string& path) const;
  int OpenParent(const std::string& path, std::string* name) const;
  int fd_;
};
std::string DisplayName(const std::string& name);
}  // namespace recovery::files
