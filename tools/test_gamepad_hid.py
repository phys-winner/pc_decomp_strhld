"""Drive a real Android UHID gamepad through adb shell hid (no event injection).

python -u tools/test_gamepad_hid.py --serial DEVICE --output build/evidence/gamepad
Then send one JSON object per line: {"tap":"SELECT"}, {"lx":0.5},
{"reset":true}, {"capture":"menu"}, or {"quit":true}.
Axes persist until changed; closing stdin removes the virtual device.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import sys
import time

DESCRIPTOR = bytes.fromhex(
    "05 01 09 05 a1 01 85 01 "
    "05 09 19 01 29 10 15 00 25 01 75 01 95 10 81 02 "
    "05 01 09 30 09 31 09 32 09 35 15 81 25 7f 35 81 45 7f 75 08 95 04 81 02 "
    "05 02 09 c5 09 c4 15 00 26 ff 00 35 00 46 ff 00 75 08 95 02 81 02 "
    "05 01 09 39 15 00 25 07 35 00 46 3b 01 65 14 75 04 95 01 81 42 "
    "65 00 75 04 95 01 81 03 c0")
BUTTONS = {name: 1 << i for i, name in enumerate(
    ("A", "B", "C", "X", "Y", "Z", "LB", "RB", "L2", "R2", "SELECT", "START", "MODE", "L3", "R3"))}
DEFAULT = dict(buttons=[], lx=0, ly=0, rx=0, ry=0, lt=0, rt=0, hat=8)


def report(state):
    mask = 0
    for button in state["buttons"]:
        mask |= BUTTONS[button]
    sticks = [round(max(-1, min(1, state[axis])) * 127) for axis in ("lx", "ly", "rx", "ry")]
    triggers = [round(max(0, min(1, state[axis])) * 255) for axis in ("lt", "rt")]
    hat = state["hat"]
    if not isinstance(hat, int) or not 0 <= hat <= 8:
        raise ValueError("hat must be 0..7 clockwise from up, or 8 for neutral")
    return [1, *struct.pack("<HbbbbBBB", mask, *sticks, *triggers, hat)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--bus", choices=("usb", "bluetooth"), default="usb")
    parser.add_argument("--script", type=Path, help="JSON lines to replay instead of interactive stdin")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    adb = ["adb", "-s", args.serial]
    assert report(DEFAULT) == [1, 0, 0, 0, 0, 0, 0, 0, 0, 8]
    transcript = (args.output / "hid-reports.jsonl").open("a", encoding="utf-8")
    log = (args.output / "hid-process.log").open("a", encoding="utf-8")
    hid = subprocess.Popen([*adb, "shell", "hid", "-"], stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT, text=True)

    def send(command):
        command = dict(id=1, **command)
        hid.stdin.write(json.dumps(command) + "\n")
        hid.stdin.flush()
        transcript.write(json.dumps(dict(time=time.time(), **command)) + "\n")
        transcript.flush()

    unplug = False
    try:
        send(dict(command="register", name="Stronghold Virtual Gamepad", vid=0x1234, pid=0x3472,
                  bus=args.bus, descriptor=list(DESCRIPTOR)))
        for attempt in range(30):
            if hid.poll() is not None:
                raise RuntimeError("hid exited: " + (args.output / "hid-process.log").read_text())
            dump = subprocess.check_output([*adb, "shell", "dumpsys", "input"], text=True, encoding="utf-8")
            if "Stronghold Virtual Gamepad" in dump.partition("Input Reader State")[2]:
                (args.output / "hid-device.txt").write_text(dump, encoding="utf-8")
                break
            time.sleep(.2)
        else:
            raise RuntimeError("Android InputReader did not register the virtual gamepad")
        state = DEFAULT.copy()
        send(dict(command="report", report=report(state)))
        print(json.dumps(dict(status="ready", serial=args.serial, bus=args.bus)), flush=True)
        source = args.script.open(encoding="utf-8") if args.script else sys.stdin
        for line in source:
            if not line.strip():
                continue
            command = json.loads(line)
            if command.get("quit") or command.get("unplug"):
                unplug = bool(command.get("unplug"))
                break
            if command.get("reset"):
                state = DEFAULT.copy()
            state.update({k: v for k, v in command.items() if k in DEFAULT})
            send(dict(command="report", report=report(state)))
            if "tap" in command:
                tapped = state.copy()
                tapped["buttons"] = list(set(state["buttons"]) | {command["tap"]})
                send(dict(command="report", report=report(tapped)))
                time.sleep(.15)
                send(dict(command="report", report=report(state)))
            if "wait" in command:
                time.sleep(max(0, min(40, float(command["wait"]))))
            if "hold" in command:
                time.sleep(max(0, min(10, float(command["hold"]))))
                state = DEFAULT.copy()
                send(dict(command="report", report=report(state)))
            if "idle" in command:
                state = dict(DEFAULT, lx=.65)
                send(dict(command="report", report=report(state)))
                time.sleep(.15)
                state = DEFAULT.copy()
                send(dict(command="report", report=report(state)))
                began = time.monotonic()
                samples = []
                for seconds in (0, 29, 31):
                    time.sleep(max(0, began + seconds - time.monotonic()))
                    filename = f"{command['idle']}-{seconds:02}.png"
                    if Path(filename).name != filename:
                        raise ValueError("idle must be a filename stem")
                    at = time.monotonic() - began
                    (args.output / filename).write_bytes(subprocess.check_output([*adb, "exec-out", "screencap", "-p"]))
                    samples.append(dict(seconds=at, screenshot=filename))
                (args.output / (command["idle"] + ".json")).write_text(json.dumps(samples, indent=2))
            if "capture" in command:
                name = command["capture"]
                if Path(name).name != name:
                    raise ValueError("capture must be a filename stem")
                (args.output / (name + ".png")).write_bytes(subprocess.check_output([*adb, "exec-out", "screencap", "-p"]))
                for service in ("window", "input"):
                    (args.output / (name + "-" + service + ".txt")).write_bytes(subprocess.check_output([*adb, "shell", "dumpsys", service]))
                subprocess.run([*adb, "shell", "uiautomator", "dump", "/data/local/tmp/stronghold-gamepad-ui.xml"], capture_output=True, timeout=15)
                (args.output / (name + ".xml")).write_bytes(subprocess.check_output([*adb, "exec-out", "cat", "/data/local/tmp/stronghold-gamepad-ui.xml"]))
            print(json.dumps(dict(status="sent", command=command)), flush=True)
    finally:
        if hid.poll() is None:
            try:
                if not unplug:
                    send(dict(command="report", report=report(DEFAULT)))
                hid.stdin.close()
                hid.wait(timeout=5)
            except (BrokenPipeError, subprocess.TimeoutExpired):
                hid.terminate()
        transcript.close()
        log.close()


if __name__ == "__main__":
    main()
