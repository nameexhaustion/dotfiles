# dotfiles

Run `./adb-dpi` to view and change an Android display’s DPI. If several devices
are connected, pick one or start with `./adb-dpi -s SERIAL`. The picker and
display screen show the device’s Android name, model, and ADB serial.

Internal displays appear first. Use ↑/↓ to highlight a display, type its new DPI
in the prompt at the bottom, and press Enter to apply it. You can move the
highlight while typing. The list scrolls above the prompt.
