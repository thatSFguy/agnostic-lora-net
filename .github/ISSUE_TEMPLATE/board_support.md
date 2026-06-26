---
name: New board support
about: Propose a board/radio to support — note that the merge path is a tested PR
title: 'board: '
labels: board
assignees: ''
---

> ⚠️ **Please read first.** This project is **closed to feature requests** — the only open
> contribution lane is **new board support**, and the way support actually lands is a
> **PR that the submitter has tested on the real hardware** (see
> [CONTRIBUTING.md](../CONTRIBUTING.md)). An issue here is fine to flag interest or ask a
> wiring question, but it is **not a request the maintainer will implement** — there's no
> roadmap and no one to validate a board blind. If you have the board, the fastest (and
> only reliable) path is to send the tested PR.

**Board / radio**
- Board: <!-- name + link -->
- MCU: <!-- e.g. nRF52840, ESP32-S3 -->
- LoRa chip: <!-- e.g. SX1262, LR1110 -->
- RF front-end / FEM / RF-switch / TCXO details, if known: <!-- the chip-specific seam -->

**Do you have the board and can you test a PR?**
<!-- This is the deciding factor. A board only gets supported when someone who owns it has
     flashed the firmware and confirmed it boots, joins the mesh, and does LoRa RX/TX. -->

**Anything else**
Prior art, existing PlatformIO board defs, pin maps, links.
