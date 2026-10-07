// Copyright (C) 2026 The nasgorOS Project
// SPDX-License-Identifier: Apache-2.0
#include "file_manager.h"
#include "operations.h"
#include "partitions.h"
#include <ctime>
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
// Partition roots are labelled like device paths ("/", "/system", "/product");
// storage volumes keep "<label>:/<folder>".
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
// Only partitions mounted read-write are offered for browsing; read-only ones are
// hidden. On system-as-root devices the system partition holds the whole root
// filesystem, so it appears as "/" and its system/ folder as "/system".
std::vector<Root> WritablePartitionRoots() {
  std::vector<Root> roots;
  for (const auto& part : recovery::partitions::List()) {
    if (recovery::partitions::GetState(part) != recovery::partitions::State::kReadWrite) continue;
    if (part.name == "system" && IsDirectory(part.mount_point + "/system")) {
      roots.push_back({"/", part.mount_point});
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
  void Details(const Storage& storage, const std::string& path);
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
    std::vector<std::string> items{"Back", "Refresh storage"};
    // Partitions mounted RW from "Mount partitions" (main menu) are listed first.
    const auto roots = WritablePartitionRoots();
    for (const auto& root : roots) items.push_back(root.label + "  [RW]");
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
      if (!storage->valid()) { Message({"Cannot read partition: " + root.path}); continue; }
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
  while (!stopped_) {
    if (!ui_->EditText({"Rename", "Current: " + DisplayName(old)}, name)) return false;
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
  while (!stopped_) {
    std::vector<Entry> entries;
    std::string error;
    if (!storage.List(folder, &entries, &error)) {
      Message({error});
      if (folder.empty()) return;
      folder = Parent(folder);
      continue;
    }
    std::vector<std::string> items{"../"};
    std::vector<bool> actions{false};
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
    const auto& entry = entries[index - 1];
    std::string path = Join(folder, entry.name);
    if (!overflow && S_ISDIR(entry.info.st_mode)) { folder = path; selection = 0; continue; }
    std::vector<std::string> options{"Cancel", "Delete", "Rename", "Move", "Details"};
    if (S_ISDIR(entry.info.st_mode)) options.push_back("Open folder");
    size_t action = Menu({DisplayName(path)}, options);
    if (action == 1) {
      if (Confirm({"Delete permanently?", DisplayName(path),
                   "Folders are deleted with all their contents."}, "Delete")) {
        ui_->Print("Deleting %s...\n", DisplayName(path).c_str());
        if (!storage.Remove(path, &error)) Message({"Delete incomplete:", error});
      }
    } else if (action == 2) {
      std::string name;
      if (RenameInput(entry.name, &name) && name != entry.name &&
          !storage.Move(path, storage, Join(folder, name), &error)) Message({error});
    } else if (action == 3) {
      std::unique_ptr<Storage> dest;
      std::string dest_folder, dest_label;
      if (Destination(&dest, &dest_folder, &dest_label) &&
          Confirm({"Move", Location(label, path),
                   "To: " + Location(dest_label, Join(dest_folder, entry.name)),
                   "Existing names are never overwritten."}, "Move here")) {
        ui_->Print("Moving %s; please keep storage connected...\n", DisplayName(path).c_str());
        if (!storage.Move(path, *dest, Join(dest_folder, entry.name), &error)) Message({error});
      }
    } else if (action == 4) {
      Details(storage, path);
    } else if (action == 5 && S_ISDIR(entry.info.st_mode)) { folder = path; selection = 0; }
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
