---
name: Bug report
about: Something doesn't work as documented
title: ''
labels: bug
assignees: ''
---

**What happened**
A clear description of the bug and what you expected instead.

**Repro steps**
1. …
2. …

**Environment**
- Board(s): <!-- e.g. RAK4631, XIAO nRF52840 + Wio-SX1262, T1000-E, Heltec V4 -->
- Firmware version: <!-- `info` / boot banner, e.g. v0.14.0 -->
- Host OS / toolchain: <!-- e.g. WSL Ubuntu, PlatformIO version -->
- Controller (`agnctl`) involved? If so, Go version.

**Logs / console output**
Paste relevant console (`trace on`), `pio` build output, or `agnctl` logs. Redact any
controller keys or BLE PINs.

> ⚠️ This is **Alpha** software — APIs and the wire format can change between commits.
> A bug against an old version may already be fixed on `main`.
