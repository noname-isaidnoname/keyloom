#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
build=${KEYLOOM_PACKAGE_BUILD:-"$root/build/package"}
out=${KEYLOOM_PACKAGE_OUT:-"$root/build/artifacts"}
version=$(sed -n 's/^[[:space:]]*VERSION \([0-9][0-9.]*\)$/\1/p' "$root/CMakeLists.txt" | head -n1)
[[ $version =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo 'Invalid project version' >&2; exit 1; }
mkdir -p "$out"
out=$(cd "$out" && pwd)
arch=$(uname -m)

case ${1:-} in
    source)
        # Release archives are generated from the checked-out commit, not ignored
        # local files or uncommitted changes.
        committed_version=$(git -C "$root" show HEAD:CMakeLists.txt | sed -n 's/^[[:space:]]*VERSION \([0-9][0-9.]*\)$/\1/p' | head -n1)
        [[ $committed_version == "$version" ]] || { echo 'Commit the version change before creating the source archive' >&2; exit 1; }
        git -C "$root" archive --format=tar --prefix="keyloom-$version/" HEAD | gzip -n > "$out/keyloom-$version-source.tar.gz"
        ;;
    deb|rpm|tar)
        case $1 in deb) generator=DEB; ext=deb;; rpm) generator=RPM; ext=rpm;; tar) generator=TGZ; ext=tar.gz;; esac
        cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DBUILD_TESTING=OFF
        cmake --build "$build" --parallel "${JOBS:-2}"
        cpack --config "$build/CPackConfig.cmake" -G "$generator" -B "$build/cpack"
        cp "$build/cpack/keyloom-$version-$arch.$ext" "$out/"
        ;;
    appimage)
        command -v appimagetool >/dev/null || { echo 'appimagetool is required' >&2; exit 1; }
        cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DBUILD_TESTING=OFF
        cmake --build "$build" --parallel "${JOBS:-2}"
        appdir=$(mktemp -d "$build/appdir.XXXXXXXX")
        trap 'rm -rf -- "$appdir"' EXIT
        chmod 755 "$appdir"
        mkdir -p "$appdir/usr/bin" "$appdir/usr/lib"
        install -m755 "$build/keyloom" "$appdir/usr/bin/keyloom"
        install -Dm644 "$root/LICENSE" "$appdir/usr/share/licenses/keyloom/LICENSE"
        # Bundle non-glibc shared libraries. The host still supplies its loader,
        # glibc, kernel and desktop services; see the documented baseline.
        while read -r lib; do
            case $(basename "$lib") in libc.so.*|libm.so.*|libpthread.so.*|libdl.so.*|librt.so.*|ld-linux*.so*) continue;; esac
            cp -L "$lib" "$appdir/usr/lib/$(basename "$lib")"
        done < <(ldd "$build/keyloom" | awk '/=> \/.* \(/ {print $3}')
        install -m755 "$root/packaging/appimage/AppRun" "$appdir/AppRun"
        install -m644 "$root/packaging/appimage/keyloom.desktop" "$appdir/keyloom.desktop"
        install -m644 "$root/packaging/common/keyloom.svg" "$appdir/keyloom.svg"
        rsvg-convert -w 256 -h 256 "$appdir/keyloom.svg" -o "$appdir/keyloom.png"
        args=()
        if [[ -n ${APPIMAGE_RUNTIME:-} ]]; then args+=(--runtime-file "$APPIMAGE_RUNTIME"); fi
        ARCH="$arch" APPIMAGE_EXTRACT_AND_RUN=1 appimagetool "${args[@]}" "$appdir" "$out/keyloom-$version-$arch.AppImage"
        ;;
    *) echo "Usage: $0 {source|tar|deb|rpm|appimage}" >&2; exit 2;;
esac
