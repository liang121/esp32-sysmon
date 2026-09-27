# ESP32 Mac system monitor

Source for a Waveshare ESP32-S3-Touch-LCD-4.3C display and its macOS data collector. The display has two swipeable pages:

- **System:** CPU and per-core usage, memory pressure and usage, swap activity, recent history, and top processes.
- **AI usage:** Claude Code and Codex five-hour and weekly usage windows, reset countdowns, and a manual refresh button.

The ESP32 reads `GET /api/now` from the Mac over the local network once a second. The Mac collector uses a native Swift sampler for system metrics, the Claude Code credential in macOS Keychain, and a locally installed Codex CLI for usage data. Tokens are read at runtime and are not stored in this repository. The Claude Code usage endpoint is undocumented and can return HTTP 429; the display may then show cached values and an error.

## Layout

- `firmware-idf/`: current ESP-IDF firmware, including display and touch components and pinned component dependencies. The Wi-Fi and Mac settings live in the device's NVS, outside the firmware source.
- `mac/`: Swift sampler, Node server, web status page, serial configuration utility, and optional LaunchAgent installer.
- `firmware/sysmon/`: older Arduino sketch and board support sources, retained for reference. For the two-page display, build `firmware-idf/`.

Build outputs, compiled binaries, generated SDK configuration, device flash backups, local LaunchAgent files, and account credentials are excluded.

## Build and run

Prerequisites: macOS with Swift (`swiftc`) and Node.js 18+; Espressif ESP-IDF 5.5.x with `idf.py` for the firmware. Dependencies in `firmware-idf/main/idf_component.yml` are downloaded by the ESP-IDF component manager. The board needs 16 MB flash and octal PSRAM, as specified in `sdkconfig.defaults`.

```sh
cd firmware-idf
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

Replace the port with the one found by `arduino-cli board list` or `ls /dev/cu.usbmodem*`. Flashing changes device firmware; existing Wi-Fi settings are in NVS and the partition layout in `partitions.csv` preserves their location.

In another terminal, start the Mac collector:

```sh
node mac/server.mjs
```

It builds `mac/sampler` with `swiftc` as needed and serves a browser page at `http://localhost:8787/`. To start it automatically on login, run `node mac/install-agent.mjs`. The server binds to the local network; use it on a trusted LAN.

With the ESP32 connected over USB, configure its Wi-Fi and Mac address once:

```sh
node mac/configure.mjs
node mac/configure.mjs status
```

The status command reports network connection, resolved host, and last request result. On the display, swipe horizontally between the two pages; tap **REFRESH** on the AI usage page to request updated usage. The collector needs a signed-in Codex CLI with its account in `~/.codex-usage` and a Claude Code credential in the macOS Keychain for both usage sections to populate.
