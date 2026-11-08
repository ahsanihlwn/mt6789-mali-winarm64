#!/usr/bin/env python3
"""Fast file transfer to the installed-Windows P13001L USB agent.

The agent's !PUT path stages files under X:\\P13Stage and is gated on:
  * HKLM\\SYSTEM\\CurrentControlSet\\Control\\MiniNT existing, and
  * X:\\P13Stage being a real directory (no reparse point).

On installed Windows X: is normally absent, so --enable does exactly what the
gate wants: map X: to C:\\P13Stage with subst, create the key, and create the
staging directory. Files then land at X:\\P13Stage\\NAME, i.e.
C:\\P13Stage\\P13Stage\\NAME.

Each !PUT frame carries up to 1536 bytes as hex (the agent's Chunk[1536]) and
does not spawn a process, so this is ~100x faster than PowerShell chunks.

Remote names must satisfy the agent rule: [A-Z0-9_]{1,8}\\.[A-Z0-9]{1,3}
(so GPU-Z.EXE must be sent as GPUZ.EXE).

Usage:
  p13put.py --port COM8 --output out\\put --enable \\\n            --put FILE.EXE GPUZ.EXE --put ... \\\n            [--copy-to C:\\P13Stage\\MTKMALI]
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from p13001l_bridge import WinSerial, devices, select_port          # noqa: E402
from p13001l_windows_bridge import request                          # noqa: E402

FRAME_MAX = 1536
NAME_RE = re.compile(r"^[A-Z0-9_]{1,8}\.[A-Z0-9]{1,3}$")
HEX64 = re.compile(r"\b([0-9a-fA-F]{64})\b")
RETRIES = 4


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port")
    ap.add_argument("--output", type=Path, required=True)
    ap.add_argument("--enable", action="store_true",
                    help="satisfy the MiniNT + X:\\P13Stage gates first")
    ap.add_argument("--copy-to", metavar="DIR",
                    help="after upload, copy the staged files to this directory")
    ap.add_argument("--cleanup", action="store_true",
                    help="remove the MiniNT key and the X: mapping afterwards")
    ap.add_argument("--frame", type=int, default=FRAME_MAX)
    ap.add_argument("--resume", action="store_true",
                    help="attempt to resume a partial upload instead of always "
                         "rewriting from offset 0 (unreliable across size changes)")
    ap.add_argument("--put", nargs=2, action="append", default=[],
                    metavar=("LOCAL", "NAME"))
    a = ap.parse_args()
    if not a.put:
        ap.error("at least one --put LOCAL NAME is required")
    a.frame = max(1, min(a.frame, FRAME_MAX))
    for _, name in a.put:
        if not NAME_RE.match(name):
            ap.error(f"remote name {name!r} violates the agent rule "
                     "[A-Z0-9_]{1,8}.[A-Z0-9]{1,3}")

    a.output.mkdir(parents=True, exist_ok=True)
    result: dict = {"files": []}
    serial = None
    log = None
    try:
        serial = WinSerial(select_port(devices(), a.port))
        first = True
        log = (a.output / "console.raw.log").open("ab")

        def command(text: str) -> tuple[str, int]:
            nonlocal first
            data, code = request(serial, text, log, timeout=180,
                                 handshake=first, minimum_version=2)
            first = False
            return data, code

        def checked(text: str) -> str:
            data, code = command(text)
            if code:
                raise RuntimeError(f"command exit {code:#x}: {text[:80]}")
            return data

        def hash_of(name: str) -> str | None:
            try:
                out = checked(f"!HASH {name}")
            except RuntimeError:
                return None
            m = HEX64.search(out)
            return m.group(1).lower() if m else None

        def remote_size(name: str, cap: int) -> int:
            """Size of the remote file, capped at cap. The agent returns an
            empty payload for an offset at/after EOF, so binary-search the
            largest offset that still yields a byte."""
            if cap <= 0:
                return 0
            try:
                if not checked(f"!GET {name} 00000000 00000001").strip():
                    return 0            # exists but empty
            except RuntimeError:
                return 0                # missing
            best, lo, hi = 0, 0, cap - 1
            while lo <= hi:
                mid = (lo + hi) // 2
                try:
                    present = bool(checked(f"!GET {name} {mid:08X} 00000001").strip())
                except RuntimeError:
                    present = False
                if present:
                    best = mid
                    lo = mid + 1
                else:
                    hi = mid - 1
            return best + 1

        if a.enable:
            command("cmd /c if not exist C:\\P13Stage mkdir C:\\P13Stage")
            command("cmd /c subst X: C:\\P13Stage")   # fails harmlessly if present
            command("cmd /c if not exist X:\\P13Stage mkdir X:\\P13Stage")
            checked('reg add "HKLM\\SYSTEM\\CurrentControlSet\\Control\\MiniNT" /f')
            out = checked("cmd /c if exist X:\\P13Stage\\ (echo XOK) else (echo XNO)")
            if "XOK" not in out:
                raise RuntimeError("X:\\P13Stage is not usable; subst/mapping failed")

        for local, name in a.put:
            blob = Path(local).read_bytes()
            want = hashlib.sha256(blob).hexdigest()
            entry = {"local": local, "name": name, "bytes": len(blob),
                     "sha256_local": want}

            if hash_of(name) == want:
                entry.update(sha256_remote=want, verified=True, skipped=True)
                result["files"].append(entry)
                print(json.dumps(entry), flush=True)
                continue

            # Writing at offset 0 truncates, so it is always correct. The
            # agent's append-at-offset path proved unreliable when the new
            # file differs in size from the staged one, so only resume when
            # explicitly asked to.
            start = 0
            if a.resume:
                start = remote_size(name, len(blob)) if len(blob) else 0
                if start > len(blob):
                    start = 0      # stale bigger file: offset 0 truncates
                if start == len(blob):
                    start = 0      # full size but wrong hash: redo from scratch

            pos = start
            frames = 0
            while pos < len(blob):
                chunk = blob[pos:pos + a.frame]
                cmd = f"!PUT {name} {pos:08X} {chunk.hex()}"
                for attempt in range(1, RETRIES + 1):
                    try:
                        checked(cmd)
                        break
                    except (RuntimeError, TimeoutError):
                        if attempt == RETRIES:
                            raise
                        time.sleep(1.0 * attempt)
                pos += len(chunk)
                frames += 1
                if frames % 512 == 0:
                    print(json.dumps({"name": name, "sent": pos,
                                      "total": len(blob)}), flush=True)

            got = hash_of(name)
            entry.update(sha256_remote=got, resumed_from=start, frames=frames)
            if got != want:
                raise RuntimeError(f"SHA256 mismatch for {name}: {got} != {want}")
            entry["verified"] = True
            result["files"].append(entry)
            print(json.dumps(entry), flush=True)

        if a.copy_to:
            command(f'cmd /c if not exist "{a.copy_to}" mkdir "{a.copy_to}"')
            checked(f'cmd /c copy /y "X:\\P13Stage\\*.*" "{a.copy_to}\\"')
            result["copy_to"] = a.copy_to
        if a.cleanup:
            command('reg delete "HKLM\\SYSTEM\\CurrentControlSet\\Control\\MiniNT" /f')
            command("cmd /c subst X: /d")
            result["cleaned"] = True
        return 0
    except (OSError, ValueError, RuntimeError, TimeoutError) as err:
        result["error"] = str(err)
        print(json.dumps(result), flush=True)
        return 1
    finally:
        if log:
            try:
                log.close()
            except Exception:
                pass
        if serial:
            serial.close()
        (a.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    raise SystemExit(main())