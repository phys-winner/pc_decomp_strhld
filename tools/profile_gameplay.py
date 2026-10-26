"""Measure an already-loaded Android match during repeatable diagonal camera pans.

Requires an explicit device/PID and the matching unstripped game ELF. It never
navigates menus or changes graphics/preferences. CPU percentages use 100% per
logical CPU; device_cpu_percent is normalized to the whole device. Simpleperf
samples are CPU execution, while the game's existing busy ring is wall time.
"""
import argparse
from collections import defaultdict
import hashlib
import importlib
import json
import math
import os
from pathlib import Path
import re
import shlex
import shutil
import struct
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
SDK = Path(os.environ.get("ANDROID_SDK") or os.environ.get("ANDROID_HOME") or
           os.environ.get("ANDROID_SDK_ROOT") or Path.home() / "AppData/Local/Android/Sdk")
NDK = Path(os.environ.get("ANDROID_NDK", SDK / "ndk/27.2.12479018"))
PACKAGE = "org.stronghold.port"


def parse_stat(line):
    """Linux comm can contain spaces and closing parentheses."""
    close = line.rfind(")")
    if close < 0:
        raise ValueError("Missing process comm")
    fields = line[close + 2:].split()
    if len(fields) < 20:
        raise ValueError("Incomplete /proc stat")
    return {"tid": int(line[:line.index("(")].strip()), "name": line[line.index("(") + 1:close],
            "ticks": int(fields[11]) + int(fields[12]), "start_ticks": int(fields[19])}


def parse_snapshot(raw):
    lines = raw.strip().splitlines()
    if len(lines) < 4:
        raise ValueError("Incomplete CPU snapshot")
    process = parse_stat(lines[0])
    cpu = [int(value) for value in lines[-2].split()[1:]]
    if not lines[-2].startswith("cpu ") or len(cpu) < 8:
        raise ValueError("Missing device CPU counters")
    # guest and guest_nice are already included in user and nice.
    return {"uptime": float(lines[-1].split()[0]), "process": process,
            "threads": [parse_stat(line) for line in lines[1:-2]],
            "device_ticks": sum(cpu[:8]), "device_idle": cpu[3] + cpu[4]}


def cpu_summary(samples, hz, cores):
    first, last = samples[0], samples[-1]
    if first["process"]["start_ticks"] != last["process"]["start_ticks"]:
        raise ValueError("PID was reused during recording")
    elapsed = last["uptime"] - first["uptime"]
    if elapsed <= 0 or hz <= 0 or cores <= 0:
        raise ValueError("Invalid CPU measurement interval")
    ticks = last["process"]["ticks"] - first["process"]["ticks"]
    threads = defaultdict(lambda: {"ticks": 0, "name": ""})
    intervals = []
    for before, after in zip(samples, samples[1:]):
        duration = after["uptime"] - before["uptime"]
        if duration <= 0:
            continue
        intervals.append({"seconds": duration, "cpu_percent":
            100 * (after["process"]["ticks"] - before["process"]["ticks"]) / hz / duration})
        old = {(item["tid"], item["start_ticks"]): item for item in before["threads"]}
        for item in after["threads"]:
            key = (item["tid"], item["start_ticks"])
            if key not in old:
                continue
            threads[key]["ticks"] += max(0, item["ticks"] - old[key]["ticks"])
            threads[key]["name"] = item["name"]
    device_ticks = last["device_ticks"] - first["device_ticks"]
    device_idle = last["device_idle"] - first["device_idle"]
    return {"elapsed_seconds": elapsed, "cpu_seconds": ticks / hz,
            "cpu_percent": 100 * ticks / hz / elapsed,
            "cpu_device_capacity_percent": 100 * ticks / hz / elapsed / cores,
            "device_cpu_percent": 100 * (device_ticks - device_idle) / device_ticks if device_ticks else None,
            "cpu_count": cores, "clock_ticks_per_second": hz,
            "thread_coverage_note": "Threads absent at either interval boundary are omitted; process CPU includes them.",
            "threads": sorted([{"tid": key[0], "name": item["name"], "cpu_seconds": item["ticks"] / hz,
                "cpu_percent": 100 * item["ticks"] / hz / elapsed,
                "process_cpu_share_percent": 100 * item["ticks"] / ticks if ticks else 0}
                for key, item in threads.items()], key=lambda item: -item["cpu_seconds"]),
            "intervals": intervals}


def run(command, **kwargs):
    return subprocess.run([str(value) for value in command], check=True, capture_output=True, **kwargs)


def read_native_memory(adb, pid, address, size):
    aligned = address & ~3
    command = f"dd if=/proc/{pid}/mem bs=4 skip={aligned // 4} count={(address - aligned + size + 3) // 4} 2>/dev/null"
    stderr = b""
    for attempt in range(3):
        try:
            result = run([*adb, "exec-out", "run-as " + PACKAGE + " sh -c " + shlex.quote(command)])
        except subprocess.CalledProcessError as error:
            stderr = error.stderr or stderr
            reason = f"ADB exit {error.returncode}"
        else:
            stderr = result.stderr or stderr
            if len(result.stdout) >= address - aligned + size:
                return result.stdout[address - aligned:address - aligned + size]
            reason = f"short read ({len(result.stdout)} bytes, need {address - aligned + size})"
        if attempt < 2:
            time.sleep(.1)
    detail = stderr.decode(errors="replace") if isinstance(stderr, bytes) else stderr
    raise RuntimeError(f"Native memory read failed after 3 attempts: {reason}; {detail}")


def build_input():
    source = ROOT / "tests/GameplayInput.java"
    output = ROOT / "build/gameplay-input"
    classes, dex = output / "classes", output / "dex"
    android_jar = SDK / "platforms/android-36/android.jar"
    javac = Path(os.environ.get("JAVA_HOME", "")) / "bin/javac.exe"
    if not javac.is_file():
        found = shutil.which("javac")
        if not found:
            raise RuntimeError("Set JAVA_HOME to a JDK")
        javac = Path(found)
    d8 = SDK / "build-tools/36.1.0/d8.bat"
    digest = hashlib.sha256(source.read_bytes() + str(javac).encode() + str(android_jar).encode()).hexdigest()
    marker, jar = output / "source.sha256", output / "gameplay-input.jar"
    if jar.is_file() and marker.is_file() and marker.read_text() == digest:
        return jar
    classes.mkdir(parents=True, exist_ok=True)
    dex.mkdir(parents=True, exist_ok=True)
    run([javac, "--release", "8", "-cp", android_jar, "-d", classes, source])
    run([d8, "--lib", android_jar, "--min-api", "28", "--output", jar, classes / "GameplayInput.class"])
    marker.write_text(digest)
    return jar


def sampling_report(record, symfs):
    sys.path.insert(0, str(NDK / "simpleperf"))
    report_module = importlib.import_module("simpleperf_report_lib")
    report = report_module.ReportLib()
    totals, threads, dsos, symbols = 0, defaultdict(int), defaultdict(int), defaultdict(int)
    try:
        report.SetRecordFile(str(record))
        report.SetSymfs(str(symfs))
        while True:
            sample = report.GetNextSample()
            if sample is None:
                break
            symbol = report.GetSymbolOfCurrentSample()
            event = report.GetEventOfCurrentSample()
            if "cpu-clock" not in event.name:
                raise RuntimeError("Expected cpu-clock samples")
            period = int(sample.period)
            totals += period
            threads[(sample.tid, sample.thread_comm)] += period
            dsos[symbol.dso_name] += period
            symbols[(symbol.dso_name, symbol.symbol_name)] += period
    finally:
        report.Close()
    if not totals:
        raise RuntimeError("Simpleperf produced no CPU samples")
    def rows(values, keys):
        return [{**dict(zip(keys, key if isinstance(key, tuple) else (key,))), "period": value,
                 "cpu_sample_share_percent": 100 * value / totals}
                for key, value in sorted(values.items(), key=lambda item: -item[1])]
    return {"event": "cpu-clock", "total_period": totals,
            "threads": rows(threads, ("tid", "name")), "libraries": rows(dsos, ("library",)),
            "symbols": rows(symbols, ("library", "symbol"))}


def recorded_probes(probes):
    # A racing first snapshot can miss old slots that reappear in a later read.
    cutoff = max((ended for ended, _ in probes[0].get("frames", []) if ended), default=0)
    return [{**probe, "frames": [(ended, busy) for ended, busy in probe.get("frames", []) if ended > cutoff]}
            for probe in probes]


def frame_summary(probes, cpu_seconds):
    frames = {}
    for probe in probes:
        for ended, busy in probe.get("frames", []):
            if ended:
                frames[ended] = busy
    times = sorted(frames)
    intervals = [(b - a) / 1e6 for a, b in zip(times, times[1:])]
    busy = sorted(frames.values())
    def p95(values):
        return sorted(values)[math.ceil(len(values) * .95) - 1] if values else None
    cameras = [tuple(item["camera"][:2]) for item in probes if "camera" in item]
    counter_delta = counter_fps = counter_elapsed = counter_fps_low = counter_fps_high = None
    counter_reset = False
    if len(probes)>1 and "frame_count" in probes[0] and "frame_count" in probes[-1]:
        counters = [probe["frame_count"] for probe in probes if "frame_count" in probe]
        counter_reset = any(after<before for before,after in zip(counters,counters[1:]))
        if not counter_reset:
            counter_delta = counters[-1]-counters[0]
            before,after = probes[0],probes[-1]
            counter_elapsed = after["host_monotonic"]-before["host_monotonic"]
            if counter_elapsed>0:
                counter_fps = counter_delta/counter_elapsed
                shortest = after["host_monotonic"]-before.get("host_monotonic_end",before["host_monotonic"])
                longest = after.get("host_monotonic_end",after["host_monotonic"])-before["host_monotonic"]
                counter_fps_low = counter_delta/longest if longest>0 else None
                counter_fps_high = counter_delta/shortest if shortest>0 else None
    count = counter_delta if counter_delta is not None else len(times)
    return {"frames": len(times), "fps": (len(times) - 1) * 1e9 / (times[-1] - times[0]) if len(times) > 1 else None,
            "frame_counter_delta": counter_delta,"fps_counter": counter_fps,"counter_elapsed_seconds": counter_elapsed,
            "fps_counter_lower_bound": counter_fps_low,"fps_counter_upper_bound": counter_fps_high,"counter_reset": counter_reset,
            "cpu_ms_per_frame": cpu_seconds * 1000/count if count else None,
            "cpu_ms_per_frame_source": "frame_counter" if counter_delta is not None else "sampled_ring",
            "frame_p95_ms": p95(intervals), "busy_wall_mean_ms": sum(busy) / len(busy) if busy else None,
            "busy_wall_p95_ms": p95(busy), "camera_unique_positions": len(set(cameras)),
            "camera_moved": len(set(cameras)) > 1,
            "note": "Busy wall includes driver waits. Unsynchronized ring snapshots can lose frames at reset; ring frames/FPS remain diagnostics. Counter FPS is approximate within the first/last probe read timing bounds."}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--pid", required=True, type=int)
    parser.add_argument("--elf", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--seconds", type=float, default=20)
    parser.add_argument("--rounds", type=int, default=2)
    parser.add_argument("--width", type=int, default=1560)
    parser.add_argument("--height", type=int, default=720)
    parser.add_argument("--pinch-out", action="store_true", help="Apply one centered 0.5 pinch before measurement")
    args = parser.parse_args()
    if args.pid <= 0 or not math.isfinite(args.seconds) or not 1 <= args.seconds <= 600 or not 1 <= args.rounds <= 100:
        parser.error("Positive PID, seconds 1..600, rounds 1..100 required")
    if args.seconds * 1000 < args.rounds * 200:
        parser.error("Each of the four pan legs per round needs at least 50 ms")
    if not 320 <= args.width <= 16384 or not 240 <= args.height <= 16384 or not args.elf.is_file():
        parser.error("Valid screen dimensions and existing ELF required")
    args.output.mkdir(parents=True, exist_ok=True)
    if (args.output / "report.json").exists():
        parser.error("Output already contains a report; use a new directory")
    adb = [SDK / "platform-tools/adb.exe", "-s", args.serial]
    def shell(command):
        return run([*adb, "shell", command], text=True).stdout
    def private(command):
        return shell("run-as " + PACKAGE + " sh -c " + shlex.quote(command))
    maps = private(f"cat /proc/{args.pid}/maps")
    mapped = sorted(set(re.findall(r"(/[^\s]+/" + re.escape(args.elf.name) + r")$", maps, re.M)))
    if len(mapped) != 1:
        raise RuntimeError("ELF name must identify exactly one loaded library: " + repr(mapped))
    device_elf = run([*adb, "exec-out", "run-as", PACKAGE, "cat", mapped[0]]).stdout
    local_hash = hashlib.sha256(args.elf.read_bytes()).hexdigest()
    if hashlib.sha256(device_elf).hexdigest() != local_hash:
        raise RuntimeError("Provided ELF does not match the installed loaded library")
    symfs = args.output / "symfs"
    target_elf = symfs / mapped[0].lstrip("/")
    target_elf.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(args.elf, target_elf)
    (args.output / "maps.txt").write_text(maps)
    base = next((int(line.split("-", 1)[0], 16) for line in maps.splitlines()
                 if line.endswith(mapped[0]) and int(line.split()[2], 16) == 0), None)
    if base is None:
        raise RuntimeError("Cannot find the loaded ELF base")
    nm = NDK / "toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-nm.exe"
    symbols = {}
    for line in run([nm, "-S", "--defined-only", args.elf], text=True).stdout.splitlines():
        fields = line.split()
        if len(fields) == 4:
            symbols[fields[3]] = (int(fields[0], 16), int(fields[1], 16))
    probe_names = ("world_zoom", "published_world_zoom", "smooth_camera", "requested_smooth_camera",
                   "render_scale", "source_width", "source_height", "ended_ns", "busy_ms", "perf_next", "perf_count","frame_count")
    chosen = {name: symbols[name] for name in probe_names if name in symbols}
    probes = []
    def native_probe():
        result = {"host_monotonic": time.monotonic()}
        # The explicit camera layout is used only for Crusader Classic measurements.
        if args.elf.name == "libstronghold_classic.so":
            camera = read_native_memory(adb,args.pid,0x2000000000 + 0x23443C0 + 0x78,28)
            result["camera"] = list(struct.unpack_from("<ii", camera)) + [struct.unpack_from("<I", camera, 24)[0]]
            result["game_state"] = struct.unpack("<I",read_native_memory(adb,args.pid,0x2000000000 + 0x21259A0 + 12,4))[0]
        if chosen:
            # Merge adjacent globals, avoiding a separate ADB process for every value.
            groups = []
            for name, (offset, size) in sorted(chosen.items(), key=lambda item: item[1][0]):
                if groups and offset - groups[-1][1] <= 4096:
                    groups[-1][1] = max(groups[-1][1], offset + size)
                    groups[-1][2].append(name)
                else:
                    groups.append([offset, offset + size, [name]])
            for start, end, names in groups:
                data = read_native_memory(adb,args.pid,base+start,end-start)
                for name in names:
                    offset, size = chosen[name]
                    part = data[offset - start:offset - start + size]
                    if name == "ended_ns":
                        result[name] = struct.unpack("<" + "Q" * (size // 8), part)
                    elif name == "frame_count":
                        result[name] = struct.unpack("<Q",part)[0]
                    elif name == "busy_ms":
                        result[name] = struct.unpack("<" + "f" * (size // 4), part)
                    else:
                        result[name] = struct.unpack("<f" if name in ("world_zoom", "published_world_zoom", "render_scale") else "<I", part)[0]
            if all(name in result for name in ("ended_ns", "busy_ms", "perf_count", "perf_next")):
                count = min(result["perf_count"], len(result["ended_ns"]))
                result["frames"] = [(result["ended_ns"][index], result["busy_ms"][index])
                                    for index in ((result["perf_next"] - count + i) % len(result["ended_ns"]) for i in range(count))]
                del result["ended_ns"], result["busy_ms"]
        result["host_monotonic_end"] = time.monotonic()
        probes.append(result)
    helper = build_input()
    remote_jar = "/data/local/tmp/stronghold-gameplay-input.jar"
    run([*adb, "push", helper, remote_jar])
    helper_command = f"CLASSPATH={remote_jar} app_process /system/bin GameplayInput"
    shell(f"{helper_command} selfcheck {args.width} {args.height}")
    if args.pinch_out:
        prepare = shell(f"{helper_command} pinch {args.width} {args.height} 0.5 700")
        (args.output / "prepare.txt").write_text(prepare)
        time.sleep(.5)
    hz = int(shell("getconf CLK_TCK").strip())
    cores = int(shell("getconf _NPROCESSORS_ONLN").strip())
    script = f"cat /proc/{args.pid}/stat; cat /proc/{args.pid}/task/*/stat"
    samples, raw = [], []
    def snapshot():
        value = shell("run-as " + PACKAGE + " sh -c " + shlex.quote(script) + "; head -n 1 /proc/stat; cat /proc/uptime")
        raw.append(value)
        samples.append(parse_snapshot(value))
    remote_record = f"files/gameplay-profile-{args.pid}-{time.time_ns()}.data"
    record_command = f"run-as {PACKAGE} /system/bin/simpleperf record -e cpu-clock -f 400 --duration {args.seconds:g} -p {args.pid} -o {remote_record}"
    pan_command = f"{helper_command} pan {args.width} {args.height} {args.seconds:g} {args.rounds}"
    record_process = input_process = None
    try:
        snapshot()
        native_probe()
        started = time.monotonic()
        record_process = subprocess.Popen([str(value) for value in [*adb, "shell", record_command]], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        input_process = subprocess.Popen([str(value) for value in [*adb, "shell", pan_command]], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        due = started + 1
        while time.monotonic() < started + args.seconds:
            time.sleep(max(0, min(due, started + args.seconds) - time.monotonic()))
            snapshot()
            native_probe()
            due += 1
        input_stdout, input_stderr = input_process.communicate(timeout=30)
        record_stdout, record_stderr = record_process.communicate(timeout=30)
        (args.output / "input.txt").write_text(input_stdout + input_stderr)
        (args.output / "simpleperf.txt").write_text(record_stdout + record_stderr)
        if input_process.returncode or record_process.returncode:
            raise RuntimeError("Input or simpleperf failed; inspect raw logs")
        record = args.output / "perf.data"
        record.write_bytes(run([*adb, "exec-out", "run-as", PACKAGE, "cat", remote_record]).stdout)
        summary = cpu_summary(samples, hz, cores)
        # Exclude pre-record ring entries from the frame results.
        measured = recorded_probes(probes)
        summary.update(serial=args.serial, pid=args.pid, elf=str(args.elf.resolve()), elf_sha256=local_hash,
                       loaded_elf=mapped[0], requested_seconds=args.seconds, rounds=args.rounds,
                       width=args.width, height=args.height, pinch_out=args.pinch_out,
                       native_probes=probes, frame_stats=frame_summary(measured,summary["cpu_seconds"]), sampling=sampling_report(record, symfs))
        (args.output / "report.json").write_text(json.dumps(summary, indent=2) + "\n")
        print(json.dumps({"report": str((args.output / "report.json").resolve()),
            "cpu_percent": summary["cpu_percent"], "cpu_seconds": summary["cpu_seconds"],
            "frame_stats": summary["frame_stats"], "top_symbols": summary["sampling"]["symbols"][:12]}))
    finally:
        for process in (input_process, record_process):
            if process is not None and process.poll() is None:
                # Let bounded remote gestures finish and release both fingers.
                try:
                    process.communicate(timeout=args.seconds + 30)
                except subprocess.TimeoutExpired:
                    process.terminate()
                    process.wait(timeout=10)
        (args.output / "cpu-raw.txt").write_text("\n".join(raw))
        (args.output / "cpu-samples.json").write_text(json.dumps(samples, indent=2) + "\n")
        (args.output / "native-probes.json").write_text(json.dumps(probes, indent=2) + "\n")
        private("rm -f " + shlex.quote(remote_record))


if __name__ == "__main__":
    main()
