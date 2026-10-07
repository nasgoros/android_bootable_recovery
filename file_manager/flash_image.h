// Copyright (C) 2026 The nasgorOS Project
// SPDX-License-Identifier: Apache-2.0
//
// Flashing raw partition images (boot.img, recovery.img, dtbo.img, vbmeta.img)
// from the recovery file manager. The image is checked against the partition type
// and size, written in chunks, flushed and read back for verification.
#pragma once

#include <stdint.h>

#include <functional>
#include <string>

namespace recovery::flash {

enum class ImageKind { kBoot, kVendorBoot, kDtbo, kVbmeta, kUnknown };

// Image format expected in a partition, from its name without slot suffix
// ("boot", "recovery", "init_boot", "vendor_boot", "dtbo", "vbmeta*").
ImageKind KindForPartition(const std::string& name);
const char* KindName(ImageKind kind);

// Checks the first bytes of an image for the expected format. `header` should hold
// at least the first 8 bytes of the image.
bool CheckHeader(ImageKind kind, const std::string& header, std::string* error);

// Size in bytes of a block device (or regular file, for tests); -1 on error.
int64_t DeviceSize(const std::string& block_device, std::string* error);

// Writes image_size bytes from image_fd (at offset 0) to block_device, flushes,
// and reads back to verify. progress() receives 0..100. The rest of the partition
// beyond the image is left as it is (like fastboot).
bool Flash(int image_fd, uint64_t image_size, const std::string& block_device,
           const std::function<void(int percent)>& progress, std::string* error);

}  // namespace recovery::flash
