# ESP32 USB screenshot design

Date: 2026-09-29

## Goal

Capture the pixels currently drawn on the attached 800×480 ESP32 display as a local PNG so UI layouts can be reviewed before calling them complete. This is a USB-only development command; it does not add a screen control, network endpoint, cloud upload, or persistent image on the device.

## Design

The existing RGB panel uses two full-screen RGB565 framebuffers with LVGL full-refresh rendering. Under the LVGL mutex, copy the buffer opposite `lv_display_get_buf_active()` into PSRAM. LVGL swaps the active draw buffer after flushing, so the opposite buffer holds the last displayed frame. Release the mutex before USB transmission, keeping the UI responsive while bytes are sent.

The serial console accepts `screenshot` and replies with `FRAME RGB565 <width> <height> <byte-count> <crc32>\n`, exactly that many raw bytes, then `\nEND FRAME\n`. The header uses the text console; the raw frame bypasses its byte-by-byte newline translation and uses the USB-Serial-JTAG driver directly. A Mac-side Node tool reads the frame, checks dimensions and CRC32, converts little-endian RGB565 to RGB PNG using only Node built-ins, and writes a caller-selected path with mode 0600. Other serial log text before the frame is ignored; interleaved text during the binary payload fails the checksum instead of silently producing a misleading screenshot.

The output PNG stays outside the public repository by default. It may contain whatever the screen shows, including a password if the user chose to reveal it. Firmware and Node source contain no screenshot content or local configuration values.

## Validation

Test CRC and PNG conversion on known RGB565 pixels, build the ESP-IDF firmware, flash while preserving NVS, capture an actual frame over USB, inspect its resolution/colors/content, and verify the repository contains no generated PNG. A photo remains useful for physical clipping, backlight, and touch alignment.
