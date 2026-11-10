# Build and install

## Prerequisites

- C++20 toolchain (GCC or Clang)
- CMake ≥ 3.25
- Ninja
- pkg-config
- X11, XCB and libpng development headers

Debian/Ubuntu:

```bash
sudo apt install cmake ninja-build pkg-config libx11-dev libxcb1-dev libpng-dev
```

Arch:

```bash
sudo pacman -S cmake ninja pkgconf libx11 libxcb libpng
```

Runtime (not needed to build): `ydotool`/`ydotoold`, or a writable
`/dev/uinput`, and one of the screenshot tools for `--get-mouse-pos`.

## Build presets

`CMakePresets.json` defines three configure/build/test presets:

| Preset | Build type | Tests | Notes |
| --- | --- | --- | --- |
| `release` | Release | yes | Ninja, ccache, mold when installed |
| `debug` | Debug | yes | Ninja, ccache, mold when installed |
| `sanitize` | Debug | yes | ASan + UBSan, hardening disabled |

```bash
cmake --preset release
cmake --build build/release -j$(nproc)
ctest --preset release

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
| `KEYLOOM_USE_MOLD` | ON | Link with `mold` when it is installed |
| `KEYLOOM_SANITIZE` | OFF | Build with `-fsanitize=address,undefined` |
| `KEYLOOM_HARDEN` | ON | `-fstack-protector-strong`, `-D_FORTIFY_SOURCE=3`, `-fPIE` |

`KEYLOOM_SANITIZE` and `KEYLOOM_HARDEN` are mutually exclusive in practice: the
sanitize preset turns hardening off because the sanitizers replace the
allocator and libc's own checks add nothing.

ccache is used automatically when `CMAKE_CXX_COMPILER_LAUNCHER` is not set
explicitly and `ccache` is on `PATH`.

## Install

```bash
./scripts/install.sh                     # release build, install into /usr/local
PREFIX="$HOME/.local" ./scripts/install.sh
```

`scripts/install.sh` checks for cmake/ninja/pkg-config and the x11/xcb/libpng
development packages, refuses a non-writable default prefix with a
`PREFIX="$HOME/.local"` hint, and never runs sudo. It configures the `release`
preset with `BUILD_TESTING=OFF`, builds, and runs `cmake --install`.

The install honours `GNUInstallDirs`: the binary goes into
`CMAKE_INSTALL_BINDIR` (normally `bin`). `make install` and the Arch package
both use the same layout.

## Arch package

```bash
cd packaging/arch
makepkg -si                                   # build and install
makepkg -si KEYLOOM_BUILD_TESTS=ON            # also build and run the test suite
```

`packaging/arch/PKGBUILD` builds from the local checkout (`KEYLOOM_SOURCE_DIR`,
default `../..`); it needs no network and no release tarball. `pkgver` must
stay in sync with `project(keyloom VERSION ...)` in `CMakeLists.txt`. `check()`
only runs `ctest` when `KEYLOOM_BUILD_TESTS=ON` is passed to `makepkg`.

## Runtime permissions

- Reading physical input needs read access to `/dev/input/event*` (the
  `input` group or a narrow udev rule). Log out/in after group changes.
- Pointer motion, scrolling and key consumption need write access to
  `/dev/uinput`. If it is missing, `sudo modprobe uinput` may help when the
  kernel supports it.
- Buttons, keys and text through `ydotool` need `ydotoold` running. The socket
  can be overridden with `YDOTOOL_SOCKET`.

Do not run Keyloom as root to bypass permissions.

## Environment assumptions

The code must not assume a session type, compositor, distro or package
manager. Detect at runtime (`--doctor`, `choose_backend()`, the picker's
X11/XWayland path) and keep docs generic. Label session- and distro-specific
steps explicitly, as the Arch sections above do.

`--doctor` reports the detected environment; `--devices` lists readable input
devices; `--screen` prints the detected virtual desktop size.
