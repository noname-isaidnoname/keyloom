# Releases and container testing

## Artifacts

`scripts/package.sh source|tar|deb|rpm|appimage` and, on Arch,
`scripts/package-arch.sh` write to `build/artifacts`.
Set `KEYLOOM_PACKAGE_OUT` to override either script's output directory.
For `package.sh`, `KEYLOOM_PACKAGE_BUILD` and `JOBS` override its build
directory and parallelism. The build directory must be writable.
Use a clean checkout: `source` archives **HEAD**, not uncommitted changes, while
binary packages build the working tree. Install CMake, Ninja, a C++20 compiler,
pkg-config and the X11/XCB/libpng headers; install GTK3/wayland-client headers to
include native Wayland panels. DEB requires dpkg tools and `file`; RPM requires
rpmbuild; AppImage requires `appimagetool`, `file`, `mksquashfs` (squashfs-tools) and
`rsvg-convert` (librsvg2-bin on Debian/Ubuntu).
Set `APPIMAGE_RUNTIME` to a local type-2 runtime to prevent appimagetool from
downloading its default runtime. The AppImage
bundles non-glibc shared libraries; the loader, glibc and desktop services come
from the host. It requires the same input permissions and optional external
desktop programs as the native executable. Native packages use dependency
metadata. The Arch package also installs the existing `.loom` desktop
association and system-wide default; DEB and RPM do not.

Official binaries target x86-64. The Arch package is built on current Arch;
the other binaries are built on Ubuntu 24.04, so require glibc 2.39 or newer.
The binary tarball needs X11, XCB, libpng, GTK3, wayland-client and the C++ runtime
installed separately. Other architectures and older systems should build from
source. SHA256SUMS detects corruption, not malicious replacements; no signing
or independent provenance verification is currently provided.

All packages ship the same desktop integration files from `packaging/common/`:
the application entry with its scalable `hicolor` icon, the
`application/x-keyloom` MIME type and the `keyloom-terminal` launcher. The DEB
and RPM packages refresh the desktop, icon and MIME caches from their
post-install scripts; the Arch package does the same in `post_install()`. The
Arch package additionally registers the system-wide default handler in
`/etc/xdg/mimeapps.list`; DEB and RPM rely on the desktop's own association
handling. The AppImage carries the icon and desktop entry inside the image.

## Publishing

The release workflow runs nightly on the default branch and by manual dispatch.
Nightly moves the `nightly` tag only after a successful build and replaces the
prerelease, explicitly warning that it may be untested. Manual dispatch reads
`X.Y.Z` from `CMakeLists.txt` at the selected ref, accepts optional Markdown
release notes, and creates a new `vX.Y.Z` tag and release. It refuses an
existing tag or release. Run it only from a
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

### Dispatching a release

1. Set the new version in `CMakeLists.txt` and `packaging/arch/PKGBUILD`, run
   the tests and package checks, commit, and push the trusted ref. Existing
   version tags and releases are never overwritten by manual dispatch.
2. In GitHub Actions, choose **Build and publish releases**, select the ref,
   enter the optional release description in Markdown, and run the workflow.
   The description becomes the release body, followed by download guidance.
   If omitted, a short versioned introduction and download guidance are used.
3. Alternatively, prepare a Markdown file and dispatch it with GitHub CLI:

   ```sh
   gh workflow run release.yml --ref main --field description=@release-notes.md
   gh run list --workflow release.yml --limit 5
   ```

   For an initial release, summarize the project's main capabilities,
   platform support, known limitations, and installation prerequisites rather
   than relying on an empty generated changelog. For subsequent releases,
   describe the changes since the previous version. Review the notes before
   dispatch; the action publishes them verbatim.

   A concise starting point for the Markdown file is:

   ```markdown
   ## Highlights
   - Describe the important features or changes in this version.

   ## Before running
   - Describe supported platforms, required permissions, and known limitations.
   ```

   The workflow appends platform-specific download guidance and the source
   commit automatically; include only claims verified for the release.
4. Check that both the Ubuntu and Arch jobs and the publishing job pass. On
   the new release, verify the source, tarball, DEB, RPM, AppImage, Arch package,
   and `SHA256SUMS` assets; `sha256sum -c SHA256SUMS` checks the entire set.
   The Arch job installs its package in a clean container and checks the CLI.
   Publishing occurs only after both build jobs succeed.

For a local Arch build, install `base-devel`, CMake, Ninja, pkgconf, X11/XCB,
libpng, GTK3 and Wayland, then run `scripts/package-arch.sh` **without root**.
The script does not install dependencies or use sudo. Set
`KEYLOOM_PACKAGE_OUT` for an alternate output directory and
`CMAKE_BUILD_PARALLEL_LEVEL` to limit compilation.

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
  apt-get install -y build-essential cmake ninja-build pkg-config file libx11-dev libxcb1-dev libpng-dev libgtk-3-dev libwayland-dev
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
