// Copyright (C) 2026 The nasgorOS Project
// SPDX-License-Identifier: Apache-2.0

#include "zip_create.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <memory>

#include <zlib.h>

namespace recovery::zip {

std::string EntryName(const std::string& path) {
  std::string name;
  size_t start = 0;
  while (start <= path.size()) {
    size_t end = path.find('/', start);
    if (end == std::string::npos) end = path.size();
    const std::string part = path.substr(start, end - start);
    if (part == "..") return "";
    if (!part.empty() && part != ".") {
      if (!name.empty()) name += '/';
      name += part;
    }
    start = end + 1;
  }
  return name;
}

void Builder::Skip(const std::string& path, const std::string& reason) {
  ++stats_.skipped;
  if (warn) warn(path, reason);
}

bool Builder::Add(const std::string& path, const std::string& name, bool recursive,
                  std::string* error) {
  return AddAt(path, name, recursive, 0, error);
}

bool Builder::AddAt(const std::string& path, const std::string& name, bool recursive, int depth,
                    std::string* error) {
  if (depth > 128) {
    Skip(path, "folder nesting too deep");
    return true;
  }
  struct stat st {};
  if (lstat(path.c_str(), &st) != 0) {
    Skip(path, strerror(errno));
    return true;
  }
  if (S_ISLNK(st.st_mode)) {
    if (stat(path.c_str(), &st) != 0) {
      Skip(path, "broken symlink");
      return true;
    }
    if (!S_ISREG(st.st_mode)) {
      Skip(path, S_ISDIR(st.st_mode) ? "symlinked folder" : "symlink to a special file");
      return true;
    }
  }

  if (S_ISREG(st.st_mode)) {
    if (st.st_dev == skip_dev_ && st.st_ino == skip_ino_) {
      Skip(path, "the archive being written");
      return true;
    }
    if (name.empty() || !names_.insert(name).second) {
      Skip(path, "duplicate entry name");
      return true;
    }
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
      Skip(path, strerror(errno));
      return true;
    }
    struct stat opened {};
    bool ok = fstat(fd, &opened) == 0 && S_ISREG(opened.st_mode);
    if (ok) {
      ok = sink_->AddFile(name, fd, static_cast<uint64_t>(opened.st_size), opened.st_mtime,
                          opened.st_mode, error);
    }
    close(fd);
    if (!ok) {
      if (error->empty()) *error = path + ": not a regular file";
      return false;
    }
    ++stats_.files;
    stats_.bytes += static_cast<unsigned long long>(opened.st_size);
    if (added) added(name);
    return true;
  }

  if (!S_ISDIR(st.st_mode)) {
    Skip(path, "special file");
    return true;
  }
  if (!name.empty()) {
    if (!names_.insert(name + "/").second) {
      Skip(path, "duplicate entry name");
      return true;
    }
    if (!sink_->AddDirectory(name + "/", st.st_mtime, st.st_mode, error)) return false;
    ++stats_.folders;
  }
  if (!recursive) return true;

  struct CloseDir {
    void operator()(DIR* d) const { closedir(d); }
  };
  std::unique_ptr<DIR, CloseDir> dir(opendir(path.c_str()));
  if (!dir) {
    Skip(path, strerror(errno));
    return true;
  }
  std::vector<std::string> children;
  while (auto* entry = readdir(dir.get())) {
    std::string child(entry->d_name);
    if (child != "." && child != "..") children.push_back(std::move(child));
  }
  std::sort(children.begin(), children.end());
  const std::string prefix = path.empty() || path.back() == '/' ? path : path + "/";
  for (const auto& child : children) {
    if (!AddAt(prefix + child, name.empty() ? child : name + "/" + child, true, depth + 1, error)) {
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// ArchiveWriter

namespace {

constexpr uint32_t kMax32 = 0xffffffffu;
constexpr uint16_t kVersionZip64 = 45;
constexpr uint16_t kVersionDeflate = 20;
constexpr uint16_t kMadeByUnix = 3 << 8;

void Put16(std::string* out, uint16_t value) {
  out->push_back(static_cast<char>(value & 0xff));
  out->push_back(static_cast<char>(value >> 8));
}
void Put32(std::string* out, uint32_t value) {
  for (int i = 0; i < 4; ++i) out->push_back(static_cast<char>((value >> (8 * i)) & 0xff));
}
void Put64(std::string* out, uint64_t value) {
  for (int i = 0; i < 8; ++i) out->push_back(static_cast<char>((value >> (8 * i)) & 0xff));
}
uint32_t Clamp32(uint64_t value) { return value >= kMax32 ? kMax32 : static_cast<uint32_t>(value); }

void DosTime(time_t mtime, uint16_t* time, uint16_t* date) {
  struct tm tm {};
  localtime_r(&mtime, &tm);
  if (tm.tm_year < 80) {  // DOS dates start in 1980
    *time = 0;
    *date = (1 << 5) | 1;
    return;
  }
  *time = static_cast<uint16_t>((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
  *date = static_cast<uint16_t>(((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
}

uint16_t NameFlags(const std::string& name) {
  // Bit 11: the name is UTF-8.
  for (unsigned char c : name) if (c >= 0x80) return 1 << 11;
  return 0;
}

std::string Errno(const std::string& action) { return action + ": " + strerror(errno); }

// Files near 4 GB reserve zip64 fields in the local header; compressed data can be
// slightly larger than the input.
constexpr uint64_t kZip64Threshold = 0xffff0000ull;

}  // namespace

bool ArchiveWriter::Write(const std::string& data, std::string* error) {
  size_t done = 0;
  while (done < data.size()) {
    ssize_t n = pwrite(fd_, data.data() + done, data.size() - done,
                       static_cast<off_t>(offset_ + done));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) {
      *error = Errno("Write archive");
      return false;
    }
    done += static_cast<size_t>(n);
  }
  offset_ += data.size();
  return true;
}

bool ArchiveWriter::PatchHeader(const Entry& entry, bool zip64, std::string* error) {
  std::string fields;
  Put32(&fields, entry.crc);
  Put32(&fields, zip64 ? kMax32 : static_cast<uint32_t>(entry.compressed));
  Put32(&fields, zip64 ? kMax32 : static_cast<uint32_t>(entry.uncompressed));
  std::string extra;
  if (zip64) {
    Put64(&extra, entry.uncompressed);
    Put64(&extra, entry.compressed);
  }
  if (pwrite(fd_, fields.data(), fields.size(), static_cast<off_t>(entry.offset + 14)) !=
          static_cast<ssize_t>(fields.size()) ||
      (zip64 && pwrite(fd_, extra.data(), extra.size(),
                       static_cast<off_t>(entry.offset + 30 + entry.name.size() + 4)) !=
                    static_cast<ssize_t>(extra.size()))) {
    *error = Errno("Write archive");
    return false;
  }
  return true;
}

bool ArchiveWriter::AddDirectory(const std::string& name, time_t mtime, mode_t mode,
                                 std::string* error) {
  Entry entry{name, 0, 0, 0, NameFlags(name), 0,
              (static_cast<uint32_t>((mode & 07777) | S_IFDIR) << 16) | 0x10, 0, 0, offset_};
  DosTime(mtime, &entry.time, &entry.date);
  std::string header;
  Put32(&header, 0x04034b50);
  Put16(&header, kVersionDeflate);
  Put16(&header, entry.flags);
  Put16(&header, 0);  // stored
  Put16(&header, entry.time);
  Put16(&header, entry.date);
  Put32(&header, 0);
  Put32(&header, 0);
  Put32(&header, 0);
  Put16(&header, static_cast<uint16_t>(name.size()));
  Put16(&header, 0);
  header += name;
  if (!Write(header, error)) return false;
  entries_.push_back(entry);
  return true;
}

bool ArchiveWriter::AddFile(const std::string& name, int fd, uint64_t size, time_t mtime,
                            mode_t mode, std::string* error) {
  const bool zip64 = size >= kZip64Threshold;
  Entry entry{name, static_cast<uint16_t>(compress_ ? 8 : 0), 0, 0, NameFlags(name), 0,
              (static_cast<uint32_t>((mode & 07777) | S_IFREG)) << 16, 0, 0, offset_};
  DosTime(mtime, &entry.time, &entry.date);
  std::string header;
  Put32(&header, 0x04034b50);
  Put16(&header, zip64 ? kVersionZip64 : kVersionDeflate);
  Put16(&header, entry.flags);
  Put16(&header, entry.method);
  Put16(&header, entry.time);
  Put16(&header, entry.date);
  Put32(&header, 0);  // crc and sizes are patched after the data
  Put32(&header, 0);
  Put32(&header, 0);
  Put16(&header, static_cast<uint16_t>(name.size()));
  Put16(&header, zip64 ? 20 : 0);
  header += name;
  if (zip64) {
    Put16(&header, 0x0001);
    Put16(&header, 16);
    Put64(&header, 0);
    Put64(&header, 0);
  }
  if (!Write(header, error)) return false;

  std::vector<unsigned char> in(1 << 16), out(1 << 16);
  uLong crc = crc32(0L, Z_NULL, 0);
  z_stream stream{};
  if (compress_ && deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8,
                                Z_DEFAULT_STRATEGY) != Z_OK) {
    *error = "Compression setup failed";
    return false;
  }
  struct EndDeflate {
    void operator()(z_stream* s) const { deflateEnd(s); }
  };
  std::unique_ptr<z_stream, EndDeflate> cleanup(compress_ ? &stream : nullptr);
  bool eof = false;
  while (!eof) {
    ssize_t n = read(fd, in.data(), in.size());
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) {
      *error = Errno("Read " + name);
      return false;
    }
    eof = n == 0;
    entry.uncompressed += static_cast<uint64_t>(n);
    crc = crc32(crc, in.data(), static_cast<uInt>(n));
    if (!compress_) {
      if (!Write(std::string(reinterpret_cast<char*>(in.data()), static_cast<size_t>(n)), error)) {
        return false;
      }
      entry.compressed += static_cast<uint64_t>(n);
      continue;
    }
    stream.next_in = in.data();
    stream.avail_in = static_cast<uInt>(n);
    do {
      stream.next_out = out.data();
      stream.avail_out = static_cast<uInt>(out.size());
      if (deflate(&stream, eof ? Z_FINISH : Z_NO_FLUSH) == Z_STREAM_ERROR) {
        *error = "Compression failed: " + name;
        return false;
      }
      const size_t produced = out.size() - stream.avail_out;
      if (produced > 0) {
        if (!Write(std::string(reinterpret_cast<char*>(out.data()), produced), error)) return false;
        entry.compressed += produced;
      }
    } while (stream.avail_out == 0);
  }
  entry.crc = static_cast<uint32_t>(crc);
  if (!zip64 && (entry.uncompressed >= kMax32 || entry.compressed >= kMax32)) {
    *error = name + ": the file grew past 4 GB while it was read";
    return false;
  }
  if (!PatchHeader(entry, zip64, error)) return false;
  entries_.push_back(entry);
  return true;
}

bool ArchiveWriter::Finish(std::string* error) {
  const uint64_t cd_offset = offset_;
  for (const auto& e : entries_) {
    std::string extra;
    if (e.uncompressed >= kMax32) Put64(&extra, e.uncompressed);
    if (e.compressed >= kMax32) Put64(&extra, e.compressed);
    if (e.offset >= kMax32) Put64(&extra, e.offset);
    std::string record;
    Put32(&record, 0x02014b50);
    Put16(&record, kMadeByUnix | kVersionZip64);
    Put16(&record, extra.empty() ? kVersionDeflate : kVersionZip64);
    Put16(&record, e.flags);
    Put16(&record, e.method);
    Put16(&record, e.time);
    Put16(&record, e.date);
    Put32(&record, e.crc);
    Put32(&record, Clamp32(e.compressed));
    Put32(&record, Clamp32(e.uncompressed));
    Put16(&record, static_cast<uint16_t>(e.name.size()));
    Put16(&record, static_cast<uint16_t>(extra.empty() ? 0 : extra.size() + 4));
    Put16(&record, 0);  // comment
    Put16(&record, 0);  // disk
    Put16(&record, 0);  // internal attributes
    Put32(&record, e.external);
    Put32(&record, Clamp32(e.offset));
    record += e.name;
    if (!extra.empty()) {
      Put16(&record, 0x0001);
      Put16(&record, static_cast<uint16_t>(extra.size()));
      record += extra;
    }
    if (!Write(record, error)) return false;
  }
  const uint64_t cd_size = offset_ - cd_offset;
  const uint64_t count = entries_.size();
  const bool zip64 = count >= 0xffff || cd_size >= kMax32 || cd_offset >= kMax32;
  std::string end;
  if (zip64) {
    const uint64_t record_offset = offset_;
    Put32(&end, 0x06064b50);
    Put64(&end, 44);
    Put16(&end, kMadeByUnix | kVersionZip64);
    Put16(&end, kVersionZip64);
    Put32(&end, 0);
    Put32(&end, 0);
    Put64(&end, count);
    Put64(&end, count);
    Put64(&end, cd_size);
    Put64(&end, cd_offset);
    Put32(&end, 0x07064b50);  // locator
    Put32(&end, 0);
    Put64(&end, record_offset);
    Put32(&end, 1);
  }
  const uint16_t count16 = count >= 0xffff ? 0xffff : static_cast<uint16_t>(count);
  Put32(&end, 0x06054b50);
  Put16(&end, 0);
  Put16(&end, 0);
  Put16(&end, count16);
  Put16(&end, count16);
  Put32(&end, Clamp32(cd_size));
  Put32(&end, Clamp32(cd_offset));
  Put16(&end, 0);  // comment
  return Write(end, error);
}

// ---------------------------------------------------------------------------
// CreateZip

bool CreateZip(const std::string& archive,
               const std::vector<std::pair<std::string, std::string>>& inputs, bool recursive,
               bool store, const Callbacks& callbacks, Stats* stats, std::string* error) {
  error->clear();
  struct stat existing {};
  if (lstat(archive.c_str(), &existing) == 0) {
    *error = archive + " already exists";
    return false;
  }
  const size_t slash = archive.rfind('/');
  const std::string folder =
      slash == std::string::npos ? "." : (slash == 0 ? "/" : archive.substr(0, slash));
  std::string temp = folder + "/.nasgor-zip-XXXXXX";
  int fd = mkostemp(temp.data(), O_CLOEXEC);
  if (fd < 0) {
    *error = Errno("Create " + archive);
    return false;
  }
  struct stat self {};
  fstat(fd, &self);
  ArchiveWriter writer(fd, !store);
  Builder builder(&writer, self.st_dev, self.st_ino);
  builder.warn = callbacks.warn;
  builder.added = callbacks.progress;
  bool ok = true;
  for (const auto& [path, name] : inputs) {
    if (!builder.Add(path, name, recursive, error)) {
      ok = false;
      break;
    }
  }
  if (stats) *stats = builder.stats();
  if (ok && builder.stats().files + builder.stats().folders == 0) {
    *error = "Nothing to add";
    ok = false;
  }
  if (ok) ok = writer.Finish(error);
  if (ok && fsync(fd) != 0) {
    *error = Errno("Flush " + archive);
    ok = false;
  }
  if (ok) fchmod(fd, 0644);
  close(fd);
  // link() publishes the finished archive without replacing a file created meanwhile.
  if (ok && link(temp.c_str(), archive.c_str()) != 0) {
    *error = Errno("Save " + archive);
    ok = false;
  }
  unlink(temp.c_str());
  return ok;
}

}  // namespace recovery::zip
