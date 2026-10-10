// Copyright (C) 2026 The nasgorOS Project
// SPDX-License-Identifier: Apache-2.0
//
// Building zip archives from files and folders, shared by the recovery `zip`
// command and the file manager. Own zip writer with zip64 (archives, entries and
// entry counts beyond 4 GB / 65535), deflate via zlib. Unit tested on the host.
#pragma once

#include <sys/types.h>
#include <time.h>

#include <stdint.h>

#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace recovery::zip {

class Sink {
 public:
  virtual ~Sink() = default;
  // `name` ends with '/'.
  virtual bool AddDirectory(const std::string& name, time_t mtime, mode_t mode,
                            std::string* error) = 0;
  // Reads the whole file from `fd` (positioned at 0); `size` is a hint from fstat.
  virtual bool AddFile(const std::string& name, int fd, uint64_t size, time_t mtime, mode_t mode,
                       std::string* error) = 0;
};

struct Stats {
  size_t files = 0;
  size_t folders = 0;
  size_t skipped = 0;  // symlinked folders, special files, unreadable entries
  unsigned long long bytes = 0;
};

class Builder {
 public:
  // Entries are never added for this file (the archive being written).
  Builder(Sink* sink, dev_t skip_dev, ino_t skip_ino) : sink_(sink), skip_dev_(skip_dev), skip_ino_(skip_ino) {}

  // Adds `path` (as given on the command line or by the file manager) under the
  // entry name `name`. Folders are added with their contents when `recursive`.
  // Symlinks to files are followed; symlinked folders and special files are
  // skipped with a message to `warn`. Returns false on a fatal write error.
  bool Add(const std::string& path, const std::string& name, bool recursive, std::string* error);

  // Called for each skipped path with the reason.
  std::function<void(const std::string& path, const std::string& reason)> warn;
  // Called after each added file (for progress).
  std::function<void(const std::string& name)> added;

  const Stats& stats() const { return stats_; }

 private:
  bool AddAt(const std::string& path, const std::string& name, bool recursive, int depth,
             std::string* error);
  void Skip(const std::string& path, const std::string& reason);

  Sink* sink_;
  dev_t skip_dev_;
  ino_t skip_ino_;
  Stats stats_;
  std::set<std::string> names_;
};

// Writes a zip archive to a seekable file descriptor (not closed). Local headers are
// patched after each entry, so no data descriptors are needed. Entries use zip64
// fields only when a size or offset needs them.
class ArchiveWriter : public Sink {
 public:
  ArchiveWriter(int fd, bool compress) : fd_(fd), compress_(compress) {}
  bool AddDirectory(const std::string& name, time_t mtime, mode_t mode, std::string* error) override;
  bool AddFile(const std::string& name, int fd, uint64_t size, time_t mtime, mode_t mode,
               std::string* error) override;
  // Writes the central directory (and zip64 end records when needed).
  bool Finish(std::string* error);

 private:
  struct Entry {
    std::string name;
    uint16_t method, time, date, flags;
    uint32_t crc, external;
    uint64_t compressed, uncompressed, offset;
  };
  bool Write(const std::string& data, std::string* error);
  bool PatchHeader(const Entry& entry, bool zip64, std::string* error);

  int fd_;
  bool compress_;
  uint64_t offset_ = 0;
  std::vector<Entry> entries_;
};

struct Callbacks {
  std::function<void(const std::string& name)> progress;                      // per file
  std::function<void(const std::string& path, const std::string& reason)> warn;  // skipped
};

// Writes a new archive at `archive` from (path, entry name) pairs. Folders are added
// recursively when `recursive`; `store` disables compression. The archive is built in
// a temporary file next to it and published without replacing an existing file.
bool CreateZip(const std::string& archive,
               const std::vector<std::pair<std::string, std::string>>& inputs, bool recursive,
               bool store, const Callbacks& callbacks, Stats* stats, std::string* error);

// Entry name for a command-line path: leading "/" and "./" parts and repeated or
// trailing slashes removed. Empty for "", "." or paths with a ".." component.
std::string EntryName(const std::string& path);

}  // namespace recovery::zip
