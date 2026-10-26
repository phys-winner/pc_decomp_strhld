"""Check portable build settings and publication exclusions without game assets."""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))


def load_builder(name, environment):
    with patch.dict(os.environ, environment, clear=True), patch.object(Path, "home", return_value=Path("test-home")):
        spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / (name + ".py"))
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module


class BuildConfigurationTests(unittest.TestCase):
    def test_sdk_settings_match_between_builders(self):
        for environment, expected in (
            ({}, Path("test-home/AppData/Local/Android/Sdk")),
            ({"ANDROID_HOME": "sdk-home"}, Path("sdk-home")),
            ({"ANDROID_SDK_ROOT": "sdk-root"}, Path("sdk-root")),
            ({"ANDROID_SDK": "sdk-legacy", "ANDROID_HOME": "sdk-home"}, Path("sdk-legacy")),
            ({"ANDROID_SDK": "", "ANDROID_HOME": "sdk-home", "ANDROID_SDK_ROOT": "sdk-root"}, Path("sdk-home")),
        ):
            with self.subTest(environment=environment):
                native = load_builder("build_android", environment)
                apk = load_builder("build_apk", environment)
                self.assertEqual(native.SDK, expected)
                self.assertEqual(apk.SDK, expected)
                self.assertEqual(native.NDK, expected / "ndk/27.2.12479018")

    def test_explicit_ndk_and_java_paths(self):
        environment = {"ANDROID_NDK": "custom-ndk", "JAVA_HOME": "custom-jdk"}
        self.assertEqual(load_builder("build_android", environment).NDK, Path("custom-ndk"))
        self.assertEqual(load_builder("build_apk", environment).JAVA_HOME, Path("custom-jdk"))

    def test_missing_jdk_fails_before_building(self):
        apk = load_builder("build_apk", {})
        with patch.dict(os.environ, {}, clear=True), patch.object(sys, "argv", ["build_apk.py"]), \
                patch.object(apk.subprocess, "run") as run, patch("sys.stderr"):
            with self.assertRaises(SystemExit) as failure:
                apk.main()
        self.assertEqual(failure.exception.code, 2)
        run.assert_not_called()

    def test_non_utf8_tool_output_preserves_failure_handling(self):
        apk = load_builder("build_apk", {})
        with patch("sys.stdout"), self.assertRaisesRegex(SystemExit, r"failed \(3\)"):
            apk.run(sys.executable, "-c", "import sys; sys.stdout.buffer.write(bytes([129, 255])); sys.exit(3)")

    def test_host_only_link_matches_full_order_with_patches_and_ties(self):
        native=load_builder("build_android",{})
        with tempfile.TemporaryDirectory() as directory:
            target=Path(directory);native.OUT=target/"build"
            generated=[];full_jobs=[]
            for name,original_size,patched_size in (("a",10,300),("b",200,None),("c",200,None),("d",1000,50)):
                source=target/"generated/game-x86"/(name+".c")
                source.parent.mkdir(parents=True,exist_ok=True);source.write_bytes(b"x"*original_size)
                generated.append(source)
                obj=native.OUT/"obj/game-x86"/(name+".o")
                obj.parent.mkdir(parents=True,exist_ok=True);obj.touch()
                if patched_size is not None:
                    patched=native.OUT/"patched/game-x86"/source.name
                    patched.parent.mkdir(parents=True,exist_ok=True);patched.write_bytes(b"x"*patched_size)
                    source=patched
                full_jobs.append((source,obj,["unchanged-flags"],"unchanged-fingerprint"))
            host_jobs=[]
            for name in ("host_a","host_b"):
                source=target/"android/host"/(name+".c")
                source.parent.mkdir(parents=True,exist_ok=True);source.write_bytes(b"x"*200)
                host_jobs.append((source,native.OUT/(name+".o"),[],"host-fingerprint"))
            # Compile scheduling sorts both modes before the linker receives them.
            full_jobs=sorted(full_jobs+host_jobs,key=lambda job:-job[0].stat().st_size)
            host_jobs=sorted(host_jobs,key=lambda job:-job[0].stat().st_size)
            expected=[str(native.OUT/"obj/game-x86"/(name+".o")) for name in ("a","b","c")]
            expected += [str(job[1]) for job in host_jobs]+[str(native.OUT/"obj/game-x86/d.o")]
            with patch.object(native,"generated_sources",return_value=generated):
                self.assertEqual(native.ordered_link_objects(full_jobs,False),expected)
                self.assertEqual(native.ordered_link_objects(host_jobs,True),expected)
                (native.OUT/"obj/game-x86/d.o").unlink()
                with self.assertRaisesRegex(SystemExit,"requires a complete prior native build; missing"):
                    native.ordered_link_objects(host_jobs,True)

    def test_ignore_rules_keep_private_files_out(self):
        private = ["port.json", "STATE.md", ".env", "originals/game.dat", "generated/game-x86/game.c",
                   "pub/game.zip", "build/android/debug.keystore", "docs/device-report.md",
                   "reference/pcrecomp/.git/config", "android/app/debug.keystore", "tools/local.json",
                   "tools/verify_native_build.py", "tools/verify_stronghold_release.py"]
        public = ["readme.md", "LICENSE", "port.example.json", "config/inputs.json",
                  "android/host/memory.c", "android/app/res/values/colors.xml",
                  "tools/build_apk.py", "tests/test_build_configuration.py", "reference/dlmalloc/malloc.c"]
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory)
            shutil.copyfile(ROOT / ".gitignore", target / ".gitignore")
            subprocess.run(["git", "init", "--quiet", directory], check=True, capture_output=True)
            for name in private + public:
                path = target / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.touch()
            result = subprocess.run(["git", "check-ignore", "--no-index", "--stdin", "-z"], cwd=target,
                                    input=("\0".join(private + public) + "\0").encode(),
                                    capture_output=True, check=True)
        self.assertEqual(set(result.stdout.decode().strip("\0").split("\0")), set(private))


if __name__ == "__main__":
    unittest.main()
