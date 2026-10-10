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
  // Absolute path of the storage root, for tools that take paths (zip).
  const std::string& root() const { return root_; }
  bool List(const std::string& path, std::vector<Entry>* entries, std::string* error) const;
  bool Stat(const std::string& path, struct stat* info, std::string* error) const;
  bool Remove(const std::string& path, std::string* error) const;
  // Create an empty folder (0755) or empty regular file (0644); existing names are
  // never replaced.
  bool CreateFolder(const std::string& path, std::string* error) const;
  bool CreateFile(const std::string& path, std::string* error) const;
  bool Move(const std::string& from, const Storage& target, const std::string& to,
            std::string* error) const;
  // Reads a regular file (never a symlink) of at most max_size bytes.
  bool ReadFile(const std::string& path, size_t max_size, std::string* data,
                std::string* error) const;
  // Replaces a regular file atomically (temporary file, fsync, rename) keeping its
  // mode, owner and SELinux label. The original is untouched if anything fails.
  bool WriteFileAtomic(const std::string& path, const std::string& data,
                       std::string* error) const;
  // Opens a regular file (never a symlink) for reading; returns -1 on error.
  int OpenRead(const std::string& path, struct stat* info, std::string* error) const;
  static bool ValidName(const std::string& name);
 private:
  int OpenDirectory(const std::string& path) const;
  int OpenParent(const std::string& path, std::string* name) const;
  int fd_;
  std::string root_;
};
std::string DisplayName(const std::string& name);
}  // namespace recovery::files
