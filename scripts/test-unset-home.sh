#!/bin/bash
# test-unset-home.sh — Run the native test suite in an isolated temp sandbox.
#
# CBM native tests and helpers derive database/cache/config paths from HOME and
# related XDG variables. Unsetting HOME falls back to /tmp, which can still
# collide with shared local state. Keep this wrapper as the safe local entrypoint
# for Crumbs validation so scripts/test.sh cannot read or write the developer's
# real cache/config/data directories.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

SANDBOX="$(mktemp -d "${TMPDIR:-/tmp}/cbm-test-home.XXXXXX")"
cleanup() {
    rm -rf "$SANDBOX"
}
trap cleanup EXIT INT TERM

export HOME="$SANDBOX/home"
export XDG_CACHE_HOME="$HOME/.cache"
export XDG_CONFIG_HOME="$SANDBOX/xdg-config"
export XDG_DATA_HOME="$SANDBOX/xdg-data"
export XDG_STATE_HOME="$SANDBOX/xdg-state"
export CBM_CACHE_DIR="$XDG_CACHE_HOME/codebase-memory-mcp"

mkdir -p "$HOME" "$XDG_CACHE_HOME" "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" \
    "$XDG_STATE_HOME" "$CBM_CACHE_DIR"

scripts/test.sh "$@"
