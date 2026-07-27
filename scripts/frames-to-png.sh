#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Convert every PPM frame under DIR to a PNG beside it.
#
# renderd writes PPM because it is trivial to verify byte by byte, but
# nothing displays PPM. CI runs this before uploading its artifacts so
# the frames can be looked at straight from the run page, and it is handy
# by hand after a local test run:
#
#   scripts/frames-to-png.sh .tmp/e2e
#
# Existing PNGs are overwritten. The PPMs are left in place — they are
# still the thing the tests assert on.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR="${1:?usage: $0 DIR}"

[ -d "$DIR" ] || { echo "frames-to-png: no such directory: $DIR" >&2; exit 1; }
make -C "$ROOT" build/ppm2png >/dev/null

n=0
while IFS= read -r -d '' ppm; do
    "$ROOT/build/ppm2png" "$ppm" "${ppm%.ppm}.png"
    n=$((n + 1))
done < <(find "$DIR" -type f -name '*.ppm' -print0)

echo "frames-to-png: converted $n frame(s) under $DIR"
