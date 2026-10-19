#!/bin/sh
# Build the game if needed, then start it. Options go to the game, e.g.
#   ./play.sh --fullscreen
set -e
cd "$(dirname "$0")"
BUILD_DIR=${BUILD_DIR:-build}
BONK_FROM_PLAY=1 ./build.sh
exec "./$BUILD_DIR/bonkheads" "$@"
