#!/usr/bin/env bash
# Cut a firmware release: tag the current commit vX.Y.Z, rebuild the web/fw packages so
# the version is stamped into them, and print the result. Because AGN_FW_VERSION is taken
# from `git describe` (scripts/git_version.py), the tag IS the version — no file to bump.
#
# Usage:  scripts/release.sh 0.12.0  ["release notes line"]
#
# Preconditions it enforces:
#   - run from a clean working tree (so the build isn't '-dirty')
#   - tag must not already exist
set -euo pipefail

VER="${1:-}"
NOTE="${2:-}"
if [[ -z "$VER" ]]; then echo "usage: scripts/release.sh X.Y.Z [\"notes\"]" >&2; exit 2; fi
if [[ ! "$VER" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then echo "version must be X.Y.Z, got '$VER'" >&2; exit 2; fi
TAG="v$VER"

cd "$(dirname "$0")/.."

if [[ -n "$(git status --porcelain)" ]]; then
    echo "ERROR: working tree is dirty — commit or stash first so the build isn't '-dirty'." >&2
    git status --short >&2
    exit 1
fi
if git rev-parse "$TAG" >/dev/null 2>&1; then
    echo "ERROR: tag $TAG already exists." >&2; exit 1
fi

git tag -a "$TAG" -m "${NOTE:-release $TAG}"
echo "tagged $TAG"

# Rebuild web/fw so the staged packages carry the tagged version (git_version.py reads the
# new tag on this clean, tagged commit -> banner shows exactly $VER).
PATH="$HOME/.platformio/penv/bin:$PATH" bash scripts/refresh_web_fw.sh

echo
echo "released $TAG — web/fw rebuilt at version $VER."
echo "  push the tag:   git push origin $TAG"
echo "  flash nodes from the agnctl Flash tab; the banner will read: fw $VER"
