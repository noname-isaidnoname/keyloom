# Releases and container testing

## Artifacts

`scripts/package.sh source|tar|deb|rpm|appimage` writes to `build/artifacts`.
Set `KEYLOOM_PACKAGE_OUT`, `KEYLOOM_PACKAGE_BUILD` and `JOBS` to override the
output, build directory and parallelism. The build directory must be writable.
Use a clean checkout: `source` archives **HEAD**, not uncommitted changes, while
binary packages build the working tree. Install CMake, Ninja, a C++20 compiler,
pkg-config and the X11/XCB/libpng headers. DEB requires dpkg tools and `file`; RPM requires
rpmbuild; AppImage requires `appimagetool`, `file`, `mksquashfs` (squashfs-tools) and
`rsvg-convert` (librsvg2-bin on Debian/Ubuntu).
Set `APPIMAGE_RUNTIME` to a local type-2 runtime to prevent appimagetool from
downloading its default runtime. The AppImage
bundles non-glibc shared libraries; the loader, glibc and desktop services come
from the host. It requires the same input permissions and optional external
desktop programs as the native executable. Native packages use dependency
metadata; they deliberately do not set a system-wide file association.

Official binaries target x86-64 and are built on Ubuntu 24.04, so require glibc
2.39 or newer. The binary tarball needs X11, XCB, libpng and the C++ runtime
installed separately. Other architectures and older systems should build from
source. SHA256SUMS detects corruption, not malicious replacements; no signing
or independent provenance verification is currently provided.

## Publishing

The release workflow runs nightly on the default branch and by manual dispatch.
Nightly moves the `nightly` tag only after a successful build and replaces the
prerelease, explicitly warning that it may be untested. Manual dispatch accepts
an `X.Y.Z` matching `CMakeLists.txt`; it creates a new `vX.Y.Z` tag and release
at the selected ref, and refuses an existing tag or release. Run it only from a
trusted ref. GitHub's `GITHUB_TOKEN` creates the release as github-actions[bot];
repository Actions settings must allow Actions and contents write permission.
The build job has read-only permissions; only the publication job can write.
The separate CI workflow runs on pushes and pull requests with read-only
permissions. Both workflows cache compiler outputs; the release workflow uploads intermediate artifacts for
seven days. It checks the downloaded appimagetool against a pinned SHA-256;
the upstream continuous assets (tool and runtime) may change, in which case
updating their verified hashes is an intentional maintenance task. Nightly replacement briefly removes
the previous release; a failed publish may require rerunning the workflow.
Review changes, licenses, dependencies, and secrets before opening the repo;
release artifacts are not a substitute for independent security review.

## Podman for workflow work

Use rootless Podman and a disposable container, not host package installs. Pull
an image once and reuse its cached layers. Mount source read-only and a separate
ignored output directory writable; on SELinux hosts use appropriate `:z`/`:Z`
labels or a dedicated copy (relabeling a shared checkout can affect the host).
Do not mount credentials, SSH agents, `/dev/input`, or the Docker socket.
Limit `JOBS` to avoid memory pressure; use persistent named caches for package
downloads or ccache only when their trust and ownership are understood. Pin
image digests when reproducibility matters. Containers share the host kernel and
headless smoke tests cannot validate actual input injection or desktop GUI.

For example, from a clean checkout, build and test the DEB in Ubuntu:

```sh
mkdir -p build/podman
podman run --rm -v "$PWD:/src:ro" -v "$PWD/build/podman:/out:rw" ubuntu:24.04 bash -ec '
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq
  apt-get install -y build-essential cmake ninja-build pkg-config file libx11-dev libxcb1-dev libpng-dev
  KEYLOOM_PACKAGE_BUILD=/out/build KEYLOOM_PACKAGE_OUT=/out/artifacts JOBS=2 /src/scripts/package.sh deb
  apt-get install -y /out/artifacts/*.deb
  keyloom --version
  keyloom --check /src/examples/two_spots.loom
'
```

Run the resulting RPM in Fedora with `dnf install /out/artifacts/*.rpm` and
the tarball or AppImage in Arch with their runtime dependencies installed.
Use `APPIMAGE_EXTRACT_AND_RUN=1` inside containers rather than exposing FUSE.
Run the full suite with `make` as well; integration tests may report skipped
without Xvfb, desktop helpers or uinput access. Testing a GitHub Actions YAML
locally does not emulate token permissions, release APIs or hosted-runner
network policy: validate those on a trusted GitHub repository before relying
on publishing. Use `podman images`, `podman system df` and targeted
`podman image rm` to inspect and reclaim disk; avoid indiscriminate pruning of
other projects' images and volumes.
