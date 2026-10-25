"""Copy the user's original game assets to the selected Android device.

    python tools/push_data.py --serial SERIAL                       # Crusader / Extreme
    python tools/push_data.py --serial SERIAL --edition stronghold  # the original Stronghold HD

Crusader assets go to /sdcard/Stronghold/game and its four PE inputs to
/sdcard/Stronghold/lifted. Stronghold HD has its own installation: its assets
go to /sdcard/Stronghold/stronghold-hd and its PE inputs (Stronghold.exe and
the Mss32/binkw32 shared with Crusader) to /sdcard/Stronghold/lifted-stronghold.
Installer, uninstaller and GOG files are not copied. --verify compares every
copied file's SHA256 on the device with the local original.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
EDITIONS = {
    "crusader": dict(target="/sdcard/Stronghold/game", lifted="/sdcard/Stronghold/lifted",
                     profiles=("userdata", "userdata-classic"),
                     folders=("aiv", "binks", "fx", "gfx", "gm", "maps", "mapsExtreme"),
                     suffixes=(".tex", ".bmp", ".csv", ".cur", ".ani"),
                     inputs=("Stronghold_Crusader_Extreme.exe", "Stronghold Crusader.exe", "Mss32.dll", "binkw32.dll")),
    "stronghold": dict(target="/sdcard/Stronghold/stronghold-hd", lifted="/sdcard/Stronghold/lifted-stronghold",
                       profiles=("userdata-stronghold",),
                       folders=("binks", "fx", "gfx", "gfx8", "gm", "help", "maps", "soundtrack"),
                       suffixes=(".tex", ".mlb", ".ani"),
                       inputs=("Stronghold.exe", "Mss32.dll", "binkw32.dll")),
}


def sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--serial", default=os.environ.get("ANDROID_SERIAL"),
                        help="ADB device serial (default: ANDROID_SERIAL, port.json device, or ADB selection)")
    parser.add_argument("--edition", choices=list(EDITIONS), default="crusader")
    parser.add_argument("--verify", action="store_true", help="compare device SHA256 of every copied file")
    args = parser.parse_args()
    if not (ROOT / "port.json").is_file():
        parser.error("copy port.example.json to port.json and set your original game directories")
    config = json.loads((ROOT / "port.json").read_text(encoding="utf-8"))
    layout = EDITIONS[args.edition]
    game = ROOT / (config["editions"]["stronghold"]["game_directory"] if args.edition == "stronghold"
                   else config["game_directory"])
    inputs = json.loads((ROOT / "config/inputs.json").read_text())
    for name in layout["inputs"]:
        source = ROOT / "generated/input" / name
        if sha256(source) != inputs[name]:
            raise ValueError(f"{name}: lifted input SHA256 mismatch")
    serial = args.serial or config.get("device")
    adb = ["adb"] + (["-s", serial] if serial else [])
    profiles = " ".join(f"/sdcard/Stronghold/{p}" for p in layout["profiles"])
    subprocess.run(adb + ["shell", f"mkdir -p {layout['target']} {layout['lifted']} {profiles}"], check=True)
    files = {}
    for name in layout["folders"]:
        source = game / name
        if not source.is_dir():
            raise FileNotFoundError(source)
        subprocess.run(adb + ["push", "--sync", str(source), layout["target"] + "/"], check=True)
        files.update({f"{layout['target']}/{p.relative_to(game).as_posix()}": p for p in source.rglob("*") if p.is_file()})
    for source in sorted(game.iterdir()):
        if source.is_file() and source.suffix.lower() in layout["suffixes"]:
            subprocess.run(adb + ["push", "--sync", str(source), layout["target"] + "/"], check=True)
            files[f"{layout['target']}/{source.name}"] = source
    for name in layout["inputs"]:
        source = ROOT / "generated/input" / name
        subprocess.run(adb + ["push", "--sync", str(source), layout["lifted"] + "/"], check=True)
        files[f"{layout['lifted']}/{name}"] = source
    if args.verify:
        listing = subprocess.run(adb + ["shell", f"find {layout['target']} {layout['lifted']} -type f -exec sha256sum {{}} +"],
                                 check=True, capture_output=True, text=True, encoding="utf-8").stdout
        device = {}
        for line in listing.splitlines():
            digest, _, path = line.partition("  ")
            device[path.strip()] = digest
        bad = [path for path, source in sorted(files.items()) if device.get(path) != sha256(source)]
        extra = sorted(set(device) - set(files))
        print(f"verified {len(files) - len(bad)}/{len(files)} files ({sum(p.stat().st_size for p in files.values())} bytes); "
              f"{len(extra)} other files on the device")
        if bad:
            raise SystemExit(f"{len(bad)} files differ, first: {bad[:5]}")


if __name__ == "__main__":
    main()
