# SPDX-License-Identifier: GPL-3.0-or-later
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true

$keychain = Join-Path $env:RUNNER_TEMP qtkeychain
$tdlib = Join-Path $env:RUNNER_TEMP tdlib
$prefix = Join-Path $env:RUNNER_TEMP phoschat-dependencies
$licenses = Join-Path $env:RUNNER_TEMP phoschat-runtime-licenses
New-Item -ItemType Directory $licenses | Out-Null

git clone --depth 1 --branch $env:QTKEYCHAIN_TAG https://github.com/frankosterfeld/qtkeychain.git $keychain
cmake -S $keychain -B "$keychain/build" -G Ninja -DCMAKE_BUILD_TYPE=Release `
    -DBUILD_SHARED_LIBS=OFF -DBUILD_TRANSLATIONS=OFF "-DCMAKE_INSTALL_PREFIX=$prefix"
cmake --build "$keychain/build" --target install --parallel 2
Copy-Item "$keychain/COPYING" "$licenses/QtKeychain.txt"

$vcpkg = $env:VCPKG_INSTALLATION_ROOT
if (!$vcpkg) { throw 'VCPKG_INSTALLATION_ROOT is missing from the runner.' }
& "$vcpkg/vcpkg.exe" install --classic gperf:x64-windows openssl:x64-windows-static zlib:x64-windows-static
$env:PATH = "$vcpkg/installed/x64-windows/tools/gperf;$env:PATH"
foreach ($dependency in 'openssl', 'zlib') {
    $share = "$vcpkg/installed/x64-windows-static/share/$dependency"
    Copy-Item "$share/copyright" "$licenses/$dependency.txt"
    if (Test-Path "$share/vcpkg.spdx.json") {
        Copy-Item "$share/vcpkg.spdx.json" "$licenses/$dependency.spdx.json"
    }
}

git init $tdlib
git -C $tdlib fetch --depth 1 https://github.com/tdlib/td.git $env:TDLIB_REF
git -C $tdlib checkout --detach FETCH_HEAD
cmake -S $tdlib -B "$tdlib/build" -G Ninja -DCMAKE_BUILD_TYPE=Release `
    "-DCMAKE_TOOLCHAIN_FILE=$vcpkg/scripts/buildsystems/vcpkg.cmake" `
    -DVCPKG_TARGET_TRIPLET=x64-windows-static -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded `
    -DTD_ENABLE_LTO=OFF -DTD_INSTALL_STATIC_LIBRARIES=OFF
cmake --build "$tdlib/build" --target tdjson --parallel 2

$tdjson = Get-ChildItem "$tdlib/build" -Filter tdjson.dll -Recurse | Select-Object -First 1
if (!$tdjson) { throw 'TDLib build did not produce tdjson.dll.' }
"PHOSCHAT_DEPENDENCY_PREFIX=$prefix" >> $env:GITHUB_ENV
"PHOSCHAT_TDLIB_DLL=$($tdjson.FullName)" >> $env:GITHUB_ENV
"PHOSCHAT_TDLIB_LICENSE=$tdlib/LICENSE_1_0.txt" >> $env:GITHUB_ENV
"PHOSCHAT_WINDOWS_LICENSE_DIR=$licenses" >> $env:GITHUB_ENV
