// Copyright (C) 2026 The nasgorOS Project
// SPDX-License-Identifier: Apache-2.0
#include "operations.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <utility>

namespace recovery::files {
namespace {
class Fd {
 public:
  explicit Fd(int fd) : fd_(fd) {}
  ~Fd() { if (fd_ >= 0) close(fd_); }
  operator int() const { return fd_; }
 private:
  int fd_;
};
bool Fail(std::string* error, const std::string& action) {
  *error = action + ": " + strerror(errno);
  return false;
}
bool SyncDir(int fd) {
  return fsync(fd) == 0 || errno == EINVAL || errno == EROFS;
}
int ChildDirectory(int parent, const std::string& name) {
  int fd = openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return -1;
  struct stat a {}, b {};
  if (fstat(parent, &a) || fstat(fd, &b) || a.st_dev != b.st_dev) {
    close(fd);
    errno = EXDEV;
    return -1;
  }
  return fd;
}
bool Names(int fd, std::vector<std::string>* names) {
  // openat creates a fresh directory stream; dup would share the seek offset.
  DIR* raw = fdopendir(openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (!raw) return false;
  std::unique_ptr<DIR, decltype(&closedir)> dir(raw, closedir);
  errno = 0;
  while (auto* entry = readdir(dir.get())) {
    std::string name(entry->d_name);
    if (name != "." && name != "..") names->push_back(std::move(name));
    errno = 0;
  }
  return errno == 0;
}
bool RemoveAt(int parent, const std::string& name, unsigned depth, std::string* error) {
  if (depth > 64) { errno = ELOOP; return Fail(error, "Folder too deep"); }
  struct stat st {};
  if (fstatat(parent, name.c_str(), &st, AT_SYMLINK_NOFOLLOW)) return Fail(error, "Read entry");
  if (S_ISDIR(st.st_mode)) {
    Fd child(ChildDirectory(parent, name));
    if (child < 0) return Fail(error, "Open folder");
    std::vector<std::string> names;
    if (!Names(child, &names)) return Fail(error, "List folder");
    for (const auto& item : names) {
      if (!RemoveAt(child, item, depth + 1, error)) return false;
    }
    if (unlinkat(parent, name.c_str(), AT_REMOVEDIR)) return Fail(error, "Remove folder");
  } else if (unlinkat(parent, name.c_str(), 0)) {
    return Fail(error, "Remove file");
  }
  return true;
}
bool CopyAt(int source, const std::string& name, int dest, const std::string& new_name,
            unsigned depth, std::string* error) {
  if (depth > 64) { errno = ELOOP; return Fail(error, "Folder too deep"); }
  struct stat st {};
  if (fstatat(source, name.c_str(), &st, AT_SYMLINK_NOFOLLOW)) return Fail(error, "Read source");
  if (S_ISDIR(st.st_mode)) {
    Fd input(ChildDirectory(source, name));
    if (input < 0) return Fail(error, "Open source folder");
    if (mkdirat(dest, new_name.c_str(), 0700)) return Fail(error, "Create folder");
    Fd output(ChildDirectory(dest, new_name));
    if (output < 0) return Fail(error, "Open new folder");
    std::vector<std::string> names;
    if (!Names(input, &names)) return Fail(error, "List source");
    for (const auto& item : names) {
      if (!CopyAt(input, item, output, item, depth + 1, error)) return false;
    }
    // Storage filesystems may not implement Unix permissions or timestamps.
    fchown(output, st.st_uid, st.st_gid);
    fchmod(output, st.st_mode & 0777);
    const timespec times[] = {st.st_atim, st.st_mtim};
    futimens(output, times);
    if (!SyncDir(output)) return Fail(error, "Flush folder");
    return true;
  }
  if (S_ISLNK(st.st_mode)) {
    std::array<char, 4096> link;
    ssize_t size = readlinkat(source, name.c_str(), link.data(), link.size());
    if (size < 0) return Fail(error, "Read link");
    if (static_cast<size_t>(size) == link.size()) { errno = ENAMETOOLONG; return Fail(error, "Link"); }
    std::string value(link.data(), size);
    if (symlinkat(value.c_str(), dest, new_name.c_str())) return Fail(error, "Copy link");
    fchownat(dest, new_name.c_str(), st.st_uid, st.st_gid, AT_SYMLINK_NOFOLLOW);
    return true;
  }
  if (!S_ISREG(st.st_mode)) { errno = ENOTSUP; return Fail(error, "Special files cannot be moved"); }
  Fd input(openat(source, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
  if (input < 0) return Fail(error, "Open source");
  struct stat before {};
  if (fstat(input, &before) || !S_ISREG(before.st_mode) || before.st_ino != st.st_ino ||
      before.st_dev != st.st_dev) { errno = ESTALE; return Fail(error, "Source changed"); }
  Fd output(openat(dest, new_name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
  if (output < 0) return Fail(error, "Create destination");
  std::array<char, 131072> buffer;
  while (true) {
    ssize_t count = read(input, buffer.data(), buffer.size());
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) return Fail(error, "Read file");
    if (count == 0) break;
    ssize_t pos = 0;
    while (pos < count) {
      ssize_t written = write(output, buffer.data() + pos, count - pos);
      if (written < 0 && errno == EINTR) continue;
      if (written <= 0) return Fail(error, "Write file");
      pos += written;
    }
  }
  struct stat after {};
  if (fstat(input, &after) || before.st_size != after.st_size ||
      before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
      before.st_mtim.tv_nsec != after.st_mtim.tv_nsec) {
    errno = ESTALE;
    return Fail(error, "Source changed during move");
  }
  fchown(output, before.st_uid, before.st_gid);
  fchmod(output, before.st_mode & 0777);
  const timespec times[] = {before.st_atim, before.st_mtim};
  futimens(output, times);
  if (fsync(output)) return Fail(error, "Flush file");
  return true;
}
// Check ancestry by inode, including when the same storage is exposed via two roots.
bool IsInside(int folder, int candidate) {
  struct stat root {};
  if (fstat(folder, &root)) return true;
  int current = dup(candidate);
  for (unsigned i = 0; current >= 0 && i < 256; ++i) {
    Fd child(current);
    struct stat here {}, parent_stat {};
    if (fstat(child, &here)) return true;
    if (here.st_dev == root.st_dev && here.st_ino == root.st_ino) return true;
    current = openat(child, "..", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (current < 0) return true;
    if (fstat(current, &parent_stat)) { close(current); return true; }
    if (here.st_dev == parent_stat.st_dev && here.st_ino == parent_stat.st_ino) {
      close(current);
      return false;
    }
  }
  if (current >= 0) close(current);
  return true;
}
int RenameNoReplace(int from_fd, const std::string& from, int to_fd, const std::string& to) {
  return syscall(SYS_renameat2, from_fd, from.c_str(), to_fd, to.c_str(), 1 /* RENAME_NOREPLACE */);
}
}  // namespace

Storage::Storage(const std::string& path)
    : fd_(open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)), root_(path) {}
Storage::~Storage() { if (fd_ >= 0) close(fd_); }
bool Storage::ValidName(const std::string& name) {
  return !name.empty() && name != "." && name != ".." && name.size() <= 255 &&
         name.find('/') == std::string::npos && name.find('\0') == std::string::npos;
}
int Storage::OpenDirectory(const std::string& path) const {
  int fd = dup(fd_);
  if (fd < 0 || path.empty()) return fd;
  size_t start = 0;
  while (true) {
    size_t end = path.find('/', start);
    std::string part = path.substr(start, end == std::string::npos ? end : end - start);
    if (!ValidName(part)) { close(fd); errno = EINVAL; return -1; }
    // Navigation may enter other mounts (/dev, /proc, /mnt/system from the recovery
    // root); recursive delete and copy still stop at mount points (ChildDirectory).
    int next = openat(fd, part.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    int error = errno;
    close(fd);
    if (next < 0) { errno = error; return -1; }
    fd = next;
    if (end == std::string::npos) return fd;
    start = end + 1;
  }
}
int Storage::OpenParent(const std::string& path, std::string* name) const {
  size_t slash = path.rfind('/');
  *name = slash == std::string::npos ? path : path.substr(slash + 1);
  if (!ValidName(*name) || (!path.empty() && path[0] == '/')) { errno = EINVAL; return -1; }
  return OpenDirectory(slash == std::string::npos ? "" : path.substr(0, slash));
}
bool Storage::CreateFolder(const std::string& path, std::string* error) const {
  std::string name;
  Fd parent(OpenParent(path, &name));
  if (parent < 0) return Fail(error, "Open folder");
  if (mkdirat(parent, name.c_str(), 0755)) {
    return Fail(error, errno == EEXIST ? "Name already exists" : "Create folder");
  }
  if (!SyncDir(parent)) return Fail(error, "Flush folder");
  return true;
}
bool Storage::CreateFile(const std::string& path, std::string* error) const {
  std::string name;
  Fd parent(OpenParent(path, &name));
  if (parent < 0) return Fail(error, "Open folder");
  Fd file(openat(parent, name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644));
  if (file < 0) return Fail(error, errno == EEXIST ? "Name already exists" : "Create file");
  if (fsync(file) || !SyncDir(parent)) return Fail(error, "Flush file");
  return true;
}
bool Storage::List(const std::string& path, std::vector<Entry>* entries, std::string* error) const {
  entries->clear();
  Fd dir(OpenDirectory(path));
  if (dir < 0) return Fail(error, "Open folder (storage may be locked or disconnected)");
  std::vector<std::string> names;
  if (!Names(dir, &names)) return Fail(error, "List folder");
  for (const auto& name : names) {
    Entry item{name, {}};
    if (fstatat(dir, name.c_str(), &item.info, AT_SYMLINK_NOFOLLOW)) return Fail(error, "Read entry");
    entries->push_back(std::move(item));
  }
  std::sort(entries->begin(), entries->end(), [](const Entry& a, const Entry& b) {
    if (S_ISDIR(a.info.st_mode) != S_ISDIR(b.info.st_mode)) return S_ISDIR(a.info.st_mode);
    return a.name < b.name;
  });
  return true;
}
bool Storage::Stat(const std::string& path, struct stat* info, std::string* error) const {
  std::string name;
  Fd dir(OpenParent(path, &name));
  if (dir < 0 || fstatat(dir, name.c_str(), info, AT_SYMLINK_NOFOLLOW)) return Fail(error, "Read details");
  return true;
}
bool Storage::Remove(const std::string& path, std::string* error) const {
  std::string name;
  Fd dir(OpenParent(path, &name));
  if (dir < 0) return Fail(error, "Open parent");
  if (!RemoveAt(dir, name, 0, error)) return false;
  if (!SyncDir(dir)) return Fail(error, "Flush removal");
  return true;
}
bool Storage::Move(const std::string& from, const Storage& target, const std::string& to,
                   std::string* error) const {
  std::string source_name, dest_name;
  Fd source(OpenParent(from, &source_name));
  if (source < 0) return Fail(error, "Open source parent");
  Fd dest(target.OpenParent(to, &dest_name));
  if (dest < 0) return Fail(error, "Open destination parent");
  struct stat st {};
  if (fstatat(source, source_name.c_str(), &st, AT_SYMLINK_NOFOLLOW)) return Fail(error, "Read source");
  if (S_ISDIR(st.st_mode)) {
    Fd folder(ChildDirectory(source, source_name));
    if (folder < 0) return Fail(error, "Open source folder");
    if (IsInside(folder, dest)) { errno = EINVAL; return Fail(error, "Cannot move a folder inside itself"); }
  }
  if (RenameNoReplace(source, source_name, dest, dest_name) == 0) {
    if (!SyncDir(dest) || !SyncDir(source)) return Fail(error, "Moved, but flush failed");
    return true;
  }
  if (errno != EXDEV) return Fail(error, "Move (existing names are never overwritten)");
  // Copy into a private staging directory on the destination filesystem. The
  // source is deleted only after the entire copy is flushed and committed.
  static unsigned serial = 0;
  std::string staging;
  bool created = false;
  for (int i = 0; i < 100; ++i) {
    staging = ".nasgor-move-" + std::to_string(getpid()) + "-" + std::to_string(++serial);
    if (mkdirat(dest, staging.c_str(), 0700) == 0) { created = true; break; }
    if (errno != EEXIST) return Fail(error, "Create staging folder");
  }
  if (!created) { errno = EEXIST; return Fail(error, "Create staging folder"); }
  Fd stage(ChildDirectory(dest, staging));
  bool copied = stage >= 0 ? CopyAt(source, source_name, stage, "item", 0, error)
                           : Fail(error, "Open staging folder");
  if (copied && !SyncDir(stage)) copied = Fail(error, "Flush staging folder");
  if (copied && RenameNoReplace(stage, "item", dest, dest_name)) copied = Fail(error, "Commit move");
  std::string cleanup_error;
  RemoveAt(dest, staging, 0, &cleanup_error);
  if (!copied) return false;
  if (!SyncDir(dest)) return Fail(error, "Copied; source retained because flush failed");
  if (!RemoveAt(source, source_name, 0, error)) {
    *error = "Copy completed; source removal incomplete. " + *error;
    return false;
  }
  if (!SyncDir(source)) return Fail(error, "Moved, but source flush failed");
  return true;
}
bool Storage::ReadFile(const std::string& path, size_t max_size, std::string* data,
                       std::string* error) const {
  data->clear();
  std::string name;
  Fd dir(OpenParent(path, &name));
  if (dir < 0) return Fail(error, "Open folder");
  Fd file(openat(dir, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
  if (file < 0) return Fail(error, "Open file");
  struct stat st {};
  if (fstat(file, &st)) return Fail(error, "Read file details");
  if (!S_ISREG(st.st_mode)) { errno = EINVAL; return Fail(error, "Not a regular file"); }
  if (static_cast<size_t>(st.st_size) > max_size) {
    *error = "File is too large (limit " + std::to_string(max_size / 1024) + " KB)";
    return false;
  }
  std::array<char, 65536> buffer;
  while (true) {
    ssize_t count = read(file, buffer.data(), buffer.size());
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) return Fail(error, "Read file");
    if (count == 0) break;
    data->append(buffer.data(), count);
    if (data->size() > max_size) { errno = EFBIG; return Fail(error, "File grew while reading"); }
  }
  return true;
}

bool Storage::WriteFileAtomic(const std::string& path, const std::string& data,
                              std::string* error) const {
  std::string name;
  Fd dir(OpenParent(path, &name));
  if (dir < 0) return Fail(error, "Open folder");
  Fd original(openat(dir, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
  if (original < 0) return Fail(error, "Open file");
  struct stat st {};
  if (fstat(original, &st)) return Fail(error, "Read file details");
  if (!S_ISREG(st.st_mode)) { errno = EINVAL; return Fail(error, "Not a regular file"); }
  // Keep the SELinux label: files in /system without it can stop the OS from booting.
  std::string label;
  ssize_t label_size = fgetxattr(original, "security.selinux", nullptr, 0);
  if (label_size > 0) {
    label.resize(label_size);
    label_size = fgetxattr(original, "security.selinux", label.data(), label.size());
    if (label_size < 0) return Fail(error, "Read SELinux label");
    label.resize(label_size);
  } else if (label_size < 0 && errno != ENODATA && errno != ENOTSUP) {
    return Fail(error, "Read SELinux label");
  }

  static unsigned serial = 0;
  std::string temp;
  int temp_fd = -1;
  for (int i = 0; i < 100 && temp_fd < 0; ++i) {
    temp = ".nasgor-edit-" + std::to_string(getpid()) + "-" + std::to_string(++serial);
    temp_fd = openat(dir, temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (temp_fd < 0 && errno != EEXIST) return Fail(error, "Create temporary file");
  }
  if (temp_fd < 0) { errno = EEXIST; return Fail(error, "Create temporary file"); }
  Fd output(temp_fd);
  auto abort_write = [&](const std::string& action) {
    bool result = Fail(error, action);
    unlinkat(dir, temp.c_str(), 0);
    return result;
  };
  size_t done = 0;
  while (done < data.size()) {
    ssize_t written = write(output, data.data() + done, data.size() - done);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) return abort_write("Write file");
    done += written;
  }
  if (fchown(output, st.st_uid, st.st_gid)) return abort_write("Keep owner");
  if (fchmod(output, st.st_mode & 07777)) return abort_write("Keep permissions");
  if (!label.empty() &&
      fsetxattr(output, "security.selinux", label.data(), label.size(), 0)) {
    return abort_write("Keep SELinux label");
  }
  if (fsync(output)) return abort_write("Flush file");
  if (renameat(dir, temp.c_str(), dir, name.c_str())) return abort_write("Replace file");
  if (!SyncDir(dir)) return Fail(error, "Saved, but flushing the folder failed");
  return true;
}

int Storage::OpenRead(const std::string& path, struct stat* info, std::string* error) const {
  std::string name;
  Fd dir(OpenParent(path, &name));
  if (dir < 0) { Fail(error, "Open folder"); return -1; }
  int fd = openat(dir, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) { Fail(error, "Open file"); return -1; }
  if (fstat(fd, info) || !S_ISREG(info->st_mode)) {
    errno = EINVAL;
    Fail(error, "Not a regular file");
    close(fd);
    return -1;
  }
  return fd;
}

std::string DisplayName(const std::string& name) {
  std::string result;
  for (unsigned char c : name) {
    if (c < 32 || c == 127) result += "?";
    else result += static_cast<char>(c);
  }
  return result;
}
}  // namespace recovery::files
