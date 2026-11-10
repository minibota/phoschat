# Packaging PhosChat

The x86-64 release publishes a Debian 13 DEB, Fedora 45 RPM, a Windows
NSIS installer, and a Flatpak. The macOS preview job compiles the app and
runs the IRC parser, plain loopback and the other suites. Trusted IRC TLS
with Secure Transport and signed macOS packaging remain future work.

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
CPack generator for the target system. Debian needs `dpkg-dev` and `file`. Fedora
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

### Telegram application identity

New official package builds require two repository Actions secrets, configured once by the PhosChat maintainer in [GitHub repository settings](https://github.com/minibota/phoschat/settings/secrets/actions).

| Secret name | Value |
| --- | --- |
| `TELEGRAM_API_ID` | The application's positive integer API ID from my.telegram.org |
| `TELEGRAM_API_HASH` | The matching 32-character hexadecimal API hash |

The release workflow validates their presence before building packages. It maps them to `PHOSCHAT_TELEGRAM_API_ID` and `PHOSCHAT_TELEGRAM_API_HASH` only for the relevant build steps. CMake reads the environment directly into a private generated header, without placing values in cache entries or compiler arguments. The Flatpak helper stages an ignored, private JSON file; its generated manifest contains only a file path. Private inputs are removed after the build and are not installer assets. Do not commit these values, pass them through shell command-line flags, or paste them into issues or logs.

Application credentials embedded in a desktop executable can be extracted. They identify PhosChat to Telegram; they are not user authentication tokens or encryption keys. Every user still authorizes their own Telegram account, and local session keys come from the operating system's random generator. Pull-request CI never receives the real application secrets; its configured-build checks use synthetic values. Do not export Flatpak build sources or debug bundles from a directory containing the private inputs; distribute corresponding source from the clean repository tag.

For a local configured build, set the two `PHOSCHAT_TELEGRAM_*` environment variables through your local secret tooling before configuring CMake. `-DPHOSCHAT_REQUIRE_TELEGRAM_APP_CREDENTIALS=ON` rejects missing values. Ordinary source builds can omit both variables and use the advanced per-account fields. The published v0.2.0 binaries retain the old manual setup until a new release is built with the repository secrets.

### Publishing

Change `packaging/release-version` and the CMake project version together.
Pushing that change to `main` builds all four packages, runs installation smoke
tests, and then attempts to publish a tagged prerelease with checksums. A matching tag push also
triggers the release. For a package-only preview, run **Package and release**
manually with **Publish** disabled. A manual release from `main` uses the same
checks and version gate.

If GitHub denies release creation to the Actions integration, a repository
maintainer can reuse the four successful installer artifacts. Download them from
that same run, put the installers in one directory, and generate `SHA256SUMS`
with `sha256sum`. Use an authenticated GitHub CLI session to publish the
prerelease and attach the four packages and `SHA256SUMS`. Target the exact
commit shown on the packaging run so the tag and the built source match. If that
commit has a different workflow from `main`, publish against an authorized
release branch at the same commit, such as `release/v0.2.0`. [GitHub release API](https://docs.github.com/en/rest/releases/releases#create-a-release)

The matching GitHub tag contains the corresponding source. Packages include
the project license and applicable dependency notices. See
[third-party notices](../packaging/THIRD-PARTY-NOTICES.md).
