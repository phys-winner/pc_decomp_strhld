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

Current application version: **1.0-release**. All three editions are packaged into
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
- Original GOG offline installers can be unpacked directly on Android. Select
  the folder containing `setup_*.exe` and all adjacent BIN parts, choose the
  language, then confirm the size and destination. Progress and cancellation
  are supported; extraction uses a fresh directory and preserves the installer.
- Language detection uses SHA-256 of `cr.tex` (Crusader/Extreme) or `sh.tex`
  (Stronghold HD), with ten known localizations of each game. Known localized
  EXEs use checked normalization into a private cache, sharing one compiled
  engine per edition while preserving localized assets and original files.
- Matching on-screen controls use sprites from the original game's GM1 assets,
  retaining accessible descriptions. The map editor has one actions button;
  its six individual commands remain inside that menu.
- Per-edition mod management offers pinned Unofficial Crusader Patch functions
  and reversible ZIP asset packs, including localization packs. See below for
  the supported UCP scope and pack format.
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
| Android CMake | `cmake;3.22.1` |
| Android Platform Tools | Needed for optional ADB deployment |
| Target device | Android 9 / API 28 or later, `arm64-v8a`, OpenGL ES 3.0 |

Install the Android components with the SDK Manager or `sdkmanager.bat`:

```powershell
sdkmanager.bat "platforms;android-36" "build-tools;36.1.0" "ndk;27.2.12479018" "cmake;3.22.1" "platform-tools"
```

The APK targets API 28 and is distributed for sideloading.

## Original game inputs

The port is based on the **Russian versions** of the original PC games.
Supported game distributions are available on GOG:

- [Stronghold HD](https://www.gog.com/en/game/stronghold)
- [Stronghold Crusader HD / Extreme HD](https://www.gog.com/en/game/stronghold_crusader)

Use your own legally obtained installations. The address hooks and function
seeds are built for the exact Russian-version engines listed below. The known
GOG HD 1.41 localizations listed in [config/compatibility.json](config/compatibility.json)
are also accepted. Their EXEs are normalized in a private cache using bounded
gzip/XOR deltas, verifying both source and result SHA-256; the installation and
its language assets are preserved. Unknown or modified EXEs still produce a
warning and may fail if launched anyway. Missing files and incompatible Miles
or Bink DLLs still block launch.

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
   python tools/ucp.py --fetch
   python tools/build_innoextract.py --fetch
   python tools/prepare_compat.py --inputs build/gog-locales
   python tools/build_apk.py
   ```

   `game` means the Extreme executable, `classic` means Crusader HD and
   `stronghold` means Stronghold HD. The lifter copies and verifies the inputs
   under `generated/input`, emits C under `generated/`, and creates one dispatch
   table per edition. The APK builder compiles the three libraries and Java
   launcher, packages them, aligns and signs the APK. Native object reuse is
   checked against source, header, compiler and option fingerprints.

   Packaging strips separate copies of all native libraries and compresses the
   three game engines as ARM64-filtered XZ streams in 16 MiB chunks. XZ Utils
   5.4 or newer is required (`xz` on PATH, the `XZ` environment variable, or the
   copy bundled with Git for Windows). The existing pinned liblzma builds the
   small decoder; no extra upstream dependency is needed. The launcher prepares
   only the selected edition on its background worker, checks SHA-256, and
   atomically installs a read-only private copy. Allow about 65 MB of free
   internal storage per edition; the first launch takes longer to unpack.
   Later launches verify and reuse the copy. Packaging checks unchanged dynamic
   exports and loaded ELF bytes, 16 KiB segment alignment, and full XZ round trips
   from the signed APK. Unstripped libraries remain in `build/arm64` for debugging.

   Before preparing compatibility deltas, put the original localized game EXEs
   from your own GOG installers anywhere under `build/gog-locales` (subdirectories
   are searched). The catalog contains four unique EXE hashes per edition: the
   canonical one and three normalization inputs. Generated deltas are private
   build outputs under `generated/compatibility` and are excluded from Git.
   Existing deltas can be reused; all nine noncanonical inputs are needed for a
   clean build with the complete compatibility catalog.

   UCP, innoextract, XZ, bzip2 and Boost are pinned by revision or archive SHA-256
   in [dependencies.json](dependencies.json). Fetch scripts verify the pins;
   Android adaptations are kept outside the unmodified upstream checkouts.
   Rerun `tools/ucp.py` after changing its Android profile, then rebuild native
   libraries. `--skip-native` only packages the libraries already built.

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
requested file access. Select a complete original game folder, import a ZIP
of your own installation, or choose **Install from GOG** and the folder with
the offline installer and every BIN part. The launcher displays compressed
block counts and installed size before extracting; most files in these GOG
builds are compressed, but both complete installations took about 30 seconds
on the test phone. Keep Crusader/Extreme and Stronghold HD in separate
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

## Mods and localization packs

Open **Mods** on the Crusader or Extreme launcher tile while all games are closed.
Stronghold HD has no Mods button: UCP only targets Crusader. The launcher links
to the upstream UCP repository and can disable all UCP functions for both editions
without disabling localization or other asset packs. The selector also has a
**Disable all** action for its current edition. Changes apply at the next launch
and are stored separately for each edition. In the game's main menu, the edition
switch dialog also opens the same UCP selector; applying it restarts the current
edition. Newly created player profiles, including Japanese names, are saved when
the main menu opens, so an Android process restart does not discard the profile.
UCP functions start disabled. Hold a function to read its description; parameters
use the prepared Android profile in [config/ucp-android.json](config/ucp-android.json).
The selector groups patches into AI fixes, AI improvements, bug fixes, balance,
quality of life and miscellaneous, following the pinned upstream options. Switching
categories keeps pending selections until Apply or Cancel. Long-press descriptions
include verified original authors and explicit co-authors where known; attribution
sources are recorded in [config/ucp-credits.json](config/ucp-credits.json).
Russian patch titles and existing descriptions have been reviewed against the
English upstream text and patch code; titles have no trailing sentence punctuation.

This build adapts **52 of the 55 UCP2 Legacy parent features** for Crusader and
Extreme from the pinned UCP3 distribution. The umbrella revision is
`db6755fbfa55a4abeb2d1231487d1e9f3e434413`, framework revision
`fd326a41d8927eaee42dfb542658843138c15867`, and Legacy module revision
`04e826d00f1a5f8a623e15acffc743f115ee7fb1`. The generated catalog records
availability and conflicts per edition. `o_fix_map_sending`, `o_playercolor`
and `o_xtreme` remain unavailable because their original patch signatures do
not match these exact engines. Existing Stronghold HD localization assets remain
compatible; its game language can be supplied by its GOG installer or `sh.tex`.

The launcher, port settings and UCP selector support English, Russian, Ukrainian,
German, French, Spanish, Italian, Polish, Brazilian Portuguese, Japanese and
Simplified Chinese. **Interface language** in the launcher follows the device by
default and can select any of these languages independently of the game's TEX.
Translations are Android string resources; the UCP text reuses upstream English,
German, Polish and Chinese translations where available.
The remaining text uses machine translation with a reviewed glossary for controls
and settings; full proofreading by native speakers is still pending.

The build runs the pinned Lua patch scripts against checked PE data, translates
supported instructions to guarded ARM64 code and applies verified data writes
at startup. It does not include a runtime capable of arbitrary UCP3 Lua plugins,
Windows DLL or ASI mods; those need individual Android adaptations. Original
upstream sources stay under `reference/ucp`; port code is in `tools/ucp.py` and
`android/host/mods.c`, with the profile and signature adjustments kept separately.

An asset-pack ZIP has `mod.properties` and a `files/` directory at its root:

```properties
id=my-localization
name=My localization
edition=crusader
```

`edition` accepts `classic`, `extreme`, `crusader` (both) or `stronghold`. Use
UTF-8 for the manifest. Put `files/cr.tex` or `files/sh.tex` in a localization
pack; other supported data includes GM1 graphics, maps, AIV, sound and videos.
Code files and unsafe paths are rejected. Install a ZIP, check its row and
apply. Hold a pack to delete it. If packs replace the same asset, the first
enabled pack in the sorted list wins. Reads use the overlay; saves and other
writes continue to use the original installation/profile. Disabling a pack
restores the original assets on the next launch without rewriting them.

The launcher identifies the language of the first enabled TEX overlay. An
unknown TEX remains usable and is displayed as an unknown localization.

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
