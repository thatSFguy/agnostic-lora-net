<!--
  Scope reminder: this project is closed to feature requests and feature PRs.
  Accepted PRs are: new board support, bug fixes, security fixes, and docs/test
  improvements that don't grow the feature set. See CONTRIBUTING.md.
  Feature PRs will be closed unmerged.
-->

## What kind of PR is this?

- [ ] **New board support** (the main open lane)
- [ ] Bug fix (behaviour didn't match the docs)
- [ ] Security fix
- [ ] Docs / host-test improvement (no new features)

> If this is a **feature**, please stop here — the project is feature-frozen and the PR
> will be closed. You're very welcome to fork. See [CONTRIBUTING.md](../CONTRIBUTING.md).

## Summary

<!-- What changed and why. Keep board changes confined to the radio HAL seam + board glue;
     don't touch the portable lib/mesh/ core. -->

## New-board attestation (required for board PRs)

**Untested board PRs will not be merged.** Confirm you ran this on the real hardware:

- [ ] I **own this board** and **flashed this firmware** onto it.
- [ ] It **boots** and the console/boot banner is sane.
- [ ] It **joins the mesh** (announces seen, neighbours appear).
- [ ] It does **LoRa RX/TX** with another node (sent and received a message).
- Board / radio: <!-- e.g. Heltec V4 (ESP32-S3 + SX1262) -->
- Firmware commit tested: <!-- short SHA -->

**What I tested vs. didn't** (be honest — gaps are fine, hidden gaps aren't):

<!-- e.g. "Verified RX/TX and mesh join at SF7. Did NOT verify max TX power, the FEM/RF-switch
     control line, or sleep current — no way to measure here." -->

## Tests

- [ ] `pio test -e native` passes
- [ ] `cd controller && go test ./...` passes (if controller touched)
- [ ] Relevant `platformio.ini` env builds (`pio run -e <env>`)
