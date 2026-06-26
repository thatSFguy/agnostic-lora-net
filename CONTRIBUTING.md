# Contributing

This is a single-maintainer project built for **my own use**, shared under the MIT license
so others can use, fork, and build on it. Please read the posture before opening anything:

> **The scope is closed.** The feature set covers what I need, and the design goal from here
> is **security and the smallest possible attack surface** — not new functionality.
> **I am not taking feature requests, and I won't merge feature PRs.** Feature issues will
> be closed with a pointer to this file. None of this is unfriendly — you're very welcome to
> fork and take it wherever you like.

## What contributions *are* welcome

1. **New board support** — the one open lane. See the rule below.
2. **Bug fixes** — for behaviour that doesn't match the docs.
3. **Security reports** — see [SECURITY.md](SECURITY.md). Please don't open a public issue
   for a vulnerability.
4. **Docs and host-test improvements** — as long as they don't grow the feature surface.

## New-board PRs: you must have tested it on the real board

Boards cannot be validated blind — FEM/RF-switch wiring, TCXO, and the TX path bite in ways
that don't show up at compile time. So the rule is simple:

- **A board PR must come from someone who has the board in hand and has run it.**
  Untested board PRs will not be merged.
- **Say what you tested and what you didn't.** At minimum confirm: it boots, it joins the
  mesh, and it sends/receives over LoRa. Call out anything you couldn't verify (TX power,
  FEM/RF-switch, sleep) so the gaps are on the record. The
  [pull request template](.github/PULL_REQUEST_TEMPLATE.md) walks through this.
- **What a board PR contains:** a variant under `variants/` + an `include/board_config.h`
  block + a `platformio.ini` env. The radio HAL is the only chip-specific seam — keep the
  change confined to that seam and the board glue.

## Keep the core portable and unchanged

`lib/mesh/` builds for both the host and the MCU with **no Arduino dependencies** — keep it
that way so the unit tests stay meaningful. A board PR should not need to touch the portable
core; if you think it does, open a bug first so we can keep the seam clean.

## Run the tests before you push

- `pio test -e native` — the portable routing/codec/ARQ/SAR core.
- `cd controller && go test ./...` — the Go control plane.
- CI runs the build matrix in [`.github/workflows/build.yml`](.github/workflows/build.yml).

No formal CLA. By contributing, you agree your contributions are licensed under the
repository's [MIT License](LICENSE). Be kind; assume good faith.
