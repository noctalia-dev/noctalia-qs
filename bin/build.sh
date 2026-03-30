#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build-release"

if [ "$1" = "clean" ]; then
    echo "==> Cleaning build directory..."
    rm -rf "$BUILD_DIR"
fi

echo "==> Configuring..."
cmake -GNinja \
    -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_INSTALL_PREFIX=/usr/local \
    "$ROOT_DIR"

echo "==> Building..."
cmake --build "$BUILD_DIR"

echo "==> Installing (requires sudo)..."
sudo cmake --install "$BUILD_DIR"

echo "==> Done. Binary installed to /usr/local/bin/qs"
