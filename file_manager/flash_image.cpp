// Copyright (C) 2026 The nasgorOS Project
// SPDX-License-Identifier: Apache-2.0

#include "flash_image.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <memory>
#include <vector>

namespace recovery::flash {
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

bool StartsWith(const std::string& text, const std::string& prefix) {
  return text.compare(0, prefix.size(), prefix) == 0;
}

// Full pread/pwrite, retrying short transfers and EINTR.
bool ReadAt(int fd, char* buf, size_t size, uint64_t offset) {
  size_t done = 0;
  while (done < size) {
    ssize_t n = pread(fd, buf + done, size - done, static_cast<off_t>(offset + done));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) {
      if (n == 0) errno = EIO;
      return false;
    }
    done += n;
  }
  return true;
}

bool WriteAt(int fd, const char* buf, size_t size, uint64_t offset) {
  size_t done = 0;
  while (done < size) {
    ssize_t n = pwrite(fd, buf + done, size - done, static_cast<off_t>(offset + done));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) {
      if (n == 0) errno = EIO;
      return false;
    }
    done += n;
  }
  return true;
}

constexpr size_t kChunk = 1024 * 1024;

}  // namespace

ImageKind KindForPartition(const std::string& name) {
  std::string base = name;
  if (base.size() > 2 && (base.compare(base.size() - 2, 2, "_a") == 0 ||
                          base.compare(base.size() - 2, 2, "_b") == 0)) {
    base.resize(base.size() - 2);
  }
  if (base == "boot" || base == "recovery" || base == "init_boot") return ImageKind::kBoot;
  if (base == "vendor_boot") return ImageKind::kVendorBoot;
  if (base == "dtbo") return ImageKind::kDtbo;
  if (StartsWith(base, "vbmeta")) return ImageKind::kVbmeta;
  return ImageKind::kUnknown;
}

const char* KindName(ImageKind kind) {
  switch (kind) {
    case ImageKind::kBoot: return "Android boot image";
    case ImageKind::kVendorBoot: return "vendor boot image";
    case ImageKind::kDtbo: return "DTBO image";
    case ImageKind::kVbmeta: return "vbmeta image";
    default: return "image";
  }
}

bool CheckHeader(ImageKind kind, const std::string& header, std::string* error) {
  bool ok = false;
  switch (kind) {
    case ImageKind::kBoot:
      ok = StartsWith(header, "ANDROID!");
      break;
    case ImageKind::kVendorBoot:
      ok = StartsWith(header, "VNDRBOOT");
      break;
    case ImageKind::kDtbo:
      // dt_table_header.magic, big-endian 0xd7b7ab1e
      ok = header.size() >= 4 && static_cast<unsigned char>(header[0]) == 0xd7 &&
           static_cast<unsigned char>(header[1]) == 0xb7 &&
           static_cast<unsigned char>(header[2]) == 0xab &&
           static_cast<unsigned char>(header[3]) == 0x1e;
      break;
    case ImageKind::kVbmeta:
      ok = StartsWith(header, "AVB0");
      break;
    case ImageKind::kUnknown:
      *error = "Flashing this partition is not supported.";
      return false;
  }
  if (!ok) *error = std::string("This file is not a ") + KindName(kind) + ".";
  return ok;
}

int64_t DeviceSize(const std::string& block_device, std::string* error) {
  Fd fd(open(block_device.c_str(), O_RDONLY | O_CLOEXEC));
  if (fd < 0) {
    Fail(error, "Open " + block_device);
    return -1;
  }
  off_t size = lseek(fd, 0, SEEK_END);  // works for block devices and files
  if (size < 0) {
    Fail(error, "Read partition size");
    return -1;
  }
  return size;
}

bool Flash(int image_fd, uint64_t image_size, const std::string& block_device,
           const std::function<void(int percent)>& progress, std::string* error) {
  std::string size_error;
  const int64_t partition_size = DeviceSize(block_device, &size_error);
  if (partition_size < 0) {
    *error = size_error;
    return false;
  }
  if (image_size == 0 || image_size > static_cast<uint64_t>(partition_size)) {
    *error = "Image size (" + std::to_string(image_size) + " bytes) does not fit the partition (" +
             std::to_string(partition_size) + " bytes).";
    return false;
  }
  Fd device(open(block_device.c_str(), O_WRONLY | O_CLOEXEC));
  if (device < 0) return Fail(error, "Open " + block_device + " for writing");

  std::vector<char> buffer(kChunk);
  bool cleared_read_only = false;
  for (uint64_t offset = 0; offset < image_size; offset += kChunk) {
    const size_t size = static_cast<size_t>(std::min<uint64_t>(kChunk, image_size - offset));
    if (!ReadAt(image_fd, buffer.data(), size, offset)) return Fail(error, "Read image");
    if (!WriteAt(device, buffer.data(), size, offset)) {
      // Some kernels mark boot partitions read-only; clear the flag once and retry.
      if ((errno == EPERM || errno == EROFS) && !cleared_read_only) {
        int read_only = 0;
        cleared_read_only = true;
        if (ioctl(device, BLKROSET, &read_only) == 0 &&
            WriteAt(device, buffer.data(), size, offset)) {
          if (progress) progress(static_cast<int>((offset + size) * 90 / image_size));
          continue;
        }
      }
      return Fail(error, "Write partition (it may now be incomplete)");
    }
    if (progress) progress(static_cast<int>((offset + size) * 90 / image_size));
  }
  if (fsync(device)) return Fail(error, "Flush partition");

  // Verify from the device, not from the page cache.
  Fd verify(open(block_device.c_str(), O_RDONLY | O_CLOEXEC));
  if (verify < 0) return Fail(error, "Open partition for verification");
  ioctl(verify, BLKFLSBUF, 0);  // ignored for regular files (tests)
  posix_fadvise(verify, 0, 0, POSIX_FADV_DONTNEED);
  std::vector<char> written(kChunk);
  for (uint64_t offset = 0; offset < image_size; offset += kChunk) {
    const size_t size = static_cast<size_t>(std::min<uint64_t>(kChunk, image_size - offset));
    if (!ReadAt(image_fd, buffer.data(), size, offset)) return Fail(error, "Read image");
    if (!ReadAt(verify, written.data(), size, offset)) return Fail(error, "Read back partition");
    if (memcmp(buffer.data(), written.data(), size) != 0) {
      *error = "Verification failed: the partition does not match the image.";
      return false;
    }
    if (progress) progress(90 + static_cast<int>((offset + size) * 10 / image_size));
  }
  return true;
}

}  // namespace recovery::flash
