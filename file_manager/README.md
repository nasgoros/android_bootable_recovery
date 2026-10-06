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
