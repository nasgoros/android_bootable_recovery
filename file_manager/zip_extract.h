// Copyright (C) 2026 The nasgorOS Project
// SPDX-License-Identifier: Apache-2.0
//
// Extracting zip archives (including zip64) with libziparchive for the file manager.
#pragma once

#include <string>

#include "zip_create.h"

namespace recovery::zip {

// Extracts `archive` into the new folder `dest` (which must not exist yet). Entries
// with absolute names, ".." or backslashes and symlink entries are skipped, so nothing
// outside `dest` is written; existing files are never replaced.
bool ExtractZip(const std::string& archive, const std::string& dest, const Callbacks& callbacks,
                Stats* stats, std::string* error);

}  // namespace recovery::zip
