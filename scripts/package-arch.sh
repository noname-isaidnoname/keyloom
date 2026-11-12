#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
out=${LOOMWRIGHT_PACKAGE_OUT:-"$root/build/artifacts"}
if [[ ${EUID:-$(id -u)} -eq 0 ]]; then
    echo 'makepkg must run as an unprivileged user; install dependencies separately' >&2
    exit 1
fi
command -v makepkg >/dev/null || { echo 'makepkg is required (build this package on Arch Linux)' >&2; exit 1; }
version=$(sed -n 's/^[[:space:]]*VERSION \([0-9][0-9.]*\)$/\1/p' "$root/CMakeLists.txt" | head -n1)
pkgver=$(sed -n 's/^pkgver=\([0-9][0-9.]*\)$/\1/p' "$root/packaging/arch/PKGBUILD" | head -n1)
[[ $version =~ ^[0-9]+\.[0-9]+\.[0-9]+$ && $pkgver == "$version" ]] || {
    echo 'CMake and Arch package versions must match X.Y.Z' >&2; exit 1;
}
mkdir -p "$out"
out=$(cd "$out" && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/loomwright-arch.XXXXXXXX")
trap 'rm -rf -- "$work"' EXIT
cp "$root/packaging/arch/PKGBUILD" "$work/PKGBUILD"
cd "$work"
# Build from the checkout but keep makepkg's writable workspace separate.
# Dependencies must already be installed; this script never escalates privileges.
LOOMWRIGHT_SOURCE_DIR="$root" PKGDEST="$out" makepkg --noconfirm --clean
