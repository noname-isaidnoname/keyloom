#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

# Build and install the keyloom binary.
#
#   ./scripts/install.sh                 # build (release) and install
#   PREFIX="$HOME/.local" ./scripts/install.sh
#
# The prefix defaults to /usr/local. Pass PREFIX to install elsewhere; no
# root or sudo is attempted automatically.

PREFIX="${PREFIX:-/usr/local}"
BUILD_DIR="build/release"

missing=()
for tool in cmake ninja pkg-config; do
    command -v "$tool" >/dev/null 2>&1 || missing+=("$tool")
done
if ! pkg-config --exists x11 2>/dev/null; then missing+=("libx11-dev (x11)"); fi
if ! pkg-config --exists xcb 2>/dev/null; then missing+=("libxcb1-dev (xcb)"); fi
if ! pkg-config --exists libpng 2>/dev/null; then missing+=("libpng-dev (libpng)"); fi
if (( ${#missing[@]} )); then
    cat >&2 <<EOF
Missing build dependencies: ${missing[*]}

Debian/Ubuntu:
    sudo apt install cmake ninja-build pkg-config libx11-dev libxcb1-dev libpng-dev
Arch:
    sudo pacman -S cmake ninja pkgconf libx11 libxcb libpng

Runtime (not needed to build): ydotool + ydotoold, or a writable /dev/uinput.
EOF
    exit 2
fi

if ! pkg-config --exists gtk+-wayland-3.0 wayland-client; then
    echo 'Note: GTK3/wayland-client development files are missing; native Wayland panels will be unavailable.' >&2
fi

if [[ ! -d "$PREFIX" ]] && ! mkdir -p "$PREFIX" 2>/dev/null; then
    printf 'Prefix %s does not exist and could not be created.\n' "$PREFIX" >&2
    printf 'Install into your own prefix instead:\n    PREFIX="$HOME/.local" ./scripts/install.sh\n' >&2
    exit 2
fi
if [[ ! -w "$PREFIX" ]]; then
    printf 'Prefix %s is not writable by %s.\n' "$PREFIX" "$USER" >&2
    printf 'Install into your own prefix instead:\n    PREFIX="$HOME/.local" ./scripts/install.sh\n' >&2
    exit 2
fi

echo "Configuring (release)..."
cmake --preset release -DBUILD_TESTING=OFF >/dev/null
echo "Building..."
cmake --build "$BUILD_DIR" -j"$(nproc)"
echo "Installing into $PREFIX..."
cmake --install "$BUILD_DIR" --prefix "$PREFIX"

bin_dir="$PREFIX/bin"
printf '\nKeyloom installed to %s/keyloom\n' "$bin_dir"
case ":$PATH:" in
    *":$bin_dir:"*) ;;
    *) printf 'Note: %s is not on your PATH.\n' "$bin_dir" ;;
esac
printf 'Try: keyloom --check examples/two_spots.loom\n'
printf 'Run: keyloom --run examples/two_spots.loom\n'
