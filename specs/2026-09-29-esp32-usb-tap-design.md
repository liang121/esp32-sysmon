# ESP32 USB remote tap design

Date: 2026-09-29

## Goal

Let the development Mac tap coordinates on the ESP32 over the already-connected USB console, then capture the resulting screen. Physical touch remains active. This is a development aid for reviewing layouts, not a network control interface or an automation service.

## Design

Add a second LVGL pointer input device backed by a one-tap FreeRTOS queue. `tap x y` in the serial console validates coordinates for the 800×480 screen, enqueues one press/release pair, and waits for an explicit completion notification. The LVGL input callback emits the press and release on successive reads; an LVGL async callback notifies the console after release processing, so an immediate screenshot sees the tapped page without an arbitrary delay. The existing GT911 touch input is unchanged.

Extend `node mac/configure.mjs` with `tap <x> <y>` so the tool can use the same port handling as the existing USB commands. It prints only success or a bounded error, and it does not type text or expose the device over Wi-Fi.

## Validation

Build and flash with NVS preserved. Use the USB command to leave the password overlay and return to it, capturing both resulting screens. Check that physical touch still works and no credential values enter source or logs.
