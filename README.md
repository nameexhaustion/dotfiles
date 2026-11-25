# dotfiles

Run `./adb-dpi` to view and change an Android display’s DPI. If several devices
are connected, pick one or start with `./adb-dpi -s SERIAL`. The picker and
display screen show the device’s Android name, model, and ADB serial.

Internal displays appear first. Use ↑/↓ to highlight a display, type its new DPI
in the prompt at the bottom, and press Enter to apply it. You can move the
highlight while typing. The list scrolls above the prompt.

Run `./lsrecent-aarch64 [DIR]` (`./lsrecent-amd64` on x86-64) to see what in a
directory changed last, as a cleanup helper. It lists DIR’s entries like `ls
-l`, newest first, where a directory counts as modified when anything inside it
was, and its size is the disk space it uses (as `du` counts it). Symlinks inside
DIR are never followed. It scans in parallel threads and, on a terminal, shows
the table filling in as it goes. `-r` lists oldest first, `-w` shows which file
inside each directory is the newest, `-x` stays on one filesystem, `-j N` sets
the threads, `--bytes` prints exact sizes and `-q` turns the live view off. Both
binaries are built from `lsrecent.c` (the command is at its top).
