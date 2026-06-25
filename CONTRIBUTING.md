# Contributing

Thanks for considering a contribution — **PRs are very welcome.** This is an Alpha,
single-maintainer project, and it gets better with more hands and more radios.

The full guidance lives in the **[Contributing section of the README](README.md#contributing)**;
the essentials:

- **Good first contributions:** new board support (add a `variants/` pin-map + a
  `include/board_config.h` block + a `platformio.ini` env — the radio HAL is the only
  chip-specific seam), docs fixes, and host-test coverage under `test/`.
- **Keep the core portable.** `lib/mesh/` builds for both the host and the MCU with **no
  Arduino dependencies** — keep it that way so the unit tests stay meaningful.
- **Run the tests before you push:**
  - `pio test -e native` — the portable routing/codec/ARQ/SAR core.
  - `cd controller && go test ./...` — the Go control plane.
  - CI runs the build matrix in [`.github/workflows/build.yml`](.github/workflows/build.yml).
- **New boards you can't fully bench-test:** that's fine — say so in the PR, and call out
  what you did and didn't verify (FEM/RF-switch details especially).

No formal CLA. For anything substantial, open an issue to discuss first; otherwise just
send the PR. Be kind; assume good faith.

By contributing, you agree your contributions are licensed under the repository's
[MIT License](LICENSE).
