#!/usr/bin/env python3
"""Upload files to the installed-Windows P13001L USB agent over COM.

The protocol2 agent only stages files under X:\\P13Stage and only in WinPE
(it gates on the MiniNT registry key), so on installed Windows we append raw
bytes with PowerShell [Convert]::FromBase64String via the command channel.

Robustness notes:
  * -NonInteractive so a mangled command errors out instead of blocking on
    stdin (the agent kills a child only after its 120 s limit).
  * Chunks are kept small so the whole command stays far below the agent's
    4096-byte line buffer.
  * Each chunk is retried; an already-correct file is skipped (resume).

Usage:
  p13upload.py --port COM8 --output DIR [--mkdir REMOTE_DIR] [--no-resume] \
               --put LOCAL REMOTE ... [--put LOCAL REMOTE ...]
"""
from __future__ import annotations
import argparse, base64, hashlib, json, re, sys, time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from p13001l_bridge import WinSerial, devices, select_port          # noqa: E402
from p13001l_windows_bridge import request                          # noqa: E402

CHUNK_B64 = 2400                    # multiple of 4; total command stays < 2700 B
HEX64 = re.compile(r"\b([0-9a-fA-F]{64})\b")
RETRIES = 3


def ps_write(path: str, b64: str, create: bool) -> str:
    mode = "Create" if create else "Append"
    return ("powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \""
            f"$b=[Convert]::FromBase64String('{b64}');"
            f"$f=[IO.File]::Open('{path}','{mode}');"
            "$f.Write($b,0,$b.Length);$f.Close()\"")


def remote_sha(path: str) -> str:
    return f'certutil -hashfile "{path}" SHA256'


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port")
    ap.add_argument("--output", type=Path, required=True)
    ap.add_argument("--mkdir")
    ap.add_argument("--no-resume", action="store_true")
    ap.add_argument("--put", nargs=2, action="append", metavar=("LOCAL", "REMOTE"), required=True)
    a = ap.parse_args()

    a.output.mkdir(parents=True, exist_ok=True)
    result = {"files": []}
    serial = None
    log = None
    try:
        serial = WinSerial(select_port(devices(), a.port))
        first = True
        log = (a.output / "console.raw.log").open("ab")

        def command(text: str) -> tuple[str, int]:
            nonlocal first
            data, code = request(serial, text, log, timeout=150, handshake=first, minimum_version=2)
            first = False
            return data, code

        def checked(text: str) -> str:
            data, code = command(text)
            if code:
                raise RuntimeError(f"command exit {code}: {text[:100]}")
            return data

        def hash_of(remote: str) -> str | None:
            out = checked(remote_sha(remote))
            m = HEX64.search(out)
            return m.group(1).lower() if m else None

        if a.mkdir:
            checked(f'cmd /c if not exist "{a.mkdir}" mkdir "{a.mkdir}"')

        for local, remote in a.put:
            blob = Path(local).read_bytes()
            local_sha = hashlib.sha256(blob).hexdigest()
            entry = {"local": local, "remote": remote, "bytes": len(blob),
                     "sha256_local": local_sha}

            if not a.no_resume:
                try:
                    if hash_of(remote) == local_sha:
                        entry.update(sha256_remote=local_sha, verified=True, skipped=True)
                        result["files"].append(entry)
                        print(json.dumps(entry), flush=True)
                        continue
                except RuntimeError:
                    pass  # file absent -> normal upload path

            b64 = base64.b64encode(blob).decode("ascii")
            chunks = [b64[i:i + CHUNK_B64] for i in range(0, len(b64), CHUNK_B64)]
            for idx, chunk in enumerate(chunks):
                cmd = ps_write(remote, chunk, create=(idx == 0))
                for attempt in range(1, RETRIES + 1):
                    try:
                        checked(cmd)
                        break
                    except (RuntimeError, TimeoutError) as err:
                        if attempt == RETRIES:
                            raise
                        # A timeout means the agent is still busy with the
                        # previous child until its own 120 s limit expires.
                        time.sleep(3.0 * attempt)

            remote_sha_val = hash_of(remote)
            entry.update(chunks=len(chunks), sha256_remote=remote_sha_val)
            if remote_sha_val != local_sha:
                raise RuntimeError(f"SHA256 mismatch for {remote}: {remote_sha_val} != {local_sha}")
            entry["verified"] = True
            result["files"].append(entry)
            print(json.dumps(entry), flush=True)
        log.close()
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
