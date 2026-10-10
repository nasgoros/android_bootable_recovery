// Copyright (C) 2026 The nasgorOS Project
// SPDX-License-Identifier: Apache-2.0
// Standalone host test (from the repository root; needs zlib, python3 and unzip):
//   g++ -std=c++17 -Wall -Wextra -Werror file_manager/zip_create.cpp
//       file_manager/zip_create_test.cpp -lz -o /tmp/zip-create-test
// NASGOR_ZIP_BIG=1 also writes a sparse file over 4 GB (slow, zip64 sizes/offsets).
#include "zip_create.h"

#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace recovery::zip;
namespace fs = std::filesystem;

static void Write(const std::string& path, const std::string& data) { std::ofstream(path) << data; }

static int Run(const std::string& command) {
  int status = system(command.c_str());
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

struct FakeSink : Sink {
  std::vector<std::string> names;
  bool AddDirectory(const std::string& name, time_t, mode_t, std::string*) override {
    names.push_back(name);
    return true;
  }
  bool AddFile(const std::string& name, int fd, uint64_t, time_t, mode_t, std::string*) override {
    char c;
    while (read(fd, &c, 1) > 0) {}
    names.push_back(name);
    return true;
  }
};

int main() {
  std::string error;
  // Entry names.
  assert(EntryName("/sdcard/a.txt") == "sdcard/a.txt");
  assert(EntryName("./x//y/") == "x/y");
  assert(EntryName("a/./b") == "a/b");
  assert(EntryName("../x").empty() && EntryName("a/../b").empty());
  assert(EntryName("").empty() && EntryName(".").empty() && EntryName("/").empty());

  std::string root = "/tmp/nasgor-zip-XXXXXX";
  assert(mkdtemp(root.data()));
  fs::create_directories(root + "/src/sub/deep");
  Write(root + "/src/a.txt", "hello\n");
  Write(root + "/src/sub/b.bin", std::string(100000, 'x'));
  Write(root + "/src/sub/deep/nama é.txt", "utf8");
  Write(root + "/src/empty.txt", "");
  Write(root + "/outside.txt", "outside");
  fs::create_symlink(root + "/outside.txt", root + "/src/link.txt");
  fs::create_directory_symlink(root, root + "/src/loop");
  assert(mkfifo((root + "/src/fifo").c_str(), 0600) == 0);
  chmod((root + "/src/a.txt").c_str(), 0755);

  {  // Walking: sorted, recursive, symlinked folder and fifo skipped, file symlink followed.
    FakeSink sink;
    Builder builder(&sink, 0, 0);
    std::vector<std::string> skipped;
    builder.warn = [&](const std::string& path, const std::string&) { skipped.push_back(path); };
    assert(builder.Add(root + "/src", "src", true, &error));
    const std::vector<std::string> expect{"src/", "src/a.txt", "src/empty.txt", "src/link.txt",
                                          "src/sub/", "src/sub/b.bin", "src/sub/deep/",
                                          "src/sub/deep/nama é.txt"};
    assert(sink.names == expect);
    assert(skipped.size() == 2 && builder.stats().skipped == 2 && builder.stats().files == 5);
    // Same entry again is a duplicate; folder without -r adds only the folder entry.
    assert(builder.Add(root + "/src/a.txt", "src/a.txt", false, &error));
    assert(builder.stats().skipped == 3);
    FakeSink flat;
    Builder single(&flat, 0, 0);
    assert(single.Add(root + "/src", "src", false, &error) && flat.names.size() == 1);
  }

  {  // Real archive, checked by unzip and python's zipfile.
    const std::string zip = root + "/out.zip";
    Stats stats;
    assert(CreateZip(zip, {{root + "/src", "src"}, {root + "/outside.txt", "top.txt"}}, true, false,
                     {}, &stats, &error));
    assert(stats.files == 6 && stats.folders == 3);
    assert(Run("unzip -tq '" + zip + "' >/dev/null") == 0);
    assert(Run("python3 - '" + zip + "' <<'EOF'\n"
               "import sys, zipfile\n"
               "z = zipfile.ZipFile(sys.argv[1])\n"
               "assert z.testzip() is None\n"
               "assert z.read('src/a.txt') == b'hello\\n'\n"
               "assert z.read('src/sub/b.bin') == b'x' * 100000\n"
               "assert z.read('src/sub/deep/nama é.txt') == b'utf8'\n"
               "assert z.read('src/link.txt') == b'outside'\n"
               "assert z.read('top.txt') == b'outside'\n"
               "assert (z.getinfo('src/a.txt').external_attr >> 16) & 0o777 == 0o755\n"
               "assert z.getinfo('src/sub/b.bin').compress_type == zipfile.ZIP_DEFLATED\n"
               "assert z.getinfo('src/sub/').is_dir()\n"
               "EOF") == 0);
    // Never replaces an existing file; temporary files are cleaned up.
    assert(!CreateZip(zip, {{root + "/outside.txt", "x"}}, true, false, {}, nullptr, &error));
    assert(!CreateZip(root + "/none.zip", {{root + "/missing", "m"}}, true, false, {}, nullptr,
                      &error) && error == "Nothing to add" && !fs::exists(root + "/none.zip"));
    for (const auto& e : fs::directory_iterator(root)) {
      assert(e.path().filename().string().find(".nasgor-zip-") != 0);
    }
    // Store mode.
    const std::string stored = root + "/stored.zip";
    assert(CreateZip(stored, {{root + "/src/sub/b.bin", "b.bin"}}, false, true, {}, nullptr, &error));
    assert(Run("python3 -c \"import zipfile; z = zipfile.ZipFile('" + stored + "'); "
               "i = z.getinfo('b.bin'); assert i.compress_type == zipfile.ZIP_STORED; "
               "assert z.read('b.bin') == b'x' * 100000\"") == 0);
  }

  {  // zip64 end records: more than 65535 entries.
    const std::string zip = root + "/many.zip";
    int fd = open(zip.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    assert(fd >= 0);
    ArchiveWriter writer(fd, true);
    for (int i = 0; i < 70000; ++i) {
      int empty = open("/dev/null", O_RDONLY);
      assert(writer.AddFile("f" + std::to_string(i), empty, 0, 0, 0644, &error));
      close(empty);
    }
    assert(writer.Finish(&error));
    close(fd);
    assert(Run("python3 -c \"import zipfile; z = zipfile.ZipFile('" + zip + "'); "
               "assert len(z.infolist()) == 70000; assert z.testzip() is None\"") == 0);
    assert(Run("unzip -tq '" + zip + "' >/dev/null") == 0);
  }

  if (getenv("NASGOR_ZIP_BIG")) {  // zip64 sizes and offsets: a sparse 4.1 GB file.
    const std::string big = root + "/big.img";
    const off_t size = 4100LL * 1024 * 1024;
    int fd = open(big.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    assert(fd >= 0 && ftruncate(fd, size) == 0 && pwrite(fd, "end", 3, size - 3) == 3);
    close(fd);
    const std::string zip = root + "/big.zip";
    assert(CreateZip(zip, {{big, "big.img"}, {root + "/src/a.txt", "after.txt"}}, false, false, {},
                     nullptr, &error));
    assert(Run("python3 - '" + zip + "' <<'EOF'\n"
               "import sys, zipfile\n"
               "z = zipfile.ZipFile(sys.argv[1])\n"
               "i = z.getinfo('big.img')\n"
               "assert i.file_size == 4100 * 1024 * 1024\n"
               "with z.open('big.img') as f:\n"
               "    f.seek(i.file_size - 3); assert f.read() == b'end'\n"
               "assert z.read('after.txt') == b'hello\\n'\n"
               "EOF") == 0);
    std::cout << "zip64 big file: OK\n";
  }

  fs::remove_all(root);
  std::cout << "PASS: entry names, walking (symlinks, special files, duplicates), deflate/store, "
               "permissions, UTF-8 names, no overwrite, zip64 entry count\n";
  return 0;
}
