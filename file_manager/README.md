# nasgorOS recovery file manager

Open **File manager** from the recovery main menu. Select internal storage, SD or
USB OTG. Locked/unavailable storage is labelled; this feature does not implement
PIN/FBE decryption. It exposes storage volumes, not raw partitions or `/dev`.

Tap a folder name to enter it, or the three dots to open **Hapus / Delete**,
**Rename**, **Move**, **Rincian / Details**. Volume keys select rows; Power opens
the same action menu, with **Open folder** for directories. `../` or Back returns
to the parent/storage menu. Rename supports a character picker and UTF-8-aware
backspace; existing Unicode names are retained. Move uses a destination browser
and confirmation. Delete is permanent and requires confirmation, default Cancel.

The backend uses directory-relative descriptors, rejects `..`/absolute paths,
never follows symlinks and refuses child mounts. No operation can delete a storage
root. Existing destination names are never overwritten. Cross-filesystem moves
copy into a private destination staging directory, flush the copy, publish it
atomically, and only then remove the source. If removal fails, the error states
that the copy exists and source removal is incomplete. Power loss during a copy
can leave `.nasgor-move-*` staging content; the source is retained until commit.
Do not unplug storage during an operation. Special files cannot be copied across
filesystems. Directory details report the directory entry size, not recursive size.

## Mount partitions (RO/RW)

**Mount partitions (RO/RW)** is a separate entry in the recovery main menu. It
lists system, system_ext, product, vendor and odm from the recovery fstab. Each
can be mounted read-only or read-write below `/mnt/<name>`, or unmounted.
Dynamic partitions are mapped first. Read-write clears the block device
read-only flag and remounts; it asks for confirmation.

Only partitions mounted **read-write** appear in the File manager storage list;
read-only or unmounted ones are hidden. They are labelled like device paths:
on system-as-root devices (merlinx) the system partition is shown as **`/`**
(the root filesystem) and its `system/` folder as **`/system`**; other partitions
appear as `/product`, `/system_ext`, `/vendor`, `/odm`. Folders are deleted
recursively (files, subfolders and symlinks; symlink targets are untouched).

Moving a file *into* a partition copies it without its SELinux label; deleting
or renaming existing files is safer. merlinx images are ext4 without shared
blocks and its vbmeta disables hashtree checks, so RW changes boot; other
devices (erofs, shared blocks, enforced verity) may not support RW.

Example: to disable addon.d (scripts that survive a ROM flash, e.g. LiteGapps),
mount `system` RW in **Mount partitions**, then in the File manager open `/system`
and delete or rename `addon.d`.

Before install, wipe, rescue/fastboot or reboot, every partition mounted from
this menu is unmounted (`IsPartitionWritingAction` in recovery.cpp). On user
builds the menu stays available when the file manager is enabled.

Feature switch: `ro.nasgoros.recovery_file_manager=true` in recovery properties.
Upstream baseline: LineageOS `37c5d17bec80020b8dbfb0a0d39b4ba93b2b6efd`.
Keep the feature module separate when rebasing the fork onto LineageOS.

Host regression test (Linux, writable `/tmp` and `/dev/shm` on different filesystems):

```sh
g++ -std=c++17 -Wall -Wextra -Werror -Wno-ignored-attributes \
  file_manager/operations.cpp file_manager/operations_test.cpp -o /tmp/file-manager-test
/tmp/file-manager-test
```

Build `m -j8 recoveryimage`. Check the padded image against the device's recovery
partition and test boot, touch hitboxes/rotation, physical keys, locked internal
storage, SD/OTG, and interrupted operations on a device before release.
