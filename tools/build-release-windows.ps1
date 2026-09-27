param([switch]$SkipBundle)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$desktop = Join-Path $root "desktop"
$tauri = Join-Path $desktop "src-tauri"
$build = Join-Path $root "build/package-windows"
$configPath = Join-Path $tauri "tauri.release.json"
$version = "0.1.0"
if (-not $env:TAURI_SIGNING_PRIVATE_KEY) {
    $localKey = Join-Path $root ".tools/signing/vantix.key"
    if (Test-Path -LiteralPath $localKey) {
        $env:TAURI_SIGNING_PRIVATE_KEY = $localKey
    } elseif (-not $SkipBundle) {
        throw "TAURI_SIGNING_PRIVATE_KEY is required to sign updater artifacts"
    }
}

function Invoke-Checked([string]$Command, [string[]]$Arguments) {
    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Command exited with code $LASTEXITCODE" }
}

function Initialize-Msvc {
    $vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
    if (-not (Test-Path -LiteralPath $vswhere)) { throw "Visual Studio C++ tools are missing" }
    $installation = & $vswhere -latest -products '*' `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or -not $installation) {
        throw "Visual Studio C++ tools are missing"
    }
    $vcvars = Join-Path $installation "VC/Auxiliary/Build/vcvars64.bat"
    if (-not (Test-Path -LiteralPath $vcvars)) { throw "vcvars64.bat is missing" }
    New-Item -ItemType Directory -Force -Path $build | Out-Null
    $helper = Join-Path $build "init-msvc.cmd"
    "@echo off`r`ncall `"$vcvars`" >nul`r`nset`r`n" |
        Set-Content -LiteralPath $helper -Encoding ascii
    foreach ($line in (& cmd.exe /c $helper)) {
        $separator = $line.IndexOf('=')
        if ($separator -gt 0) {
            [Environment]::SetEnvironmentVariable(
                $line.Substring(0, $separator), $line.Substring($separator + 1), "Process")
        }
    }
    if (-not $env:INCLUDE -or -not $env:LIB) {
        throw "Visual Studio C++ environment did not initialize"
    }
}

Initialize-Msvc

$cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
if ($cmakeCommand) {
    $cmake = $cmakeCommand.Source
} else {
    $cmake = Get-ChildItem (Join-Path $root ".tools/vcpkg/downloads/tools") `
        -Recurse -Filter cmake.exe -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match 'cmake-[0-9].*windows.*bin' } |
        Select-Object -First 1 -ExpandProperty FullName
}
if (-not $cmake -or -not (Test-Path -LiteralPath $cmake)) {
    throw "CMake is required to build VANTIX"
}
$toolchain = if ($env:VCPKG_ROOT) {
    Join-Path $env:VCPKG_ROOT "scripts/buildsystems/vcpkg.cmake"
} else {
    Join-Path $root ".tools/vcpkg/scripts/buildsystems/vcpkg.cmake"
}
if (-not (Test-Path -LiteralPath $toolchain)) { throw "vcpkg toolchain is missing" }

$appVersion = (Get-Content (Join-Path $desktop "package.json") -Raw | ConvertFrom-Json).version
$tauriVersion = (Get-Content (Join-Path $tauri "tauri.conf.json") -Raw | ConvertFrom-Json).version
if ($appVersion -ne $version -or $tauriVersion -ne $version) {
    throw "Release version differs between package.json and tauri.conf.json"
}

Push-Location $root
try {
    Invoke-Checked $cmake @("-S", $root, "-B", $build,
        "-DCMAKE_TOOLCHAIN_FILE=$toolchain", "-DVCPKG_TARGET_TRIPLET=x64-windows-static")
    Invoke-Checked $cmake @("--build", $build, "--config", "Release", "--parallel", "8")
    $ctest = Join-Path (Split-Path $cmake) "ctest.exe"
    Invoke-Checked $ctest @("--test-dir", $build, "-C", "Release", "--output-on-failure")
    $exe = Join-Path $build "Release/vantix.exe"
    if (-not (Test-Path -LiteralPath $exe)) { throw "Fresh vantix.exe was not built" }
    Invoke-Checked $exe @("--self-test", "--backend", "cpu")
    Invoke-Checked $exe @("--backends-json")

    $target = (& rustc --print host-tuple).Trim()
    if ($LASTEXITCODE -ne 0 -or -not $target) { throw "Rust target triple is unavailable" }
    $sidecar = Join-Path $tauri "binaries/vantix-$target.exe"
    New-Item -ItemType Directory -Force -Path (Split-Path $sidecar) | Out-Null
    Copy-Item -LiteralPath $exe -Destination $sidecar -Force

    $resources = [ordered]@{
        "../../data/ton-english.txt" = "data/ton-english.txt"
        "../../gpu/vulkan/derive.spv" = "gpu/vulkan/derive.spv"
    }
    foreach ($api in @("cuda", "hip")) {
        $dll = Join-Path $build "Release/vantix_$api.dll"
        if (Test-Path -LiteralPath $dll) {
            $resources["../../build/package-windows/Release/vantix_$api.dll"] = "vantix_$api.dll"
            Write-Host "Including optional $api module"
        }
    }
    @{ bundle = @{ resources = $resources } } |
        ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $configPath -Encoding utf8

    if (-not $SkipBundle) {
        Push-Location $desktop
        try {
            Invoke-Checked "npm" @("run", "tauri", "--", "build", "--config", "src-tauri/tauri.release.json")
        } finally { Pop-Location }
        $installer = Join-Path $tauri "target/release/bundle/nsis/VANTIX_${version}_x64-setup.exe"
        if (-not (Test-Path -LiteralPath $installer)) { throw "NSIS installer is missing" }
        if (-not (Test-Path -LiteralPath "$installer.sig")) { throw "Signed update artifact is missing" }
        Get-FileHash -LiteralPath $installer -Algorithm SHA256 |
            Select-Object Path,Hash | Format-List
    }
} finally {
    Pop-Location
}
