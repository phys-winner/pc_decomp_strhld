"""Check that the port's native libraries and Java classes need no Android API above the manifest's minSdk.

Native: every undefined dynamic symbol of each ELF must be exported by the NDK's stub libraries for that API level
(or by another ELF of the set). Java: every android.* / java.* member the compiled classes reference is looked up in
the platform's api-versions.xml; a member newer than the minimum is reported with the methods that use it, so that
each use can be compared with the list of guarded uses below.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]

# Uses behind a Build.VERSION.SDK_INT check or in a class that is only entered behind one, each reviewed by hand.
# Key: "owner" or "owner.member"; value: the API level the guard requires and the classes that hold such a guard.
# A use of the same member in any other class is reported.
GUARDED = {
    # CrashMonitor.ExitHistory is entered only with SDK_INT >= 30 (>= 31 for the tombstone).
    "android/app/ActivityManager.getHistoricalProcessExitReasons": (30, {"CrashMonitor$ExitHistory"}),
    "android/app/ApplicationExitInfo": (30, {"CrashMonitor$ExitHistory", "CrashMonitor"}),
    # All-files access on Android 11+.
    "android/os/Environment.isExternalStorageManager": (30, {"LauncherActivity"}),
    "android/content/pm/PackageInfo.getLongVersionCode": (28, {"Diagnostics"}),
    # Cutouts live in Theme.Cutout, entered with SDK_INT >= 28.
    "android/view/WindowManager$LayoutParams.layoutInDisplayCutoutMode": (28, {"Theme$Cutout"}),
    "android/view/WindowInsets.getDisplayCutout": (28, {"Theme$Cutout"}),
    "android/view/WindowInsets.consumeDisplayCutout": (28, {"Theme$Cutout"}),
    "android/view/DisplayCutout": (28, {"Theme$Cutout"}),
    "android/view/Window.setNavigationBarDividerColor": (28, {"Theme"}),
    "android/view/View.setForceDarkAllowed": (29, {"Theme", "GameActivity"}),
    "android/view/Window.setStatusBarContrastEnforced": (29, {"Theme"}),
    "android/view/Window.setNavigationBarContrastEnforced": (29, {"Theme"}),
    "android/view/Window.setDecorFitsSystemWindows": (30, {"Theme", "GameActivity"}),
    "android/view/WindowInsets.getInsets": (30, {"Theme", "FpsOverlay"}),
    "android/view/WindowInsets$Type": (30, {"Theme", "FpsOverlay"}),
    "android/view/WindowInsets.CONSUMED": (30, {"Theme"}),
    "android/graphics/Insets": (30, {"Theme", "FpsOverlay"}),
    # Shared SystemInfo (port.launcher) and the port's Gpu, guarded where used.
    "android/os/PowerManager.getCurrentThermalStatus": (29, {"SystemInfo"}),
    "android/os/PowerManager.getThermalHeadroom": (30, {"SystemInfo"}),
    "android/app/GameManager": (31, {"SystemInfo"}),
    "android/os/Build.SOC_MANUFACTURER": (31, {"SystemInfo", "Gpu"}),
    "android/os/Build.SOC_MODEL": (31, {"SystemInfo"}),
}


def native(elfs, ndk, api):
    tools = ndk / "toolchains/llvm/prebuilt/windows-x86_64"
    readelf = tools / "bin/llvm-readelf.exe"
    stubs = tools / f"sysroot/usr/lib/aarch64-linux-android/{api}"

    def symbols(path, defined):
        out = subprocess.run([str(readelf), "--dyn-syms", "-W", str(path)], capture_output=True, text=True, check=True).stdout
        names = set()
        for line in out.splitlines():
            parts = line.split()
            if len(parts) < 8 or not parts[0].endswith(":") or parts[3] not in ("FUNC", "OBJECT", "NOTYPE", "TLS"):
                continue
            if (parts[6] != "UND") == defined and parts[4] != "LOCAL":
                names.add((parts[7].split("@")[0], parts[4]))
        return names

    available = set()
    for stub in sorted(stubs.glob("*.so")):
        if stub.read_bytes()[:4] != b"ELF":
            continue   # libc++.so is a linker script
        available |= {name for name, _ in symbols(stub, True)}
    for elf in elfs:
        available |= {name for name, _ in symbols(elf, True)}
    problems = []
    for elf in elfs:
        for name, binding in sorted(symbols(elf, False)):
            if name and name not in available and binding != "WEAK":
                problems.append(f"{elf.name}: {name} is not in the API {api} NDK libraries")
    return problems


def java(classes, android_platform, javap, api):
    since = {}
    for cls in ET.parse(android_platform / "data/api-versions.xml").getroot():
        owner = cls.attrib["name"]
        base = int(cls.attrib.get("since", "1"))
        since[owner] = base
        for member in cls:
            if member.tag in ("method", "field"):
                name = member.attrib["name"].split("(")[0]
                level = int(member.attrib.get("since", base))
                key = owner + "." + name
                since[key] = min(since.get(key, level), level)   # overloads: the oldest one
    files = sorted(classes.rglob("*.class"))
    problems, guarded = [], set()
    reference = re.compile(r"=\s+(Methodref|InterfaceMethodref|Fieldref|Class)\s+\S+\s+//\s+\"?([\w/$\[;]+?)\"?(?:\.\"?([\w$<>]+)\"?:\S+)?$")
    for start in range(0, len(files), 40):
        out = subprocess.run([str(javap), "-v", "-p", *map(str, files[start:start + 40])],
                             capture_output=True, text=True, errors="replace", check=True).stdout
        current = ""
        for line in out.splitlines():
            if line.startswith("Classfile "):
                current = Path(line.split()[1]).stem
                continue
            found = reference.search(line)
            if not found:
                continue
            owner, member = found.group(2).lstrip("[L").rstrip(";"), found.group(3)
            if not owner.startswith(("android/", "java/", "javax/", "dalvik/", "org/json/")):
                continue
            for key in ([owner + "." + member] if member else []) + [owner]:
                level = since.get(key)
                if level is None:
                    continue
                if level <= api:
                    continue
                guard = GUARDED.get(key) or GUARDED.get(owner)
                if guard and level <= guard[0] and current in guard[1]:
                    guarded.add(f"{current}: {key} (API {level}, guard {guard[0]})")
                else:
                    problems.append(f"{current}: {key} needs API {level}")
    print(f"Java: {len(files)} classes, {len(guarded)} reviewed guarded uses of newer APIs")
    return sorted(set(problems))


def main():
    import build_android
    import build_apk
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--api", type=int, default=build_android.API)
    parser.add_argument("--classes", type=Path, default=build_apk.OUT / "classes")
    parser.add_argument("elf", nargs="*", type=Path)
    args = parser.parse_args()
    elfs = args.elf or sorted((ROOT / "build/arm64").glob("*.so")) + [
        ROOT / "build/gog-android/libinnoextract.so", ROOT / "build/gog-android/libstronghold_xz.so"]
    problems = native([elf for elf in elfs if elf.is_file()], build_android.NDK, args.api)
    if args.classes.is_dir():
        problems += java(args.classes, build_apk.ANDROID_JAR.parent, build_apk.JAVA_HOME / "bin/javap.exe", args.api)
    else:
        print(f"no classes at {args.classes}: Java check skipped")
    for problem in problems:
        print("PROBLEM ", problem)
    print(f"API {args.api}: {len(problems)} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
