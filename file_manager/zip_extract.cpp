// Copyright (C) 2026 The nasgorOS Project
// SPDX-License-Identifier: Apache-2.0

#include "zip_extract.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <ziparchive/zip_archive.h>

namespace recovery::zip {
namespace {

std::string Errno(const std::string& action) { return action + ": " + strerror(errno); }

// Creates the folders of `relative` below `root`. `root` was created by this
// extraction and no symlink entries are extracted, so plain paths cannot escape it.
bool MakeDirs(const std::string& root, const std::string& relative, std::string* error) {
  std::string path = root;
  size_t start = 0;
  while (start < relative.size()) {
    size_t end = relative.find('/', start);
    if (end == std::string::npos) end = relative.size();
    path += "/" + relative.substr(start, end - start);
    struct stat st {};
    if (lstat(path.c_str(), &st) == 0) {
      if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        *error = Errno("Create folder " + relative.substr(0, end));
        return false;
      }
    } else if (mkdir(path.c_str(), 0755) != 0) {
      *error = Errno("Create folder " + relative.substr(0, end));
      return false;
    }
    start = end + 1;
  }
  return true;
}

}  // namespace

bool ExtractZip(const std::string& archive, const std::string& dest, const Callbacks& callbacks,
                Stats* stats, std::string* error) {
  error->clear();
  Stats counts;
  ZipArchiveHandle handle;
  int32_t result = OpenArchive(archive.c_str(), &handle);
  if (result != 0) {
    *error = std::string("Not a readable zip archive: ") + ErrorCodeString(result);
    CloseArchive(handle);
    return false;
  }
  void* cookie = nullptr;
  result = StartIteration(handle, &cookie);
  if (result != 0) {
    *error = std::string("Read archive: ") + ErrorCodeString(result);
    CloseArchive(handle);
    return false;
  }
  if (mkdir(dest.c_str(), 0755) != 0) {
    *error = Errno("Create folder " + dest);
    EndIteration(cookie);
    CloseArchive(handle);
    return false;
  }

  bool ok = true;
  ZipEntry64 entry;
  std::string raw_name;
  while ((result = Next(cookie, &entry, &raw_name)) == 0) {
    const bool folder = !raw_name.empty() && raw_name.back() == '/';
    const std::string name = EntryName(raw_name);
    const bool unix_entry = (entry.version_made_by >> 8) == 3;
    const mode_t mode = unix_entry ? static_cast<mode_t>(entry.external_file_attributes >> 16) : 0;
    if (name.empty() || raw_name[0] == '/' || raw_name.find('\\') != std::string::npos) {
      ++counts.skipped;
      if (callbacks.warn) callbacks.warn(raw_name, "unsafe entry name");
      continue;
    }
    if (unix_entry && S_ISLNK(mode)) {
      ++counts.skipped;
      if (callbacks.warn) callbacks.warn(raw_name, "symlink entry");
      continue;
    }
    if (folder) {
      if (!MakeDirs(dest, name, error)) {
        ok = false;
        break;
      }
      ++counts.folders;
      continue;
    }
    const size_t slash = name.rfind('/');
    if (slash != std::string::npos && !MakeDirs(dest, name.substr(0, slash), error)) {
      ok = false;
      break;
    }
    // Keep execute bits from Unix archives (scripts, binaries); always owner-writable.
    const mode_t perm = unix_entry && (mode & 0777) != 0 ? ((mode & 0755) | 0600) : 0644;
    int fd = open((dest + "/" + name).c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                  perm);
    if (fd < 0) {
      *error = Errno("Create " + name);
      ok = false;
      break;
    }
    result = ExtractEntryToFile(handle, &entry, fd);
    const bool synced = result == 0 && fsync(fd) == 0;
    close(fd);
    if (!synced) {
      *error = result != 0 ? name + ": " + ErrorCodeString(result) : Errno("Flush " + name);
      ok = false;
      break;
    }
    ++counts.files;
    counts.bytes += entry.uncompressed_length;
    if (callbacks.progress) callbacks.progress(name);
  }
  if (ok && result != -1) {  // -1 ends the iteration
    *error = std::string("Read archive: ") + ErrorCodeString(result);
    ok = false;
  }
  EndIteration(cookie);
  CloseArchive(handle);
  if (stats) *stats = counts;
  return ok;
}

}  // namespace recovery::zip
