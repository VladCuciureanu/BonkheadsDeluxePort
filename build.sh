#!/bin/sh
# Build the port on macOS or Linux. To build and start the game in one go,
# use ./play.sh instead.
#   ./build.sh                                   configure and build into build/
#   ./build.sh -DBONKHEADS_SIT=/path/to/archive  extra arguments go to CMake
# BUILD_DIR (default: build) and BUILD_TYPE (default: RelWithDebInfo) can be overridden.
set -e
cd "$(dirname "$0")"
BUILD_DIR=${BUILD_DIR:-build}
BUILD_TYPE=${BUILD_TYPE:-RelWithDebInfo}

missing() {
    echo "error: $1 not found. $2" >&2
    exit 1
}
command -v cmake >/dev/null 2>&1 ||
    missing "CMake" "Install it (macOS: brew install cmake; Debian/Ubuntu: sudo apt-get install cmake)."
command -v cc >/dev/null 2>&1 || command -v gcc >/dev/null 2>&1 || command -v clang >/dev/null 2>&1 ||
    missing "A C compiler" "Install one (macOS: xcode-select --install; Debian/Ubuntu: sudo apt-get install build-essential)."

# Prefer Ninja for a new build folder; an existing one keeps its generator.
if [ ! -f "$BUILD_DIR/CMakeCache.txt" ] && command -v ninja >/dev/null 2>&1; then
    set -- -G Ninja "$@"
fi
cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" "$@"
cmake --build "$BUILD_DIR" --parallel
if [ -z "$BONK_FROM_PLAY" ]; then
    echo
    echo "Build complete. Start the game with ./play.sh or ./$BUILD_DIR/bonkheads"
fi
