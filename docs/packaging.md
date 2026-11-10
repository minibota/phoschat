# Packaging PhosChat

The x86-64 release publishes a Debian 13 DEB, Fedora 45 RPM, a Windows
NSIS installer, and a Flatpak. The CI workflow includes a macOS build and test target. Release packaging
currently targets Windows and Linux.

| Package | Install |
| --- | --- |
| Debian 13 | `sudo apt install ./phoschat_0.2.0_amd64.deb` |
| Fedora 45 | `sudo dnf install --nogpgcheck ./phoschat-0.2.0-*.x86_64.rpm` |
| Windows x64 | Run `phoschat-0.2.0-windows-x64-setup.exe` |
| Flatpak | `flatpak install --user ./phoschat-0.2.0-x86_64.flatpak` |

The RPM and Windows installer are unsigned. Check the downloads against
`SHA256SUMS` on the release page before installing. Flatpak uses the KDE 6.10
runtime from Flathub and keeps its local profile inside the application sandbox.

## Native package builds

Configure and compile with the normal Qt development dependencies and the
CPack generator for the target system. Debian needs `dpkg-dev`. Fedora
needs `rpm-build`. Windows needs NSIS 3.03+ and the Qt desktop SDK.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPHOSCHAT_ENABLE_PACKAGING=ON
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
cpack --config build/CPackConfig.cmake -G DEB -B dist
```

On Fedora use `-DCMAKE_INSTALL_LIBDIR=lib64` at configure time and generate
`RPM`. On Windows generate `NSIS`. CMake invokes Qt's deployment tool when
staging the Windows installer so it contains the Qt DLLs and platform plugins.
The Debian installer recommends the distribution's TDLib JSON package. The
other release jobs stage TDLib 1.8.70 from the pinned upstream revision.
To reproduce those builds, consult [the release workflow](../.github/workflows/release.yml).
For a native RPM or EXE build pass `PHOSCHAT_TDLIB_RUNTIME` and
`PHOSCHAT_TDLIB_LICENSE` at configure time to include a separately built TDLib
shared library and its license.

## Flatpak build

```sh
flatpak remote-add --user --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
flatpak install --user flathub org.kde.Platform//6.10 org.kde.Sdk//6.10
flatpak-builder --user --force-clean --disable-rofiles-fuse --repo=repo build-flatpak \
  packaging/flatpak/io.phoschat.desktop.json
flatpak build-bundle --runtime-repo=https://dl.flathub.org/repo/flathub.flatpakrepo \
  repo phoschat-0.2.0-x86_64.flatpak io.phoschat.desktop
```

## GitHub releases

Change `packaging/release-version` and the CMake project version together.
Pushing that change to `main` builds all four packages, runs installation smoke
tests, and publishes a tagged prerelease with checksums. A matching tag push also
triggers the release. For a package-only preview, run **Package and release**
manually with **Publish** disabled. A manual release from `main` uses the same
checks and version gate.

The matching GitHub tag contains the corresponding source. Packages include
the project license and applicable dependency notices. See
[third-party notices](../packaging/THIRD-PARTY-NOTICES.md).
