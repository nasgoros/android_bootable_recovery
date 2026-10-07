# nasgorOS recovery file manager

Open **File manager** from the recovery main menu. Select internal storage, SD or
USB OTG. Locked/unavailable storage is labelled; this feature does not implement
PIN/FBE decryption. It exposes storage volumes, not raw partitions or `/dev`.

Tap a folder name to enter it, or the three dots to open **Delete**, **Rename**,
**Move**, **Details**. Volume keys select rows; Power opens the same action menu,
with **Open folder** for directories. `../` or Back returns to the parent/storage
menu. Folders are drawn in amber, files in the default colour. **Rename** uses the
on-screen keyboard (touch screens only); the current name is pre-filled and validated
on Done. Move uses a destination browser and confirmation. Delete is permanent and
requires confirmation, default Cancel.

The backend uses directory-relative descriptors, rejects `..`/absolute paths,
never follows symlinks and refuses child mounts. No operation can delete a storage
root. Existing destination names are never overwritten. Cross-filesystem moves
copy into a private destination staging directory, flush the copy, publish it
atomically, and only then remove the source. If removal fails, the error states
that the copy exists and source removal is incomplete. Power loss during a copy
can leave `.nasgor-move-*` staging content; the source is retained until commit.
Do not unplug storage during an operation. Special files cannot be copied across
filesystems. Directory details report the directory entry size, not recursive size.

All recovery UI text is **English only** (menus, prompts, errors). Do not add
Indonesian or bilingual strings.

Storage volumes (SD card, USB OTG) opened by the file manager are unmounted when
it is closed. Leaving one mounted made **Apply update > Choose from sdcard1** fail,
because the volume manager refuses to mount a volume that is already mounted.

## On-screen keyboard

`ScreenRecoveryUI::EditText()` (`recovery_ui/nasgor_screens.cpp`) shows a touch
keyboard: QWERTY with one-shot Shift, two symbol pages (`?123`, `#+=`), space,
Del, Cancel and Done. The layout/hit-test model is `recovery_ui/keyboard.cpp`
(no graphics dependency). Labels are ASCII because the recovery font has no other
glyphs. Physical Back cancels.

## Terminal

**Terminal** in the main menu opens a root shell (`/system/bin/sh -i`) on a
pseudo-terminal (`recovery_ui/terminal.cpp`, `PtyShell`). Output is rendered by a
small own terminal engine (`TerminalEngine`): text, CR/LF, backspace, tab, the
common ANSI cursor/erase sequences and scrollback; colours are ignored and
non-ASCII characters show as `?`. The keyboard adds a control row: **Exit**, Esc,
Tab, **Ctrl** (next letter becomes Ctrl+letter, e.g. Ctrl+C), and arrow keys.
Volume up/down or a vertical swipe scrolls back; typing returns to the live screen.
Exit (or physical Back) hangs up the shell and its children. Without a touch screen
the menu prints a hint to use `adb shell` (Advanced > Enable ADB) instead.

The keyboard and terminal are nasgorOS code under Apache-2.0, written from scratch;
TWRP was used only as a behavioural reference (its sources are GPLv3).

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

Host regression tests (Linux; from the repository root; `/tmp` and `/dev/shm` on
different filesystems; the terminal test needs `/bin/sh` and `/bin/stty`):

```sh
g++ -std=c++17 -Wall -Wextra -Werror -Wno-ignored-attributes \
  file_manager/operations.cpp file_manager/operations_test.cpp -o /tmp/file-manager-test
g++ -std=c++17 -Wall -Wextra -Werror -Irecovery_ui/include \
  recovery_ui/keyboard.cpp recovery_ui/keyboard_test.cpp -o /tmp/keyboard-test
g++ -std=c++17 -Wall -Wextra -Werror -Irecovery_ui/include \
  recovery_ui/terminal.cpp recovery_ui/terminal_test.cpp -o /tmp/terminal-test
/tmp/file-manager-test && /tmp/keyboard-test && /tmp/terminal-test
```

Build `m -j8 recoveryimage`. Check the padded image against the device's recovery
partition and test boot, touch hitboxes/rotation (menus, keyboard keys), physical
keys, locked internal storage, SD/OTG, interrupted operations, and the terminal
(prompt, Ctrl+C, scrollback, Exit) on a device before release.
