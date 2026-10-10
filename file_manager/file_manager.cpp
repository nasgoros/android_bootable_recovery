// Copyright (C) 2026 The nasgorOS Project
// SPDX-License-Identifier: Apache-2.0
#include "file_manager.h"
#include "operations.h"
#include "partitions.h"
#include "flash_image.h"
#include "recovery_utils/roots.h"
#include <fcntl.h>
#include <unistd.h>
#include <fstab/fstab.h>
#include <ctime>
#include <strings.h>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include <android-base/stringprintf.h>
#include "recovery_ui/device.h"
#include "recovery_ui/ui.h"
#include <volume_manager/VolumeManager.h>

namespace {
using recovery::files::DisplayName;
using recovery::files::Entry;
using recovery::files::Storage;
using android::volmgr::VolumeInfo;
using android::volmgr::VolumeManager;
std::string Join(const std::string& dir, const std::string& name) {
  return dir.empty() ? name : dir + "/" + name;
}
std::string Parent(const std::string& path) {
  auto slash = path.rfind('/');
  return slash == std::string::npos ? "" : path.substr(0, slash);
}
// The recovery root and partitions are labelled like device paths ("/", "/system",
// "/product"); storage volumes keep "<label>:/<folder>".
std::string Location(const std::string& label, const std::string& folder) {
  if (!label.empty() && label[0] == '/') {
    std::string base = label == "/" ? "" : label;
    return base + "/" + DisplayName(folder);
  }
  return label + ":/" + DisplayName(folder);
}
bool IsDirectory(const std::string& path) {
  struct stat st {};
  return lstat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
struct Root {
  std::string label;  // "/", "/system", "/product", ...
  std::string path;   // where it is mounted in recovery
};
// Only partitions mounted read-write are offered as shortcuts; read-only ones are
// hidden. On system-as-root devices the system partition's system/ folder is the
// one shown as "/system". "/" is always the recovery's own root filesystem.
std::vector<Root> WritablePartitionRoots() {
  std::vector<Root> roots;
  for (const auto& part : recovery::partitions::List()) {
    if (recovery::partitions::GetState(part) != recovery::partitions::State::kReadWrite) continue;
    if (part.name == "system" && IsDirectory(part.mount_point + "/system")) {
      roots.push_back({"/system", part.mount_point + "/system"});
    } else {
      roots.push_back({"/" + part.name, part.mount_point});
    }
  }
  return roots;
}
class Browser {
 public:
  explicit Browser(Device* device) : ui_(device->GetUI()),
      keys_(std::bind(&Device::HandleMenuKey, device, std::placeholders::_1, std::placeholders::_2)) {}
  // Unmount the storage volumes this browser mounted. Otherwise they stay busy and
  // "Apply update > Choose from <volume>" fails: VolumeManager refuses to mount an
  // already mounted volume (-EBUSY).
  ~Browser() {
    for (const auto& id : mounted_volumes_) VolumeManager::Instance()->volumeUnmount(id);
  }
  void Run();
 private:
  size_t Menu(const std::vector<std::string>& headers, const std::vector<std::string>& items) {
    size_t result = ui_->ShowMenu(headers, items, 0, true, keys_, true);
    if (result == static_cast<size_t>(Device::kGoHome) ||
        result == static_cast<size_t>(RecoveryUI::KeyError::INTERRUPTED)) stopped_ = true;
    return result;
  }
  void Message(const std::vector<std::string>& lines) { Menu(lines, {"Back"}); }
  bool Confirm(const std::vector<std::string>& lines, const std::string& action) {
    return Menu(lines, {"Cancel", action}) == 1;
  }
  std::unique_ptr<Storage> ChooseStorage(std::string* label);
  bool Destination(std::unique_ptr<Storage>* storage, std::string* folder, std::string* label);
  bool RenameInput(const std::string& old, std::string* name);
  bool NameInput(const std::vector<std::string>& headers, std::string* name);
  void Create(Storage& storage, const std::string& label, const std::string& folder, bool folder_kind,
              std::string* created);
  void ViewFile(const Storage& storage, const std::string& label, const std::string& path);
  void Details(const Storage& storage, const std::string& path);
  void EditFile(const Storage& storage, const std::string& label, const std::string& path);
  void FlashImage(const Storage& storage, const std::string& label, const std::string& path);
  void Browse(Storage& storage, const std::string& label);
 public:
  void Partitions();
 private:
  RecoveryUI* ui_;
  std::function<int(int, bool)> keys_;
  bool stopped_ = false;
  std::set<std::string> mounted_volumes_;  // volume IDs mounted by this browser
};
std::unique_ptr<Storage> Browser::ChooseStorage(std::string* label) {
  while (!stopped_) {
    std::vector<VolumeInfo> volumes;
    VolumeManager::Instance()->getVolumeInfo(volumes);
    std::vector<std::string> items{"Back", "Refresh storage", "/  (root: dev, proc, sys, mnt...)"};
    // Partitions mounted RW from "Mount partitions" (main menu) follow the root.
    std::vector<Root> roots{{"/", "/"}};
    for (auto& root : WritablePartitionRoots()) roots.push_back(std::move(root));
    for (size_t i = 1; i < roots.size(); ++i) items.push_back(roots[i].label + "  [RW]");
    const size_t first_volume = 2 + roots.size();
    std::vector<VolumeInfo> available;
    for (const auto& volume : volumes) {
      available.push_back(volume);
      items.push_back(DisplayName(volume.mLabel.empty() ? volume.mId : volume.mLabel) +
                      (volume.mMountable ? "" : " [locked/unavailable]"));
    }
    size_t choice = Menu({"File manager - Storage", "Internal storage, SD card, USB OTG",
                          "Partitions appear here after mounting them RW in",
                          "main menu > Mount partitions (RO/RW)"}, items);
    if (choice == 1 || choice == static_cast<size_t>(Device::kRefresh)) continue;
    if (choice < 2 || choice >= items.size()) return nullptr;
    if (choice < first_volume) {
      const auto& root = roots[choice - 2];
      auto storage = std::make_unique<Storage>(root.path);
      if (!storage->valid()) { Message({"Cannot read: " + root.path}); continue; }
      *label = root.label;
      return storage;
    }
    const auto& volume = available[choice - first_volume];
    // A volume opened earlier in this session is still mounted; mounting it again
    // would fail with -EBUSY.
    bool volume_open = mounted_volumes_.count(volume.mId) > 0;
    if (!volume_open && volume.mMountable && VolumeManager::Instance()->volumeMount(volume.mId)) {
      mounted_volumes_.insert(volume.mId);
      volume_open = true;
    }
    if (!volume_open) {
      Message({"Cannot open storage.", "Internal storage may be encrypted/locked.",
               "This recovery does not decrypt PIN/FBE storage.", "For SD/USB: check the connection and filesystem."});
      continue;
    }
    // Paths may be assigned by the volume driver during mounting.
    std::vector<VolumeInfo> mounted;
    VolumeManager::Instance()->getVolumeInfo(mounted);
    std::string path;
    for (const auto& v : mounted) if (v.mId == volume.mId) path = v.mPath;
    if (path.empty()) { Message({"Storage disconnected."}); continue; }
    auto storage = std::make_unique<Storage>(path);
    if (!storage->valid()) { Message({"Storage is unreadable or still locked."}); continue; }
    *label = items[choice];
    return storage;
  }
  return nullptr;
}
// Text files up to this size can be edited (configuration files, scripts).
constexpr size_t kMaxEditSize = 256 * 1024;
// The read-only viewer accepts larger logs and sources.
constexpr size_t kMaxViewSize = 2 * 1024 * 1024;

void Browser::ViewFile(const Storage& storage, const std::string& label, const std::string& path) {
  std::string content, error;
  if (!storage.ReadFile(path, kMaxViewSize, &content, &error)) {
    Message({"Cannot open file:", error});
    return;
  }
  if (!ui_->ViewDocument(Location(label, path), content, &error) && !error.empty()) {
    Message({error});
  }
}

// Default name, or "<stem>-N<ext>" when it is taken in `entries`.
static std::string FreeName(const std::vector<Entry>& entries, const std::string& base) {
  auto taken = [&](const std::string& name) {
    for (const auto& e : entries) if (e.name == name) return true;
    return false;
  };
  if (!taken(base)) return base;
  const size_t dot = base.rfind('.');
  const std::string stem = dot == std::string::npos || dot == 0 ? base : base.substr(0, dot);
  const std::string ext = stem.size() == base.size() ? "" : base.substr(dot);
  for (int i = 1; i < 1000; ++i) {
    std::string name = stem + "-" + std::to_string(i) + ext;
    if (!taken(name)) return name;
  }
  return base;
}

void Browser::Create(Storage& storage, const std::string& label, const std::string& folder,
                     bool folder_kind, std::string* created) {
  std::vector<Entry> entries;
  std::string error;
  storage.List(folder, &entries, &error);
  std::string name = FreeName(entries, folder_kind ? "New folder" : "nasgor.txt");
  const std::string what = folder_kind ? "New folder" : "New file";
  if (ui_->HasOnScreenKeyboard()) {
    if (!NameInput({what, "In: " + Location(label, folder)}, &name)) return;
  } else if (!Confirm({what, "In: " + Location(label, folder), "Name: " + DisplayName(name)},
                      "Create")) {
    return;
  }
  const std::string path = Join(folder, name);
  const bool ok = folder_kind ? storage.CreateFolder(path, &error) : storage.CreateFile(path, &error);
  if (!ok) {
    Message({"Cannot create " + DisplayName(name) + ":", error});
    return;
  }
  *created = name;
}

void Browser::EditFile(const Storage& storage, const std::string& label,
                       const std::string& path) {
  std::string content, error;
  if (!storage.ReadFile(path, kMaxEditSize, &content, &error)) {
    Message({"Cannot open file:", error});
    return;
  }
  if (!ui_->EditDocument(Location(label, path), &content, &error)) {
    if (!error.empty()) Message({error});
    return;
  }
  ui_->Print("Saving %s...\n", DisplayName(path).c_str());
  if (!storage.WriteFileAtomic(path, content, &error)) {
    Message({"Save failed:", error});
  }
}

void Browser::FlashImage(const Storage& storage, const std::string& label,
                         const std::string& path) {
  // Image partitions from the recovery fstab (emmc entries), with the slot suffix on A/B.
  struct Target { std::string name, device; };
  std::vector<Target> targets;
  for (const char* point : {"/boot", "/init_boot", "/vendor_boot", "/recovery", "/dtbo",
                            "/vbmeta", "/vbmeta_system", "/vbmeta_vendor"}) {
    const auto* volume = volume_for_mount_point(point);
    if (!volume || volume->fs_type != "emmc") continue;
    std::string device = volume->blk_device;
    std::string name = std::string(point).substr(1);
    if (volume->fs_mgr_flags.slot_select) {
      device += fs_mgr_get_slot_suffix();
      name += fs_mgr_get_slot_suffix();
    }
    targets.push_back({name, device});
  }
  if (targets.empty()) { Message({"No image partitions found in the recovery fstab."}); return; }
  std::vector<std::string> items{"Cancel"};
  for (const auto& target : targets) items.push_back(target.name);
  size_t choice = Menu({"Flash image", Location(label, path), "Choose the target partition"}, items);
  if (choice == 0 || choice >= items.size()) return;
  const Target& target = targets[choice - 1];
  std::string error;
  struct stat st {};
  int fd = storage.OpenRead(path, &st, &error);
  if (fd < 0) { Message({"Cannot open image:", error}); return; }
  std::unique_ptr<int, void (*)(int*)> closer(&fd, [](int* f) { close(*f); });
  char header[16] = {};
  ssize_t got = pread(fd, header, sizeof(header), 0);
  const auto kind = recovery::flash::KindForPartition(target.name);
  if (got < 8 || !recovery::flash::CheckHeader(kind, std::string(header, got), &error)) {
    Message({"Not flashed:", error.empty() ? std::string("Cannot read the image.") : error});
    return;
  }
  int64_t size = recovery::flash::DeviceSize(target.device, &error);
  if (size < 0) { Message({error}); return; }
  if (st.st_size > size) {
    Message({"Not flashed: the image is larger than the partition.",
             std::to_string(st.st_size) + " > " + std::to_string(size) + " bytes"});
    return;
  }
  if (!Confirm({"Flash " + target.name + "?", Location(label, path), "To: " + target.device,
                std::to_string(st.st_size / 1024) + " KB of " + std::to_string(size / 1024) + " KB",
                "A wrong image can stop the phone from booting."}, "Flash")) {
    return;
  }
  ui_->Print("Flashing %s to %s...\n", DisplayName(path).c_str(), target.name.c_str());
  int last = -1;
  bool ok = recovery::flash::Flash(fd, static_cast<uint64_t>(st.st_size), target.device,
                                   [&](int percent) {
    if (percent / 10 != last / 10) ui_->Print("  %d%%\n", percent);
    last = percent;
  }, &error);
  Message(ok ? std::vector<std::string>{"Flashed and verified: " + target.name}
             : std::vector<std::string>{"Flash failed:", error});
}

void Browser::Details(const Storage& storage, const std::string& path) {
  struct stat st {};
  std::string error;
  if (!storage.Stat(path, &st, &error)) { Message({error}); return; }
  char date[80] = "Unknown";
  struct tm time {};
  if (localtime_r(&st.st_mtime, &time)) strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S %Z", &time);
  std::string type = S_ISDIR(st.st_mode) ? "Folder" : S_ISLNK(st.st_mode) ? "Symbolic link" :
                     S_ISREG(st.st_mode) ? "File" : "Special file";
  Message({"Details", DisplayName(path), "Type: " + type,
           android::base::StringPrintf("Size: %lld bytes%s", static_cast<long long>(st.st_size),
                                      S_ISDIR(st.st_mode) ? " (folder entry, not contents)" : ""),
           android::base::StringPrintf("Permissions: %04o  UID: %u  GID: %u", st.st_mode & 07777,
                                      st.st_uid, st.st_gid),
           std::string("Modified: ") + date});
}
bool Browser::RenameInput(const std::string& old, std::string* name) {
  *name = old;
  if (!ui_->HasOnScreenKeyboard()) {
    Message({"Rename needs a touch screen for the on-screen keyboard."});
    return false;
  }
  return NameInput({"Rename", "Current: " + DisplayName(old)}, name);
}
bool Browser::NameInput(const std::vector<std::string>& headers, std::string* name) {
  while (!stopped_) {
    if (!ui_->EditText(headers, name)) return false;
    if (Storage::ValidName(*name)) return true;
    Message({"Name must be 1-255 bytes, not . or .., without /"});
  }
  return false;
}
bool Browser::Destination(std::unique_ptr<Storage>* storage, std::string* folder, std::string* label) {
  *storage = ChooseStorage(label);
  if (!*storage) return false;
  folder->clear();
  while (!stopped_) {
    std::vector<Entry> entries;
    std::string error;
    if (!(*storage)->List(*folder, &entries, &error)) { Message({error}); return false; }
    std::vector<std::string> items{"Cancel", "Move here", "../"};
    std::vector<std::string> dirs;
    for (const auto& entry : entries) {
      if (S_ISDIR(entry.info.st_mode)) { dirs.push_back(entry.name); items.push_back(DisplayName(entry.name) + "/"); }
    }
    size_t choice = Menu({"Choose destination", Location(*label, *folder)}, items);
    if (choice == 1) return true;
    if (choice == static_cast<size_t>(Device::kRefresh)) continue;
    if (choice == 2 || choice == static_cast<size_t>(Device::kGoBack)) {
      if (folder->empty()) return false;
      *folder = Parent(*folder);
    } else if (choice >= 3 && choice < items.size()) {
      *folder = Join(*folder, dirs[choice - 3]);
    } else return false;
  }
  return false;
}
void Browser::Browse(Storage& storage, const std::string& label) {
  std::string folder;
  size_t selection = 0;
  std::string select_name;  // entry to highlight after creating it
  while (!stopped_) {
    std::vector<Entry> entries;
    std::string error;
    if (!storage.List(folder, &entries, &error)) {
      Message({error});
      if (folder.empty()) return;
      folder = Parent(folder);
      continue;
    }
    // Rows before the folder entries.
    constexpr size_t kNewFolderRow = 1, kNewFileRow = 2, kFirstEntryRow = 3;
    std::vector<std::string> items{"../", "+ New folder", "+ New file"};
    std::vector<bool> actions{false, false, false};
    if (!select_name.empty()) {
      for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].name == select_name) selection = kFirstEntryRow + i;
      }
      select_name.clear();
    }
    for (const auto& entry : entries) {
      items.push_back(DisplayName(entry.name) + (S_ISDIR(entry.info.st_mode) ? "/" : ""));
      actions.push_back(true);
    }
    if (selection >= items.size()) selection = 0;
    size_t result = ui_->ShowFileMenu({"File manager", Location(label, folder),
                                      "Tap a folder to open | three dots: actions", "Volume: select | Power: actions"},
                                     items, actions, selection, keys_);
    if (result == static_cast<size_t>(Device::kRefresh)) continue;
    if (result == static_cast<size_t>(Device::kGoHome) ||
        result == static_cast<size_t>(RecoveryUI::KeyError::INTERRUPTED)) { stopped_ = true; return; }
    if (result == 0 || result == static_cast<size_t>(Device::kGoBack)) {
      if (folder.empty()) return;
      folder = Parent(folder);
      selection = 0;
      continue;
    }
    bool overflow = (result & RecoveryUI::kFileAction) != 0;
    size_t index = result & ~RecoveryUI::kFileAction;
    if (index == 0 || index >= items.size()) return;
    selection = index;
    if (index == kNewFolderRow || index == kNewFileRow) {
      Create(storage, label, folder, index == kNewFolderRow, &select_name);
      continue;
    }
    const auto& entry = entries[index - kFirstEntryRow];
    std::string path = Join(folder, entry.name);
    if (!overflow && S_ISDIR(entry.info.st_mode)) { folder = path; selection = 0; continue; }
    std::vector<std::string> options{"Cancel", "Delete", "Rename", "Move", "Details"};
    if (S_ISDIR(entry.info.st_mode)) options.push_back("Open folder");
    if (S_ISREG(entry.info.st_mode)) {
      options.push_back("Open");
      options.push_back("Edit");
    }
    const bool is_image = entry.name.size() > 4 &&
        strcasecmp(entry.name.c_str() + entry.name.size() - 4, ".img") == 0;
    if (S_ISREG(entry.info.st_mode) && is_image) options.push_back("Flash image");
    size_t action = Menu({DisplayName(path)}, options);
    const std::string chosen = action < options.size() ? options[action] : "Cancel";
    if (chosen == "Delete") {
      if (Confirm({"Delete permanently?", DisplayName(path),
                   "Folders are deleted with all their contents."}, "Delete")) {
        ui_->Print("Deleting %s...\n", DisplayName(path).c_str());
        if (!storage.Remove(path, &error)) Message({"Delete incomplete:", error});
      }
    } else if (chosen == "Rename") {
      std::string name;
      if (RenameInput(entry.name, &name) && name != entry.name &&
          !storage.Move(path, storage, Join(folder, name), &error)) Message({error});
    } else if (chosen == "Move") {
      std::unique_ptr<Storage> dest;
      std::string dest_folder, dest_label;
      if (Destination(&dest, &dest_folder, &dest_label) &&
          Confirm({"Move", Location(label, path),
                   "To: " + Location(dest_label, Join(dest_folder, entry.name)),
                   "Existing names are never overwritten."}, "Move here")) {
        ui_->Print("Moving %s; please keep storage connected...\n", DisplayName(path).c_str());
        if (!storage.Move(path, *dest, Join(dest_folder, entry.name), &error)) Message({error});
      }
    } else if (chosen == "Details") {
      Details(storage, path);
    } else if (chosen == "Open folder") {
      folder = path;
      selection = 0;
    } else if (chosen == "Open") {
      ViewFile(storage, label, path);
    } else if (chosen == "Edit") {
      EditFile(storage, label, path);
    } else if (chosen == "Flash image") {
      FlashImage(storage, label, path);
    }
  }
}
void Browser::Partitions() {
  using recovery::partitions::State;
  while (!stopped_) {
    auto parts = recovery::partitions::List();
    std::vector<std::string> items{"Back"};
    for (const auto& part : parts) {
      items.push_back(part.name + "  [" +
                      recovery::partitions::StateLabel(recovery::partitions::GetState(part)) + "]");
    }
    size_t choice = Menu({"Mount partitions",
                          "RW: changes are written directly to the partition",
                          "RW partitions appear in File manager (/, /system, /product, ...)"},
                         items);
    if (choice == static_cast<size_t>(Device::kRefresh)) continue;
    if (choice == 0 || choice >= items.size()) return;
    const auto& part = parts[choice - 1];
    State state = recovery::partitions::GetState(part);
    std::vector<std::string> options{"Cancel", "Mount read-only (RO)",
                                     "Mount read-write (RW)", "Unmount"};
    size_t action = Menu({part.name + " -> " + part.mount_point,
                          std::string("Status: ") + recovery::partitions::StateLabel(state)},
                         options);
    std::string error;
    bool ok = true;
    if (action == 1) {
      ok = recovery::partitions::Mount(part, false, &error);
    } else if (action == 2) {
      if (!Confirm({"Mount " + part.name + " read-write?",
                    "Changes may stop the system from booting.",
                    "New files get no SELinux label; delete/rename is safer."}, "Mount RW")) continue;
      ok = recovery::partitions::Mount(part, true, &error);
    } else if (action == 3) {
      ok = recovery::partitions::Unmount(part, &error);
    } else {
      continue;
    }
    if (!ok) Message({error});
  }
}
void Browser::Run() {
  while (!stopped_) {
    std::string label;
    auto storage = ChooseStorage(&label);
    if (!storage) return;
    Browse(*storage, label);
  }
}
}  // namespace
void RunFileManager(Device* device) { Browser(device).Run(); }
void RunPartitionMenu(Device* device) { Browser(device).Partitions(); }
