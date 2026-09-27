#!/usr/bin/env bash
set -euo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${1:-$project_root/build/vendor-validation}"
mkdir -p "$build_dir"
report="$build_dir/vendor-report.txt"
printf 'VANTIX vendor validation %s\nOS: %s\n' "$(date -Is)" "$(uname -a)" > "$report"
trap 'status=$?; if (( status != 0 )); then printf "FAILED: exit %s\n" "$status" >> "$report"; printf "Validation failed; send %s and build output\n" "$report" >&2; fi' EXIT

cmake_args=(-S "$project_root" -B "$build_dir")
if [[ -n "${VCPKG_ROOT:-}" ]]; then
    cmake_args+=("-DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake")
elif [[ -f "$project_root/.tools/vcpkg/scripts/buildsystems/vcpkg.cmake" ]]; then
    cmake_args+=("-DCMAKE_TOOLCHAIN_FILE=$project_root/.tools/vcpkg/scripts/buildsystems/vcpkg.cmake")
fi
cmake "${cmake_args[@]}"
cmake --build "$build_dir" --config Release --parallel
exe="$build_dir/vantix"
if [[ ! -x "$exe" ]]; then exe="$build_dir/Release/vantix"; fi
if [[ ! -x "$exe" ]]; then printf 'VANTIX executable missing\n' >&2; exit 1; fi

"$exe" --devices-json > "$build_dir/devices.json"
"$exe" --backends-json > "$build_dir/backends.json"
printf 'Devices: %s\n' "$(cat "$build_dir/devices.json")" >> "$report"
printf 'Validated backends: %s\n' "$(cat "$build_dir/backends.json")" >> "$report"

python3 - "$exe" "$build_dir" "$report" <<'PY'
import json
import pathlib
import subprocess
import sys

exe, build_dir, report_path = sys.argv[1:]
build = pathlib.Path(build_dir)
devices = json.loads((build / "devices.json").read_text())["devices"]
backends = json.loads((build / "backends.json").read_text())["backends"]

with open(report_path, "a", encoding="utf-8") as report:
    for device in devices:
        for api in ("CUDA", "HIP"):
            if api not in device["apis"]:
                continue
            matching = [backend for backend in backends if
                        backend["api"] == api and
                        backend["device_id"] == device["id"]]
            if not matching:
                report.write(f"UNAVAILABLE {api} on {device['name']} ({device['id']})\n")
                continue
            report.write(f"Testing {api} on {device['name']} ({device['id']})\n")
            for args in (
                ["--self-test", "--backend", api.lower(), "--device", device["id"]],
                ["--prefix", "ZZ", "--backend", api.lower(), "--device", device["id"],
                 "--max", "128", "--out", str(build / f"smoke-{api.lower()}")],
            ):
                result = subprocess.run([exe, *args], text=True, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT)
                report.write(result.stdout)
                report.flush()
                if result.returncode:
                    raise RuntimeError(f"{api} validation failed: {args}")
    result = subprocess.run([exe, "--benchmark"], text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    report.write(result.stdout)
    if result.returncode:
        raise RuntimeError("Benchmark failed")
PY
printf 'Validation complete; send %s\n' "$report"
