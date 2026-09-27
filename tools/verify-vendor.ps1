param([string]$BuildDirectory = "build/vendor-validation")

$ErrorActionPreference = "Stop"
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$buildPath = Join-Path $projectRoot $BuildDirectory
$report = Join-Path $buildPath "vendor-report.txt"
$toolchain = if ($env:VCPKG_ROOT) {
    Join-Path $env:VCPKG_ROOT "scripts/buildsystems/vcpkg.cmake"
} else {
    Join-Path $projectRoot ".tools/vcpkg/scripts/buildsystems/vcpkg.cmake"
}
if (-not (Test-Path -LiteralPath $toolchain)) {
    throw "vcpkg не найден. Установите его или задайте VCPKG_ROOT."
}
$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $cmake) {
    $bundled = Get-ChildItem (Join-Path $projectRoot ".tools/vcpkg/downloads/tools") `
        -Recurse -Filter cmake.exe -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match 'cmake-[0-9].*windows.*bin' } |
        Select-Object -First 1 -ExpandProperty FullName
    if (-not (Test-Path -LiteralPath $bundled)) { throw "CMake не найден." }
    $cmakePath = $bundled
} else {
    $cmakePath = $cmake.Source
}

Push-Location $projectRoot
try {
    New-Item -ItemType Directory -Force -Path $buildPath | Out-Null
    "VANTIX vendor validation $(Get-Date -Format o)" | Set-Content -LiteralPath $report
    "OS: $([System.Environment]::OSVersion.VersionString)" | Add-Content -LiteralPath $report
    "CMake: $cmakePath" | Add-Content -LiteralPath $report
    $configureOutput = & $cmakePath -S . -B $buildPath "-DCMAKE_TOOLCHAIN_FILE=$toolchain" 2>&1
    $configureOutput | Tee-Object -FilePath $report -Append
    if ($LASTEXITCODE -ne 0) { throw "CMake configure завершился с ошибкой." }
    $buildOutput = & $cmakePath --build $buildPath --config Release --parallel 8 2>&1
    $buildOutput | Tee-Object -FilePath $report -Append
    if ($LASTEXITCODE -ne 0) { throw "Сборка завершилась с ошибкой." }
    $exe = Join-Path $buildPath "Release/vantix.exe"
    if (-not (Test-Path -LiteralPath $exe)) { $exe = Join-Path $buildPath "vantix.exe" }
    if (-not (Test-Path -LiteralPath $exe)) { throw "vantix.exe не найден после сборки." }

    $devices = & $exe --devices-json
    if ($LASTEXITCODE -ne 0) { throw "Определение устройств завершилось с ошибкой." }
    "Devices: $devices" | Add-Content -LiteralPath $report
    $backends = & $exe --backends-json
    if ($LASTEXITCODE -ne 0) { throw "Проверка движков завершилась с ошибкой." }
    "Validated backends: $backends" | Add-Content -LiteralPath $report
    $validated = ($backends | ConvertFrom-Json).backends
    foreach ($device in ($devices | ConvertFrom-Json).devices) {
        foreach ($api in @("cuda", "hip")) {
            if ($device.apis -notcontains $api) { continue }
            $backend = $validated | Where-Object {
                $_.api -ieq $api -and $_.device_id -eq $device.id
            } | Select-Object -First 1
            if (-not $backend) {
                "UNAVAILABLE $api on $($device.name) ($($device.id)): SDK, module or GPU compatibility check failed" |
                    Add-Content -LiteralPath $report
                continue
            }
            "Testing $api on $($device.name) ($($device.id))" | Add-Content -LiteralPath $report
            $selfTest = & $exe --self-test --backend $api --device $device.id 2>&1
            $selfTest | Add-Content -LiteralPath $report
            if ($LASTEXITCODE -ne 0) { throw "$api self-test завершился с ошибкой." }
            $smoke = & $exe --prefix ZZ --backend $api --device $device.id --max 128 `
                --out (Join-Path $buildPath "smoke-$api") 2>&1
            $smoke | Add-Content -LiteralPath $report
            if ($LASTEXITCODE -ne 0) { throw "$api короткий поиск завершился с ошибкой." }
        }
    }
    $speed = & $exe --benchmark 2>&1
    $speed | Add-Content -LiteralPath $report
    if ($LASTEXITCODE -ne 0) { throw "Бенчмарк завершился с ошибкой." }
    Write-Host "Проверка завершена. Отправьте файл: $report"
} catch {
    if (Test-Path -LiteralPath $report) {
        "FAILED: $($_.Exception.Message)" | Add-Content -LiteralPath $report
        Write-Error "Проверка не пройдена. Отправьте файл: $report"
    }
    throw
} finally {
    Pop-Location
}
