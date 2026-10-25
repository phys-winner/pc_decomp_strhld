"""Package the three ARM64 game editions into an APK (no Gradle) and optionally install it.

    python tools/build_apk.py [--install] [--serial SERIAL]
The libraries come from tools/build_android.py --shared --edition extreme/classic/stronghold;
game data, PE images and separate profiles live under /sdcard/Stronghold.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SDK = Path(os.environ.get("ANDROID_SDK") or os.environ.get("ANDROID_HOME") or
           os.environ.get("ANDROID_SDK_ROOT") or Path.home() / "AppData/Local/Android/Sdk")
TOOLS = SDK / "build-tools/36.1.0"
ANDROID_JAR = SDK / "platforms/android-36/android.jar"
JAVA_HOME = Path(os.environ.get("JAVA_HOME", ""))
# Parallel workstreams package into their own directories (see build_android.py).
OUT = ROOT / os.environ.get("STRONGHOLD_APK_DIR", "build/android-game")
ADB = SDK / "platform-tools/adb.exe"


def run(*args, **kwargs):
    result = subprocess.run([str(a) for a in args], capture_output=True, text=True, errors="replace", **kwargs)
    if result.returncode:
        print(result.stdout[-3000:], result.stderr[-3000:])
        raise SystemExit(f"{Path(str(args[0])).name} failed ({result.returncode})")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--install", action="store_true")
    parser.add_argument("--serial", default=os.environ.get("ANDROID_SERIAL") or os.environ.get("VOID_SERIAL"))
    parser.add_argument("--skip-native", action="store_true")
    parser.add_argument("--global-regs", action="store_true", help="build_android.py upstream mode (see there)")
    parser.add_argument("--volatile-memory", action="store_true", help="build_android.py upstream mode (see there)")
    args = parser.parse_args()
    if not os.environ.get("JAVA_HOME"):
        parser.error("set JAVA_HOME to a JDK installation")
    for tool in (TOOLS / "aapt2.exe", TOOLS / "d8.bat", TOOLS / "zipalign.exe",
                 TOOLS / "apksigner.bat", ANDROID_JAR, JAVA_HOME / "bin/javac.exe",
                 JAVA_HOME / "bin/keytool.exe"):
        if not tool.is_file():
            parser.error(f"missing build tool: {tool}; check ANDROID_HOME and JAVA_HOME")
    modes = [(flag, name) for flag, name, on in (("--global-regs", "globalregs", args.global_regs),
                                                 ("--volatile-memory", "volatilemem", args.volatile_memory)) if on]
    if not args.skip_native:
        for edition in ("extreme", "classic", "stronghold"):
            subprocess.run([sys.executable, str(ROOT / "tools/build_android.py"), "--shared", "--jobs", "12",
                            "--edition", edition, *(flag for flag, _ in modes)], check=True)
    base = os.environ.get("STRONGHOLD_BUILD_DIR", "build/arm64")
    native = ROOT / (base + "-" + "-".join(name for _, name in modes) if modes else base)
    libraries = [native / name for name in ("libstronghold.so", "libstronghold_classic.so", "libstronghold_sh.so")]
    for library in libraries:
        if not library.is_file():
            raise SystemExit(f"{library}: build all three editions before packaging")
    if OUT.exists():
        OUT.resolve().relative_to(ROOT.resolve())
        shutil.rmtree(OUT)
    (OUT / "classes").mkdir(parents=True)
    (OUT / "dex").mkdir()
    run(TOOLS / "aapt2.exe", "compile", "--dir", ROOT / "android/app/res", "-o", OUT / "resources.zip")
    run(TOOLS / "aapt2.exe", "link", "-I", ANDROID_JAR, "--manifest", ROOT / "android/app/AndroidManifest.xml",
        "-o", OUT / "base.apk", OUT / "resources.zip")
    sources = sorted((ROOT / "android/app/src").rglob("*.java"))
    run(JAVA_HOME / "bin/javac.exe", "--release", "8", "-cp", ANDROID_JAR, "-d", OUT / "classes", *sources)
    classes = sorted((OUT / "classes").rglob("*.class"))
    run(TOOLS / "d8.bat", "--lib", ANDROID_JAR, "--min-api", "26", "--output", OUT / "dex", *classes, shell=False)
    unsigned = OUT / "unsigned.apk"
    shutil.copy(OUT / "base.apk", unsigned)
    with zipfile.ZipFile(unsigned, "a", zipfile.ZIP_DEFLATED) as apk:
        apk.write(OUT / "dex/classes.dex", "classes.dex")
        for library in libraries:
            apk.write(library, "lib/arm64-v8a/" + library.name)
    aligned = OUT / "aligned.apk"
    run(TOOLS / "zipalign.exe", "-f", "-p", "4", unsigned, aligned)
    keystore = ROOT / "build/android/debug.keystore"
    if not keystore.exists():
        keystore.parent.mkdir(parents=True, exist_ok=True)
        run(JAVA_HOME / "bin/keytool.exe", "-genkeypair", "-keystore", keystore, "-storepass", "android",
            "-keypass", "android", "-alias", "androiddebugkey", "-keyalg", "RSA", "-keysize", "2048",
            "-validity", "10000", "-dname", "CN=Stronghold Port")
    apk = OUT / "stronghold-crusader.apk"
    run(TOOLS / "apksigner.bat", "sign", "--ks", keystore, "--ks-pass", "pass:android", "--key-pass", "pass:android",
        "--out", apk, aligned)
    print(f"APK: {apk} ({apk.stat().st_size / 1e6:.1f} MB)")
    if args.install:
        run(ADB, *(["-s", args.serial] if args.serial else []), "install", "-r", "-g", apk)
        print("installed")


if __name__ == "__main__":
    main()
