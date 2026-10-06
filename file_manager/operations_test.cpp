// Copyright (C) 2026 The nasgorOS Project
// SPDX-License-Identifier: Apache-2.0
// Standalone host regression test: g++ -std=c++17 operations.cpp operations_test.cpp -o /tmp/file-manager-test
#include "operations.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>
using recovery::files::Storage;
namespace fs = std::filesystem;
static std::string Temp(const char* pattern) {
  std::string path(pattern);
  assert(mkdtemp(path.data()));
  return path;
}
static void Write(const std::string& path, const std::string& data) { std::ofstream(path) << data; }
static std::string Read(const std::string& path) {
  std::ifstream in(path);
  return std::string(std::istreambuf_iterator<char>(in), {});
}
int main() {
  std::string root = Temp("/tmp/nasgor-files-XXXXXX");
  std::string external = Temp("/tmp/nasgor-outside-XXXXXX");
  std::string cross = Temp("/dev/shm/nasgor-files-XXXXXX");
  Storage a(root), b(cross);
  std::string error;
  std::vector<recovery::files::Entry> entries;
  assert(a.valid() && b.valid());
  Write(root + "/nama spasi-é.txt", "payload");
  assert(a.Move("nama spasi-é.txt", a, "baru.txt", &error));
  assert(Read(root + "/baru.txt") == "payload");
  Write(root + "/existing.txt", "keep");
  assert(!a.Move("baru.txt", a, "existing.txt", &error));
  assert(Read(root + "/existing.txt") == "keep");
  assert(fs::exists(root + "/baru.txt"));
  assert(!a.Remove("", &error));
  assert(!a.Remove("../", &error));
  assert(!a.Remove("/baru.txt", &error));
  fs::create_directories(root + "/folder/sub");
  Write(root + "/folder/sub/file", std::string(300000, 'x'));
  Write(external + "/protected", "outside");
  fs::create_directory_symlink(external, root + "/link");
  assert(!a.Remove("link/protected", &error));
  assert(!a.List("link", &entries, &error));
  assert(Read(external + "/protected") == "outside");
  assert(!a.Move("folder", a, "folder/sub/nested", &error));
  Storage alias(root + "/folder/sub");
  assert(!a.Move("folder", alias, "nested", &error));
  assert(a.List("", &entries, &error));
  auto count = entries.size();
  assert(a.List("", &entries, &error) && entries.size() == count);
  assert(a.Move("folder", b, "moved", &error));
  assert(!fs::exists(root + "/folder"));
  assert(Read(cross + "/moved/sub/file") == std::string(300000, 'x'));
  assert(b.Remove("moved", &error));
  assert(!fs::exists(cross + "/moved"));
  fs::create_directories(root + "/tree");
  fs::create_directory_symlink(external, root + "/tree/link");
  assert(a.Remove("tree", &error));
  assert(Read(external + "/protected") == "outside");
  Write(cross + "/occupied", "original");
  assert(!a.Move("baru.txt", b, "occupied", &error));
  assert(Read(root + "/baru.txt") == "payload");
  assert(Read(cross + "/occupied") == "original");
  for (const auto& e : fs::directory_iterator(cross)) assert(e.path().filename().string().find(".nasgor-move-") != 0);
  // A cross-volume failure must not delete source data or leave staged partial copies.
  fs::create_directory(root + "/unsupported");
  Write(root + "/unsupported/regular", "keep this");
  assert(mkfifo((root + "/unsupported/fifo").c_str(), 0600) == 0);
  assert(!a.Move("unsupported", b, "unsupported", &error));
  assert(Read(root + "/unsupported/regular") == "keep this");
  assert(!fs::exists(cross + "/unsupported"));
  for (const auto& e : fs::directory_iterator(cross)) assert(e.path().filename().string().find(".nasgor-move-") != 0);
  // Copy symbolic links as links; do not traverse their external targets.
  fs::create_directory(root + "/linked-tree");
  fs::create_directory_symlink(external, root + "/linked-tree/external");
  assert(a.Move("linked-tree", b, "linked-tree", &error));
  assert(fs::is_symlink(cross + "/linked-tree/external"));
  assert(b.Remove("linked-tree", &error));
  assert(Read(external + "/protected") == "outside");
  assert(!a.Move("baru.txt", b, "../escape", &error));
  assert(!a.Move("baru.txt", a, "link/escape", &error));
  assert(!Storage::ValidName(std::string("x\0y", 3)));
  assert(!Storage::ValidName(std::string(256, 'x')));
  assert(recovery::files::DisplayName("a\nb") == "a?b");
  assert(a.Remove("link", &error));
  assert(Read(external + "/protected") == "outside");
  fs::remove_all(root);
  fs::remove_all(external);
  fs::remove_all(cross);
  std::cout << "PASS: rename, recursive delete, cross-filesystem move, collisions, symlinks, traversal, ancestry, Unicode\n";
}
