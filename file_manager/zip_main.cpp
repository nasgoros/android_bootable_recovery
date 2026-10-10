// Copyright (C) 2026 The nasgorOS Project
// SPDX-License-Identifier: Apache-2.0
//
// `zip` for the recovery terminal: zip [-r] [-0] [-q] archive[.zip] path...
// Creates a new archive (zip64 when needed); an existing archive is never changed.
// Exit codes follow Info-ZIP: 0 ok, 12 nothing to add, 16 bad arguments,
// 18 some paths skipped (archive written), 1 other errors.

#include <stdio.h>
#include <string.h>

#include <string>
#include <utility>
#include <vector>

#include "zip_create.h"

namespace {

void Usage(FILE* out) {
  fprintf(out,
          "usage: zip [-r] [-0] [-q] archive[.zip] path...\n"
          "  -r  add folders with their contents\n"
          "  -0  store only (no compression)\n"
          "  -q  quiet\n"
          "Creates a new archive; an existing file is never replaced. Symlinks to files\n"
          "are followed, symlinked folders and special files are skipped. Archives and\n"
          "files over 4 GB use zip64.\n");
}

}  // namespace

int main(int argc, char** argv) {
  bool recursive = false, store = false, quiet = false;
  std::vector<std::string> args;
  bool options_done = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (!options_done && arg == "--") {
      options_done = true;
    } else if (!options_done && (arg == "-h" || arg == "--help")) {
      Usage(stdout);
      return 0;
    } else if (!options_done && arg.size() > 1 && arg[0] == '-') {
      for (size_t j = 1; j < arg.size(); ++j) {
        switch (arg[j]) {
          case 'r': recursive = true; break;
          case '0': store = true; break;
          case 'q': quiet = true; break;
          default:
            fprintf(stderr, "zip: unknown option -%c\n", arg[j]);
            Usage(stderr);
            return 16;
        }
      }
    } else {
      args.push_back(arg);
    }
  }
  if (args.size() < 2) {
    Usage(stderr);
    return 16;
  }

  std::string archive = args[0];
  const size_t slash = archive.rfind('/');
  const std::string base = slash == std::string::npos ? archive : archive.substr(slash + 1);
  if (base.find('.') == std::string::npos) archive += ".zip";

  std::vector<std::pair<std::string, std::string>> inputs;
  for (size_t i = 1; i < args.size(); ++i) {
    std::string name = recovery::zip::EntryName(args[i]);
    if (name.empty() && args[i].find_first_not_of('/') != std::string::npos) {
      fprintf(stderr, "zip: %s: paths with .. are not supported\n", args[i].c_str());
      return 16;
    }
    inputs.emplace_back(args[i], name);
  }

  recovery::zip::Callbacks callbacks;
  if (!quiet) {
    callbacks.progress = [](const std::string& name) { printf("  adding: %s\n", name.c_str()); };
  }
  callbacks.warn = [](const std::string& path, const std::string& reason) {
    fprintf(stderr, "zip: skipped %s (%s)\n", path.c_str(), reason.c_str());
  };
  recovery::zip::Stats stats;
  std::string error;
  if (!recovery::zip::CreateZip(archive, inputs, recursive, store, callbacks, &stats, &error)) {
    fprintf(stderr, "zip: %s\n", error.c_str());
    return error == "Nothing to add" ? 12 : 1;
  }
  if (!quiet) {
    printf("%s: %zu files, %zu folders, %llu bytes\n", archive.c_str(), stats.files, stats.folders,
           stats.bytes);
  }
  return stats.skipped > 0 ? 18 : 0;
}
