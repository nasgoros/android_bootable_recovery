// Copyright (C) 2026 The nasgorOS Project
// SPDX-License-Identifier: Apache-2.0
// Standalone host test (from the repository root):
//   g++ -std=c++17 -Wall -Wextra -Werror file_manager/flash_image.cpp
//       file_manager/flash_image_test.cpp -o /tmp/flash-image-test
#include "flash_image.h"

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include <cassert>
#include <fstream>
#include <iostream>
#include <string>

using namespace recovery::flash;

static std::string Temp() {
  std::string path = "/tmp/nasgor-flash-XXXXXX";
  int fd = mkstemp(path.data());
  assert(fd >= 0);
  close(fd);
  return path;
}

static void WriteFile(const std::string& path, const std::string& data) {
  std::ofstream(path, std::ios::binary | std::ios::trunc) << data;
}

static std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), {});
}

int main() {
  std::string error;
  // Partition names map to image formats (A/B suffixes ignored).
  assert(KindForPartition("boot") == ImageKind::kBoot);
  assert(KindForPartition("recovery") == ImageKind::kBoot);
  assert(KindForPartition("boot_b") == ImageKind::kBoot);
  assert(KindForPartition("vendor_boot_a") == ImageKind::kVendorBoot);
  assert(KindForPartition("dtbo") == ImageKind::kDtbo);
  assert(KindForPartition("vbmeta_system") == ImageKind::kVbmeta);
  assert(KindForPartition("userdata") == ImageKind::kUnknown);
  // Header checks.
  assert(CheckHeader(ImageKind::kBoot, "ANDROID!rest", &error));
  assert(!CheckHeader(ImageKind::kBoot, "AVB0....", &error) && !error.empty());
  assert(CheckHeader(ImageKind::kVbmeta, "AVB0....", &error));
  assert(CheckHeader(ImageKind::kDtbo, std::string("\xd7\xb7\xab\x1e\0\0\0\0", 8), &error));
  assert(!CheckHeader(ImageKind::kDtbo, "ANDROID!", &error));
  assert(CheckHeader(ImageKind::kVendorBoot, "VNDRBOOT", &error));
  assert(!CheckHeader(ImageKind::kUnknown, "ANDROID!", &error));

  // Flash a 2.5 MB image into a 4 MB "partition": written, verified, tail kept.
  const std::string partition = Temp(), image = Temp();
  std::string old_partition(4 * 1024 * 1024, 'o');
  WriteFile(partition, old_partition);
  std::string data = "ANDROID!";
  while (data.size() < 2 * 1024 * 1024 + 512 * 1024) data += static_cast<char>('a' + data.size() % 26);
  WriteFile(image, data);
  assert(DeviceSize(partition, &error) == static_cast<int64_t>(old_partition.size()));
  int image_fd = open(image.c_str(), O_RDONLY);
  assert(image_fd >= 0);
  int last = -1;
  bool monotonic = true;
  assert(Flash(image_fd, data.size(), partition, [&](int percent) {
    if (percent < last) monotonic = false;
    last = percent;
  }, &error));
  assert(last == 100 && monotonic);
  std::string after = ReadFile(partition);
  assert(after.size() == old_partition.size());
  assert(after.compare(0, data.size(), data) == 0);
  assert(after.compare(data.size(), std::string::npos, old_partition, data.size(), std::string::npos) == 0);

  // Too large: refused before touching the partition.
  const std::string small = Temp();
  WriteFile(small, std::string(1024, 's'));
  assert(!Flash(image_fd, data.size(), small, nullptr, &error));
  assert(error.find("does not fit") != std::string::npos);
  assert(ReadFile(small) == std::string(1024, 's'));
  // Missing partition.
  assert(!Flash(image_fd, data.size(), "/nonexistent/partition", nullptr, &error));
  close(image_fd);
  unlink(partition.c_str());
  unlink(image.c_str());
  unlink(small.c_str());
  std::cout << "PASS: partition kinds, image headers, size checks, flash + verify, tail kept\n";
  return 0;
}
