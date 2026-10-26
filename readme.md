# pc_decomp_strhld

Native Android ARM64 ports of **Stronghold HD**, **Stronghold Crusader HD** and
**Stronghold Crusader Extreme HD**, built by statically recompiling the original
PC x86 executables to C and supplying an Android Win32 compatibility layer.

**Bring your own PC games.** This repository contains the port, build tools and
source dependencies. Original executables, DLLs, game assets and generated game C
are excluded from version control.

This is an unofficial fan project, independent of Firefly Studios and the games'
other rights holders. Stronghold names and original game content belong to their
respective owners.

## Status and features

Current application version: **0.3-dev**. All three editions are packaged into
one APK, with separate Android processes, settings and save directories.

- The original game logic is translated ahead of time with
  [pcrecomp](https://github.com/sp00nznet/pcrecomp), then compiled by the Android NDK.
- Win32, DirectDraw, DirectSound, input, filesystem and DirectPlay compatibility
  are provided by the native host. Presentation uses OpenGL ES 3.0.
- Touch controls support unit selection, commands, camera movement, construction
  and the original interface. Mouse, keyboard and gamepad input are supported.
- Each on-screen command can be moved and resized independently in a paused
  layout preview, using dragging, pinching or sliders. Save commits the draft;
  Cancel keeps the previous layout. The toolbar moves away from the selection.
- Commands can be placed on the screen, in a side zone, or hidden individually.
  The left-click button starts hidden and can be enabled explicitly. Holding the
  original scribe book opens a vertical command list, initially control layout
  setup and the pause menu. Its commands are configurable in the same catalog.
- Two optional side command zones start empty and invisible. Assign commands,
  tap to open a vertical list, tap the same zone again to hide it, or hold and
  swipe with haptic selection before releasing. There is no Close button.
  Move each zone and resize its corner in the control layout editor.
  Each zone can instead run separate assigned tap and hold actions; an empty
  assignment does nothing. Holding executes once and suppresses the tap action.
- All three editions share continuous 50–250% zoom with a movable readout above
  the radar. Radar swipes begin throughout its active area and move the camera
  four times faster than in 0.22.
- The launcher supports selecting a game folder or importing a ZIP, choosing an
  edition, graphics and audio settings, and returning to the last played game.
- The host supports Android systems with 4 KB or 16 KB memory pages.
- LAN support targets compatible Android builds; compatibility with the original
  Windows networking stack is not established.

The project is under development. Full campaigns, every map, prolonged sessions
and maximum Extreme army sizes have not been exhaustively tested. Unsupported
x86/x87 paths may stop with diagnostic traps. Performance depends on the device,
map, army size and graphics settings; 60 FPS is not guaranteed.

## Requirements

The supplied build scripts currently target **64-bit Windows / PowerShell**.
They invoke Windows SDK and JDK tools directly; Gradle and Android Studio are
not required if the command-line toolchains are already installed.

| Component | Version used by the build |
| --- | --- |
| Python | 3.13 |
| Git | Available on `PATH` |
| Java JDK | 24; set `JAVA_HOME` |
| Android SDK Platform | `platforms;android-36` |
| Android SDK Build Tools | `build-tools;36.1.0` |
| Android NDK | `27.2.12479018` (r27c) |
| Android Platform Tools | Needed for optional ADB deployment |
| Target device | Android 9 / API 28 or later, `arm64-v8a`, OpenGL ES 3.0 |

Install the Android components with the SDK Manager or `sdkmanager.bat`:

```powershell
sdkmanager.bat "platforms;android-36" "build-tools;36.1.0" "ndk;27.2.12479018" "platform-tools"
```

The APK currently targets API 28 and is a development build for sideloading.

## Original game inputs

The port is based on the **Russian versions** of the original PC games.
Supported game distributions are available on GOG:

- [Stronghold HD](https://www.gog.com/en/game/stronghold)
- [Stronghold Crusader HD / Extreme HD](https://www.gog.com/en/game/stronghold_crusader)

Use your own legally obtained installations. The current address hooks and
function seeds are built for the exact Russian-version inputs listed below.
The launcher warns about a different EXE and offers **Launch anyway**; patches,
other localizations and modified executables may still fail in the native engine.
Missing files and incompatible Miles or Bink DLLs still block launch.

| File | SHA-256 |
| --- | --- |
| `Stronghold_Crusader_Extreme.exe` | `0c90204f7bf622b768cc1fd9ebc0ec5c6cf8e60ea84911c0fdc7ffcbb2119ba8` |
| `Stronghold Crusader.exe` | `e7c7afb39653a45e2f66d5f5c17f075749e3637f4a2ef30de4c8aca24843e69a` |
| `Stronghold.exe` | `8b581f48df4d14b65b596f3a34ca2ee37b388ed93e7b1e505cceb0b1dadbcede` |
| `Mss32.dll` | `2a515199cc7a2e1ccd8d756efdaab07cedd5c3052309ef1c7499c2560e2097bd` |
| `binkw32.dll` | `ee68dcf51e8e3b482f9fb5ef617f44a2d9e970d03f3ed21fe26d40cd63c57a48` |

The authoritative list is [config/inputs.json](config/inputs.json). The shared
Miles and Bink DLL inputs come from the configured Crusader installation; the
Stronghold HD executable comes from its separate installation. Packaging the
current launcher requires all five inputs and all three native libraries.

## Build the APK

Run the following commands from the root of a clone named `pc_decomp_strhld`.

1. Install the Python requirements and the pinned recompilation tools:

   ```powershell
   python -m venv .venv
   .\.venv\Scripts\Activate.ps1
   python -m pip install -r requirements.txt
   git clone https://github.com/sp00nznet/pcrecomp.git reference/pcrecomp
   git -C reference/pcrecomp checkout --detach 904ac3129d0c9d05efbeec19a1dce6abbe7f5d26
   ```

   `reference/pcrecomp` is an external, ignored checkout. Do not replace its pinned
   revision with the latest branch when reproducing a build. The small dlmalloc
   source dependency is already included in `reference/dlmalloc/malloc.c`.

2. Configure the toolchains and game locations:

   ```powershell
   $env:ANDROID_HOME = Join-Path $env:LOCALAPPDATA "Android\Sdk"
   $env:ANDROID_NDK = Join-Path $env:ANDROID_HOME "ndk\27.2.12479018"
   $env:JAVA_HOME = Read-Host "JDK installation directory"
   Copy-Item port.example.json port.json
   notepad port.json
   ```

   Set `game_directory` to your Crusader/Extreme installation and
   `editions.stronghold.game_directory` to your Stronghold HD installation.
   Paths may be absolute or relative to the repository root. The example uses
   the ignored folders `originals/crusader` and `originals/stronghold`.
   In JSON, use forward slashes or escaped backslashes. `port.json` stays private.

   The scripts accept `ANDROID_SDK`, `ANDROID_HOME` or `ANDROID_SDK_ROOT`, in that
   order. Without these variables they use the current Windows account's usual
   SDK location. `ANDROID_NDK` overrides the SDK's pinned NDK directory.
   `JAVA_HOME` must point to a JDK containing `bin/javac.exe` and `bin/keytool.exe`.

3. Generate C from the checked original inputs, then build and package:

   ```powershell
   python tools/lift.py bink mss game classic stronghold
   python tools/build_apk.py
   ```

   `game` means the Extreme executable, `classic` means Crusader HD and
   `stronghold` means Stronghold HD. The lifter copies and verifies the inputs
   under `generated/input`, emits C under `generated/`, and creates one dispatch
   table per edition. The APK builder compiles the three libraries and Java
   launcher, packages them, aligns and signs the APK. Native object reuse is
   checked against source, header, compiler and option fingerprints.

Output: **`build/android-game/stronghold-crusader.apk`**.

For less parallel compilation, build each library with your chosen job count,
then package the existing libraries:

```powershell
python tools/build_android.py --shared --edition extreme --jobs 4
python tools/build_android.py --shared --edition classic --jobs 4
python tools/build_android.py --shared --edition stronghold --jobs 4
python tools/build_apk.py --skip-native
```

## Install and supply game data

Install your built APK on an ARM64 device, open Stronghold Launcher and grant the
requested file access. Select a complete original game folder or import a ZIP
of your own installation. Keep Crusader/Extreme and Stronghold HD in separate
folders; the launcher discovers editions from their executables.

An optional ADB workflow copies the allowlisted game files and verified PE
inputs. It creates edition profile folders without copying or deleting your
existing saves. Add `platform-tools` to `PATH` first:

```powershell
$env:PATH = (Join-Path $env:ANDROID_HOME "platform-tools") + ";" + $env:PATH
adb devices
python tools/build_apk.py --skip-native --install
python tools/push_data.py --verify
python tools/push_data.py --edition stronghold --verify
adb shell am start -n org.stronghold.port/.LauncherActivity
```

With multiple connected devices, set `ANDROID_SERIAL` locally or pass
`--serial SERIAL` to each deployment command. No device identifier is required
in tracked configuration. Deployment is optional; building the APK does not
contact a phone.

| Content | Default location on the device |
| --- | --- |
| Crusader / Extreme assets | `/sdcard/Stronghold/game` |
| Crusader / Extreme checked PE inputs | `/sdcard/Stronghold/lifted` |
| Stronghold HD assets | `/sdcard/Stronghold/stronghold-hd` |
| Stronghold HD checked PE inputs | `/sdcard/Stronghold/lifted-stronghold` |
| Extreme profile | `/sdcard/Stronghold/userdata` |
| Crusader profile | `/sdcard/Stronghold/userdata-classic` |
| Stronghold HD profile | `/sdcard/Stronghold/userdata-stronghold` |

Back up your profile folders before moving to an independently signed build.

## Checks

Configuration and source interface checks need neither a phone nor game assets:

```powershell
python tests/test_build_configuration.py
python tests/test_android_touch_contract.py
```

After lifting the original inputs, original-code regression checks can run in
Unicorn on the PC:

```powershell
python tests/test_android_hotkeys.py
python tests/test_freebuild_f1.py
```

After building, check native 16 KB segment alignment:

```powershell
python tests/test_elf_pages.py
```

The remaining `tests/` and `tools/test_*.py` files are focused native, Java and
original-code checks; some require a built host, game inputs or an explicitly
selected Android device. They are not a single asset-free test suite.

## Repository layout

| Path | Purpose |
| --- | --- |
| `android/app/` | Launcher, activities, input and vector resources |
| `android/host/` | Android Win32 HLE, graphics, audio, files and edition integration |
| `android/runtime/` | Runtime support for generated x86 code |
| `tools/` | Lifting, hooks, native build, APK packaging and focused checks |
| `config/` | Input hashes and additional function seeds |
| `tests/` | Native, Java and Python regression checks |
| `reference/dlmalloc/` | Vendored allocator source with its original license |
| `dependencies.json` | Source dependency origins and pinned revision |
| `port.example.json` | Public template for the private local configuration |

The publication allowlist in `.gitignore` excludes local settings, original
installations, external checkouts, generated C, build outputs, game archives and
caches. Review new source files before adding them; ignore rules do not inspect
their contents.

## License and credits

The project's own port code, tools and documentation are licensed under the
**GNU General Public License, version 3 or (at your option) any later version**
(`GPL-3.0-or-later`). See [LICENSE](LICENSE). Distribution of covered modifications
must preserve the GPL's source availability and licensing requirements.

Third-party portions retain their original notices and licenses, including
pcrecomp (MIT), dlmalloc (MIT-0) and the AMD FidelityFX FSR adaptation (MIT).
See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). The GPL grant covers only
code and documentation that this project can license; it grants no rights to
original game executables, lifted game code, assets, music or trademarks.
Do not include those materials in source contributions.

Original game content is supplied by the user locally; no game downloads are
provided here.
