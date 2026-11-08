#!/usr/bin/env python3
"""P13001L auto-connect console.

Waits for the private-test USB CDC bridge to (re)appear - e.g. after the
tablet reboots - opens the port, waits for the `P13WIN READY` handshake, and
then either runs one command, a list of commands from a file, or drops into a
small interactive prompt.

The point is convenience: run it, go make coffee, and by the time you come
back it is connected and waiting for you.

Examples
--------
  p13.cmd                                   wait, connect, interactive prompt
  p13.cmd --command "ver"                   one-shot, then exit
  p13.cmd --script bringup.txt              run one command per line
  p13.cmd --wait 0                          connect once, never wait/retry
  p13.cmd --list                            just list COM devices and exit

Interactive prompt commands
---------------------------
  <anything>        sent to the tablet as one cmd.exe line
  :help             show this help
  :list             re-enumerate COM devices
  :reconnect        drop and wait for the device again
  :quit             leave
"""
from __future__ import annotations

import sys

# Keep the tree clean: never drop .pyc next to the tools.
sys.dont_write_bytecode = True

import argparse
import re
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from p13001l_bridge import WinSerial, devices, select_port
from p13001l_windows_bridge import request

READY_RE = re.compile(rb"P13WIN READY ([12])")


# --------------------------------------------------------------------------- #
# device discovery / connection
# --------------------------------------------------------------------------- #
def bridges() -> list[dict]:
    return [d for d in devices() if d.get("bridge_candidate")]


def open_ready(explicit: str | None, ready_timeout: float, log) -> tuple[WinSerial, int]:
    """Open the bridge and return (serial, protocol_version) once READY is seen."""
    port = select_port(devices(), explicit)  # raises when not exactly one
    serial = WinSerial(port)
    try:
        deadline = time.monotonic() + ready_timeout
        seen = bytearray()
        while time.monotonic() < deadline:
            chunk = serial.read()
            if chunk:
                if log is not None:
                    log.write(chunk)
                    log.flush()
                seen.extend(chunk)
                match = READY_RE.search(seen)
                if match:
                    serial.p13_windows_version = int(match[1])
                    serial.p13_port = port
                    return serial, int(match[1])
            else:
                time.sleep(0.02)
        raise TimeoutError("agent handshake (P13WIN READY) not seen")
    except BaseException:
        serial.close()
        raise


def wait_connect(wait: float, explicit: str | None, ready_timeout: float, log) -> WinSerial:
    """Retry until the bridge is up and handshakes, or `wait` seconds elapse."""
    deadline = time.monotonic() + max(wait, 0.0)
    started = time.monotonic()
    next_note = started
    attempt = 0
    while True:
        attempt += 1
        try:
            serial, version = open_ready(explicit, ready_timeout, log)
            print(f"[p13] connected on {serial.p13_port} (agent protocol v{version})", flush=True)
            return serial
        except (ValueError, OSError, TimeoutError) as error:
            now = time.monotonic()
            if now >= deadline:
                print(f"[p13] gave up after {int(now - started)}s: {error}", flush=True)
                raise
            if now >= next_note:
                print(
                    f"[p13] waiting for device... {int(now - started)}s "
                    f"(attempt {attempt}: {error})",
                    flush=True,
                )
                next_note = now + 10.0
            time.sleep(2.0)


# --------------------------------------------------------------------------- #
# command execution
# --------------------------------------------------------------------------- #
def run_one(serial: WinSerial, command: str, timeout: float, log) -> tuple[str, int | None]:
    return request(serial, command, log, timeout=timeout, handshake=False)


def print_result(output: str, code: int | None) -> None:
    sys.stdout.write(output)
    if output and not output.endswith("\n"):
        sys.stdout.write("\n")
    print(f"[exit {code}]", flush=True)


def interactive(serial: WinSerial, args, log) -> WinSerial:
    print("[p13] type a command to send, or :help for prompt commands", flush=True)
    while True:
        try:
            line = input("p13> ")
        except EOFError:
            print("", flush=True)
            return serial
        line = line.strip()
        if not line:
            continue
        if line in (":quit", ":q", ":exit"):
            return serial
        if line == ":help":
            print(__doc__.split("Interactive prompt commands")[1].strip(), flush=True)
            continue
        if line == ":list":
            for d in devices():
                mark = "*" if d.get("bridge_candidate") else " "
                print(f"  {mark} {d['port']:8} {d.get('name','')}", flush=True)
            continue
        if line == ":reconnect":
            serial.close()
            serial = wait_connect(args.wait, args.port, args.ready_timeout, log)
            continue
        try:
            output, code = run_one(serial, line, args.timeout, log)
            print_result(output, code)
        except (TimeoutError, OSError, RuntimeError) as error:
            print(f"[p13] {type(error).__name__}: {error}", flush=True)
            print("[p13] link lost? reconnecting...", flush=True)
            try:
                serial.close()
            except Exception:
                pass
            serial = wait_connect(args.wait, args.port, args.ready_timeout, log)


def read_script(path: Path) -> list[str]:
    commands = []
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        commands.append(line)
    return commands


# --------------------------------------------------------------------------- #
# entry point
# --------------------------------------------------------------------------- #
def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description="P13001L auto-connect console (waits for the tablet, then connects).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--port", help="pin a COM port; default auto-selects the single bridge")
    parser.add_argument("--command", help="run one command and exit")
    parser.add_argument("--script", type=Path, help="file with one command per line (# = comment)")
    parser.add_argument("--wait", type=float, default=600.0, help="seconds to keep waiting for the device (default 600)")
    parser.add_argument("--ready-timeout", type=float, default=25.0, help="seconds to wait for the handshake per attempt")
    parser.add_argument("--timeout", type=float, default=150.0, help="per-command completion timeout (default 150)")
    parser.add_argument("--logdir", type=Path, help="dir for console.raw.log (default: out/p13-console)")
    parser.add_argument("--list", action="store_true", help="list COM devices and exit")
    args = parser.parse_args(argv)

    if args.list:
        for d in devices():
            mark = "*" if d.get("bridge_candidate") else " "
            print(f"  {mark} {d['port']:8} {d.get('name','')}", flush=True)
        return 0

    logdir = args.logdir or Path(__file__).resolve().parent.parent.parent / "out" / "p13-console"
    logdir.mkdir(parents=True, exist_ok=True)
    logpath = logdir / "console.raw.log"

    serial = None
    try:
        with logpath.open("ab") as log:
            serial = wait_connect(args.wait, args.port, args.ready_timeout, log)
            if args.command:
                output, code = run_one(serial, args.command, args.timeout, log)
                print_result(output, code)
                return 0 if code == 0 else 1
            if args.script:
                rc = 0
                for command in read_script(args.script):
                    print(f"[p13] $ {command}", flush=True)
                    output, code = run_one(serial, command, args.timeout, log)
                    print_result(output, code)
                    if code != 0:
                        rc = 1
                return rc
            interactive(serial, args, log)
            return 0
    except KeyboardInterrupt:
        print("\n[p13] interrupted", flush=True)
        return 130
    except (OSError, ValueError, RuntimeError, TimeoutError) as error:
        print(f"[p13] {type(error).__name__}: {error}", flush=True)
        return 1
    finally:
        if serial:
            try:
                serial.close()
            except Exception:
                pass


if __name__ == "__main__":
    raise SystemExit(main())
