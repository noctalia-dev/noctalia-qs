#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build-release"

echo "==> Cleaning build directory..."
rm -rf "$BUILD_DIR"

echo "==> Configuring..."
cmake -GNinja \
    -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    "$ROOT_DIR"

echo "==> Building..."
cmake --build "$BUILD_DIR"

echo "==> Installing (requires sudo)..."
sudo cmake --install "$BUILD_DIR"

echo "==> Done. Binary installed to /usr/bin/quickshell"
