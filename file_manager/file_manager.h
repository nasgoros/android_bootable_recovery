// Copyright (C) 2026 The nasgorOS Project
// SPDX-License-Identifier: Apache-2.0
#pragma once
class Device;
void RunFileManager(Device* device);
// Mount system/product/vendor/... read-only or read-write below /mnt.
void RunPartitionMenu(Device* device);
