# Build and install

For release downloads, packaging scripts, and container-based verification see
[releases.md](releases.md).

## Prerequisites

- C++20 toolchain: Clang 16 or newer (the reference compiler) or GCC 13 or newer
- CMake ≥ 3.25
- Ninja
- pkg-config
- X11, XCB and libpng development headers
- Optional for native Wayland panels: GTK3 and wayland-client development headers

Debian/Ubuntu:

```bash
sudo apt install cmake ninja-build pkg-config clang libx11-dev libxcb1-dev libpng-dev libgtk-3-dev libwayland-dev
```

Arch:

```bash
sudo pacman -S cmake ninja pkgconf clang libx11 libxcb libpng gtk3 wayland
```

Runtime (not needed to build): `ydotool`/`ydotoold`, or a writable
`/dev/uinput`, and one of the screenshot tools for `--get-mouse-pos`.

## Build presets

`CMakePresets.json` defines the configure/build/test presets:

| Preset | Build type | Tests | Notes |
| --- | --- | --- | --- |
| `release` | Release | yes | Ninja, ccache, mold when installed, the compiler CMake picks |
| `release-clang` | Release | yes | Clang, the reference compiler |
| `release-gcc` | Release | yes | GCC |
| `debug` | Debug | yes | Ninja, ccache, mold when installed |
| `sanitize` | Debug | yes | ASan + UBSan, hardening disabled |

```bash
cmake --preset release
cmake --build build/release -j$(nproc)
ctest --preset release

cmake --preset release-clang
cmake --build build/release-clang -j$(nproc)
ctest --preset release-clang

cmake --preset sanitize
cmake --build build/sanitize -j$(nproc)
ctest --preset sanitize
```

The `sanitize` test preset sets `ASAN_OPTIONS=detect_leaks=1:strict_string_checks=1`
and `UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1`.

## Makefile targets

```bash
make              # configure, build (Release) and run the test suite
make build        # configure and build Release only, no test suite
make debug        # configure and build Debug, with the test suite
make test         # configure, build Release and run ctest
make check        # same as make test
make install      # build Release and install into PREFIX (default /usr/local)
make package      # Arch: build the package with makepkg
make clean        # delete the build directory
```

Variables: `JOBS=8`, `PREFIX=/usr/local`, `BUILD=build/release`.

## CMake options

| Option | Default | Effect |
| --- | --- | --- |
| `BUILD_TESTING` | ON (presets) | Build the test harness, probes and integration tests |
| `LOOMWRIGHT_USE_MOLD` | ON | Link with `mold` when it is installed |
| `LOOMWRIGHT_REQUIRE_REFERENCE_COMPILER` | OFF | Fail the configure step unless Clang is the C++ compiler |
| `LOOMWRIGHT_LTO` | ON | Link with link-time optimization when the linker can read the bitcode |
| `LOOMWRIGHT_SANITIZE` | OFF | Build with `-fsanitize=address,undefined` |
| `LOOMWRIGHT_HARDEN` | ON | `-fstack-protector-strong`, `-D_FORTIFY_SOURCE=3`, `-fPIE` |
| `LOOMWRIGHT_WAYLAND` | ON | Build native Wayland panels if GTK3 and wayland-client are available |
| `LOOMWRIGHT_DESKTOP_INTEGRATION` | ON | Install the desktop entry, icon, MIME type and terminal launcher |

`LOOMWRIGHT_SANITIZE` and `LOOMWRIGHT_HARDEN` are mutually exclusive in practice: the
sanitize preset turns hardening off because the sanitizers replace the
allocator and libc's own checks add nothing.

`LOOMWRIGHT_LTO` only applies to a release build, and only when the linker can
read the bitcode: GCC's driver loads its own plugin, while Clang needs `mold`
or `lld`. When neither is available the configure step says that link-time
optimization is disabled and the build works without it.

## Compiler

Clang is the reference compiler: the release artifacts are built with it, and
it produces the fastest runtime build of the two supported toolchains
([benchmark.md](benchmark.md)). GCC 13 or newer stays supported and is built by
the CI workflow as well. The configure step rejects a compiler that is too old
for the C++20 features the runtime uses, and
`LOOMWRIGHT_REQUIRE_REFERENCE_COMPILER=ON` makes it reject everything but Clang.

To build with a specific compiler, use the matching preset or set the usual
environment variables:

```bash
cmake --preset release-clang     # build/release-clang
cmake --preset release-gcc       # build/release-gcc
CC=clang CXX=clang++ cmake --preset release
```

ccache is used automatically when `CMAKE_CXX_COMPILER_LAUNCHER` is not set
explicitly and `ccache` is on `PATH`.

CMake detects GTK3 and wayland-client at configure time. Without their
development files the X11 backend still builds, while explicit Wayland panels
fail with an installation hint. Install the headers and reconfigure to enable
them. Official releases build with both dependencies.

## Install

```bash
./scripts/install.sh                     # release build, install into /usr/local
PREFIX="$HOME/.local" ./scripts/install.sh
```

`scripts/install.sh` checks for cmake/ninja/pkg-config and the x11/xcb/libpng
development packages, refuses a non-writable default prefix with a
`PREFIX="$HOME/.local"` hint, and never runs sudo. It configures the `release`
preset with `BUILD_TESTING=OFF`, builds, and runs `cmake --install`.
Extra CMake arguments go into `LOOMWRIGHT_CMAKE_ARGS`, for example
`LOOMWRIGHT_CMAKE_ARGS=-DLOOMWRIGHT_DESKTOP_INTEGRATION=OFF` to install the binary
without the desktop integration files.

The install honours `GNUInstallDirs`: the binary goes into
`CMAKE_INSTALL_BINDIR` (normally `bin`). `make install` and the Arch package
both use the same layout.

With `LOOMWRIGHT_DESKTOP_INTEGRATION=ON` (the default) the install also adds the
desktop integration files from `packaging/common/`:

| File | Destination |
| --- | --- |
| `io.github.noname_isaidnoname.loomwright.desktop` | `share/applications/` |
| `loomwright.svg` | `share/icons/hicolor/scalable/apps/` |
| `loomwright.xml` | `share/mime/packages/` |
| `loomwright-terminal` | `bin/` |

Turn the option off (`-DLOOMWRIGHT_DESKTOP_INTEGRATION=OFF`) to install the
binary alone.

## Desktop integration

Every packaged build (DEB, RPM, tarball, AppImage, Arch) ships the same
desktop entry, icon and MIME type, so Loomwright appears in the application menu
with its icon and `*.loom` files can be opened from a file manager:

- `io.github.noname_isaidnoname.loomwright.desktop` declares `Icon=loomwright`, `MimeType=application/x-loomwright;`
  and `Exec=loomwright-terminal %f`.
- `loomwright.svg` is a scalable icon in the `hicolor` theme, so it follows the
  desktop's icon size and colour scheme.
- `loomwright.xml` registers the `application/x-loomwright` MIME type for `*.loom`.
- `loomwright-terminal` opens a terminal emulator and runs the script inside it,
  so the output stays visible after the file manager launches it. It detects
  the terminal at runtime: `$TERMINAL` (colon-separated, per the freedesktop
  terminal spec) first, then common emulators such as konsole, kgx and
  gnome-terminal. Without a script it starts `loomwright` with no arguments, which
  prints the help text. It can also be used directly:
  `loomwright-terminal [--] [SCRIPT] [loomwright args...]`.

The DEB and RPM packages refresh the desktop, icon and MIME caches from their
post-install scripts; the Arch package does the same in `post_install()`.

A plain `make install` into a user-writable prefix (`PREFIX="$HOME/.local"`)
also installs these files, but the desktop only picks them up when the
matching XDG directories are on `XDG_DATA_DIRS` and `PATH`.

## Arch package

```bash
cd packaging/arch
makepkg -si                                   # build and install
makepkg -si LOOMWRIGHT_BUILD_TESTS=ON            # also build and run the test suite
```

`packaging/arch/PKGBUILD` builds from the local checkout (`LOOMWRIGHT_SOURCE_DIR`,
default `../..`); it needs no network and no release tarball. `pkgver` must
stay in sync with `project(loomwright VERSION ...)` in `CMakeLists.txt`. `check()`
only runs `ctest` when `LOOMWRIGHT_BUILD_TESTS=ON` is passed to `makepkg`.

The package also registers a file association: `*.loom` files get the
`application/x-loomwright` MIME type, and `io.github.noname_isaidnoname.loomwright.desktop` is the default
handler, so double-clicking a script in a file manager runs it. The desktop
entry, the scalable `hicolor` icon, the MIME type and the
`/usr/bin/loomwright-terminal` launcher come from the shared
`packaging/common/` files that CMake installs (see
[Desktop integration](#desktop-integration)); the package only adds the
system-wide default handler in `/etc/xdg/mimeapps.list` on top of them.

## Runtime permissions

- Reading physical input needs read access to `/dev/input/event*` (the
  `input` group or a narrow udev rule). Log out/in after group changes.
- Pointer motion, scrolling and key consumption need write access to
  `/dev/uinput`. If it is missing, `sudo modprobe uinput` may help when the
  kernel supports it.
- Buttons, keys and text through `ydotool` need `ydotoold` running. The socket
  can be overridden with `YDOTOOL_SOCKET`.

Do not run Loomwright as root to bypass permissions.

## Environment assumptions

The code must not assume a session type, compositor, distro or package
manager. Detect at runtime (`--doctor`, `choose_backend()`, the picker's
X11/XWayland path) and keep docs generic. Label session- and distro-specific
steps explicitly, as the Arch sections above do.

`--doctor` reports the detected environment; `--devices` lists readable input
devices; `--screen` prints the detected virtual desktop size.
