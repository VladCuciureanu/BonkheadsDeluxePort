#!/bin/sh
# Configure and build on macOS / Linux. Extra arguments go to CMake, e.g.
#   ./build.sh -DBONKHEADS_SIT=/path/to/Bonkheads_Deluxe.SIT
# BUILD_DIR (default: build) and BUILD_TYPE (default: RelWithDebInfo) can be overridden.
set -e
cd "$(dirname "$0")"
BUILD_DIR=${BUILD_DIR:-build}
BUILD_TYPE=${BUILD_TYPE:-RelWithDebInfo}
if command -v ninja >/dev/null 2>&1; then
    set -- -G Ninja "$@"
fi
cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" "$@"
cmake --build "$BUILD_DIR" --parallel
