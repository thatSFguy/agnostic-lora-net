#!/usr/bin/env python3
# PlatformIO pre-build script: stamp AGN_FW_VERSION from the git state so EVERY build
# self-identifies in the `fw …` banner — a tagged release shows the clean version
# (e.g. 0.12.0), any build past a tag shows tag+commits+sha (0.12.0-3-gabc1234), and a
# build with uncommitted changes is flagged `-dirty`. This kills the "which 0.11.0 am I
# running?" ambiguity that the manual -DAGN_FW_VERSION=… caused.
#
# Note: the banner (info/boot) shows the full string; the telemetry `[status] fw=` field
# is capped at TELEM_FW_MAX=12 chars, so long dev strings truncate THERE only (tagged
# releases like 0.12.0 fit). Wire format unchanged.
import subprocess

Import("env")  # noqa: F821  (injected by PlatformIO/SCons)


def git_version():
    try:
        v = subprocess.check_output(
            ["git", "describe", "--tags", "--always", "--dirty"],
            cwd=env["PROJECT_DIR"],            # noqa: F821
            stderr=subprocess.DEVNULL,
        ).decode().strip()
        return v[1:] if v.startswith("v") else v   # drop the leading 'v' on vX.Y.Z tags
    except Exception:
        return "dev"


ver = git_version()
print("AGN_FW_VERSION (from git) = %s" % ver)
env.Append(CPPDEFINES=[("AGN_FW_VERSION", env.StringifyMacro(ver))])  # noqa: F821
