#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Regenerate the example frames embedded in docs/.
#
# The images under docs/images/ are not decoration: they are what the
# renderer actually draws, produced by running it. Committing them keeps
# the docs readable on GitHub without a build, and regenerating them from
# the renderer keeps them honest.
#
#   scripts/make-doc-images.sh           regenerate and overwrite
#   scripts/make-doc-images.sh --check   fail if the committed images
#                                        differ from what the renderer
#                                        draws now (CI runs this)
#
# The check works because ppm2png is deterministic: same pixels in, same
# bytes out. A diff therefore means the renderer changed, and the images
# (and probably the surrounding prose) need updating.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/docs/images"
TMP="$ROOT/.tmp/doc-images"

CHECK=0
case "${1:-}" in
    --check) CHECK=1 ;;
    "")      ;;
    *)       echo "usage: $0 [--check]" >&2; exit 2 ;;
esac

make -C "$ROOT" build/renderd build/demo build/ppm2png >/dev/null

rm -rf "$TMP"
mkdir -p "$TMP" "$OUT"

# render <name> [extra demo args...]
render() {
    local name="$1"
    shift
    local dir="$TMP/$name"
    local sock="$dir/sock"
    local pid

    mkdir -p "$dir"
    "$ROOT/build/renderd" --unix "$sock" --out "$dir" --once \
        >"$dir/renderd.log" 2>&1 &
    pid=$!
    for _ in $(seq 1 100); do
        [ -S "$sock" ] && break
        sleep 0.05
    done
    "$ROOT/build/demo" --unix "$sock" "$@" >"$dir/demo.log" 2>&1
    wait "$pid"

    [ -f "$dir/frame-000001.ppm" ] || {
        echo "make-doc-images: $name produced no frame; renderd said:" >&2
        cat "$dir/renderd.log" >&2
        exit 1
    }
    "$ROOT/build/ppm2png" "$dir/frame-000001.ppm" "$TMP/$name.png"
}

render scene-basic
render scene-rich --rich

status=0
for name in scene-basic scene-rich; do
    if [ "$CHECK" = 1 ]; then
        if ! cmp -s "$TMP/$name.png" "$OUT/$name.png"; then
            echo "make-doc-images: docs/images/$name.png is out of date" >&2
            status=1
        fi
    else
        cp "$TMP/$name.png" "$OUT/$name.png"
        echo "make-doc-images: wrote docs/images/$name.png"
    fi
done

if [ "$CHECK" = 1 ]; then
    if [ "$status" != 0 ]; then
        echo "make-doc-images: rerun scripts/make-doc-images.sh and commit" \
             "the result" >&2
        exit 1
    fi
    echo "make-doc-images: docs images match the renderer output"
fi
