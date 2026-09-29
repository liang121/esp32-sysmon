# ESP32 USB screenshot plan

1. Add a narrow LVGL capture API that copies the displayed RGB565 framebuffer into PSRAM under the LVGL lock. Verify it identifies the inactive draw buffer and handles allocation failure.
2. Add the serial `screenshot` response with fixed header, byte count, and CRC32. Keep existing config/status commands intact.
3. Add a dependency-free Node command that reads one frame, verifies its checksum, and writes a private PNG to an explicit path. Test PNG pixels and corruption handling.
4. Build, flash without erasing NVS, capture a real screen, visually inspect it, and document the command. Audit and push source from the current local branch; keep the captured PNG outside Git.
