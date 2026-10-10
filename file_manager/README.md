# nasgorOS recovery file manager

Open **File manager** from the recovery main menu. Select internal storage, SD or
USB OTG. Locked/unavailable storage is labelled; this feature does not implement
PIN/FBE decryption. The storage list starts with `/`, the recovery root filesystem
(`/dev`, `/proc`, `/sys`, `/mnt`, ...), followed by partitions mounted read-write.

Tap a folder name to enter it, or the three dots to open **Delete**, **Rename**,
**Move**, **Details** (and **Open**/**Edit** for files). Volume keys select rows; Power opens the same action menu,
with **Open folder** for directories. `../` or Back returns to the parent/storage
menu. Folders are drawn in amber, files in the default colour. **Rename** uses the
on-screen keyboard (touch screens only); the current name is pre-filled and validated
on Done. Move uses a destination browser and confirmation. Delete is permanent and
requires confirmation, default Cancel.

**+ New folder** and **+ New file** (rows under `../`) create an empty folder (0755)
or empty file (0644) in the current folder. The name is typed on the on-screen
keyboard, pre-filled with `New folder` / `nasgor.txt` (`nasgor-1.txt`, ... when taken);
without a touch screen the default name is confirmed instead. Existing names and
symlinks are never replaced. New files take the parent folder's SELinux label.

The backend uses directory-relative descriptors, rejects `..`/absolute paths,
never follows symlinks. Browsing may enter other mounts (from `/` into `/dev`,
`/proc` or `/mnt/system`), but recursive delete and copy stop at mount points, so a
mounted partition cannot be emptied through its mount point. No operation can delete
a storage root. Existing destination names are never overwritten. Cross-filesystem moves
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

## Flash image

Three-dot menu on a `*.img` file > **Flash image** (`flash_image.cpp`):

1. Choose the target from the recovery fstab: `boot`, `init_boot`, `vendor_boot`,
   `recovery`, `dtbo`, `vbmeta`, `vbmeta_system`, `vbmeta_vendor` (only `emmc`
   entries; the slot suffix is added on A/B devices).
2. The image must match the partition (`ANDROID!` for boot/init_boot/recovery,
   `VNDRBOOT`, DTBO magic `d7b7ab1e`, `AVB0` for vbmeta) and fit in it; otherwise
   nothing is written.
3. After confirmation it is written in 1 MiB chunks with progress, flushed, and read
   back from the device for verification. The rest of the partition is left as it is
   (like fastboot). A read-only block device flag is cleared once if the write fails.

Host test: `g++ -std=c++17 -Wall -Wextra -Werror file_manager/flash_image.cpp
file_manager/flash_image_test.cpp -o /tmp/flash-image-test && /tmp/flash-image-test`

## Startup

While recovery starts (mounting `/data` for adb keys on userdebug builds, USB setup
and the volume scan), an animated **Starting recovery...** screen is shown
(`ShowBusy`/`HideBusy`) instead of a frozen screen; it ends when the volume manager
is up or a menu is shown. Touches during startup are discarded. Each step logs its
duration as `nasgorOS startup: <step> took N ms` (Advanced > View recovery logs).

## Zip and unzip

Three dots on a file or folder > **Compress (zip)** creates `<name>.zip` next to it
(`<name>-1.zip`, ... when taken); folders are added with their contents. Three dots
on a `.zip` > **Extract** unpacks it into a new folder named after the archive.
Nothing existing is replaced: the archive is written to a temporary file and published
with `link()`, extraction only creates new files in its new folder. Entries with `..`,
absolute names or backslashes and symlink entries are skipped. Unix permissions are
kept (execute bits on extract). Symlinks to files are followed when compressing;
symlinked folders and special files are skipped.

The Terminal has `zip` (`/system/bin/zip`, module `nasgor_zip.recovery`):
`zip [-r] [-0] [-q] archive[.zip] path...` (Info-ZIP style exit codes: 12 nothing to
add, 16 bad arguments, 18 some paths skipped). It never modifies an existing archive.
`unzip`/`zipinfo` are AOSP `ziptool`.

The writer (`file_manager/zip_create.cpp`) is own code with **zip64**: files and
archives over 4 GB and more than 65535 entries; deflate via zlib. Extraction uses
libziparchive (also zip64). The ROM installer (Apply update/sideload) does not use
any of this code.

## Text viewer

Three dots on a file > **Open** shows it read-only (`ViewDocument`,
`recovery_ui/text_viewer.cpp` for the model): line numbers, files up to 2 MB, LF/CRLF,
binary files (NUL bytes) and symlinks refused, non-ASCII characters shown as `?`.
Buttons: **Back**, **Wrap** (on by default; off scrolls sideways with **<** / **>**),
**Top**, **End**. Swipe or volume keys scroll; Power or Back closes it. The file is
never modified.

## Text editor

Three dots on a file > **Edit** opens it in a text editor (`EditDocument`,
`recovery_ui/text_editor.cpp` for the buffer). Files up to 256 KB; binary files
(NUL bytes) and symlinks are refused. The control row has **Save**, **Exit**,
arrows, Home/End and Page Up/Down; tapping the text moves the cursor; lines are
soft-wrapped; volume keys page. Line endings (LF/CRLF) and the final newline are
kept; non-ASCII characters show as `?` but their bytes are kept unless deleted.
Exit with unsaved changes asks for a second tap.

Saving writes a temporary file in the same folder, flushes it, copies the mode,
owner and **SELinux label** (`security.selinux`), then renames it over the
original, so the original stays intact if anything fails. For files in OS
partitions, mount the partition RW first (Mount partitions).

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

The File manager storage list always starts with **`/`**, the recovery's own
root filesystem (like TWRP): `/dev`, `/proc`, `/sys`, `/tmp`, `/mnt`, ... Every
mounted partition is reachable there (`/mnt/system`, `/mnt/vendor`, ...).
`/system` inside it is the recovery ramdisk's system folder, not the system
partition.

Partitions mounted **read-write** also get a shortcut in the storage list;
read-only or unmounted ones get none. They are labelled like device paths: on
system-as-root devices (merlinx) the system partition's `system/` folder is
**`/system`**; other partitions appear as `/product`, `/system_ext`, `/vendor`,
`/odm`. Folders are deleted recursively (files, subfolders and symlinks; symlink
targets are untouched), so take care under `/`.

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
g++ -std=c++17 -Wall -Wextra -Werror -Irecovery_ui/include \
  recovery_ui/text_editor.cpp recovery_ui/text_editor_test.cpp -o /tmp/text-editor-test
g++ -std=c++17 -Wall -Wextra -Werror -Irecovery_ui/include recovery_ui/text_editor.cpp \
  recovery_ui/text_viewer.cpp recovery_ui/text_viewer_test.cpp -o /tmp/text-viewer-test
g++ -std=c++17 -Wall -Wextra -Werror file_manager/zip_create.cpp \
  file_manager/zip_create_test.cpp -lz -o /tmp/zip-create-test   # needs python3 + unzip
/tmp/file-manager-test && /tmp/keyboard-test && /tmp/terminal-test && /tmp/text-editor-test && \
  /tmp/text-viewer-test && /tmp/zip-create-test   # NASGOR_ZIP_BIG=1: >4 GB zip64 case
```

Build `m -j8 recoveryimage`. Check the padded image against the device's recovery
partition and test boot, touch hitboxes/rotation (menus, keyboard keys), physical
keys, locked internal storage, SD/OTG, interrupted operations, the terminal
(prompt, Ctrl+C, scrollback, Exit) and the editor (edit, save, SELinux label kept
on a /system file) on a device before release.
