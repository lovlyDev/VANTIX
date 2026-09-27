"""Build Tauri's static updater manifest from signed release assets."""

import argparse
import json
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("assets", type=Path)
    parser.add_argument("version")
    parser.add_argument("repository")
    args = parser.parse_args()
    if not args.version.startswith("v") or args.version.count(".") != 2:
        parser.error("version must be a v-prefixed semantic version")
    if args.repository != "lovlyDev/VANTIX":
        parser.error("unexpected release repository")

    files = {
        "windows-x86_64": list(args.assets.glob("VANTIX_*_x64-setup.exe")),
        "linux-x86_64": list(args.assets.glob("*.AppImage")),
    }
    platforms = {}
    for platform, candidates in files.items():
        if len(candidates) != 1:
            parser.error(f"expected exactly one {platform} updater bundle")
        bundle = candidates[0]
        signature = bundle.with_name(bundle.name + ".sig")
        if not signature.is_file():
            parser.error(f"missing signature: {signature}")
        platforms[platform] = {
            "url": f"https://github.com/{args.repository}/releases/download/{args.version}/{bundle.name}",
            "signature": signature.read_text(encoding="utf-8").strip(),
        }

    manifest = {"version": args.version.removeprefix("v"), "platforms": platforms}
    (args.assets / "latest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print("Updater manifest covers:", ", ".join(platforms))


if __name__ == "__main__":
    main()
