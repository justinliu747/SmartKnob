# Notes

Short log of Plan mode work that actually landed in the repo. Newest first.

## 2026-08-17 — HID serial F+M and double-Space

- HID keyboard advertising is always on from boot (no serial toggle).
- Serial `b` sends an F+M chord; `s` sends two Space taps for iPad shortcut recording.

## 2026-08-17 — Toggleable BLE HID next to volume GATT

- Serial `b` toggles HID keyboard advertising; volume GATT ads stay on.
- Allows two BLE connections so iPhone HID and Windows volume-bridge can both stay up.

## 2026-08-16 — Volume mapping, real-time updates, and limit remap

- Replaced HID volume keys with a NimBLE status/volume service (absolute percent).
- On connect, Windows volume remaps the detent 0–100% walls onto the current pose (motor does not spin).
- Added volume_bridge.py GUI: mode, detent, volume bar.

## 2026-08-16 — BLE volume knob

- ESP32-S3 advertises as a BLE HID keyboard named SmartKnob after FOC init.
- Profile 2 detent clicks send Windows volume up/down while paired.
- Switched to HijelHID_BLEKeyboard after ESP32-BLE-Keyboard crashed on Arduino-ESP32 3.x connect.

## 2026-08-16 — Plan execution notes

- Added this file and a Cursor rule to append a brief entry after each executed plan.
