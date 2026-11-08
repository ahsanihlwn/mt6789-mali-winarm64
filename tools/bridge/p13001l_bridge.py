#!/usr/bin/env python3
"""Windows CDC console client. JSON lines on stdin/stdout; USB carries terminal bytes.

No flashing, ADB, or network service. A session must explicitly select a COM port,
or find exactly one private-test P13001L CDC device. Raw RX is retained in --log.
"""
from __future__ import annotations

import argparse
import base64
import ctypes as C
from ctypes import wintypes as W
import json
import os
from pathlib import Path
import queue
import re
import sys
import threading
import time

VID_PID = "VID_1209&PID_0001"
MAX_MESSAGE = 65536


def emit(event: str, **values):
    print(json.dumps({"event": event, **values}, ensure_ascii=True), flush=True)


def devices() -> list[dict]:
    """Enumerate present Ports-class devices, including USB hardware identity."""
    if os.name != "nt":
        return []
    import winreg

    class GUID(C.Structure):
        _fields_ = [("a", W.DWORD), ("b", W.WORD), ("c", W.WORD), ("d", C.c_ubyte * 8)]

    class INFO(C.Structure):
        _fields_ = [("cbSize", W.DWORD), ("guid", GUID), ("devInst", W.DWORD), ("reserved", C.c_size_t)]

    api = C.WinDLL("setupapi", use_last_error=True)
    api.SetupDiGetClassDevsW.argtypes = [C.POINTER(GUID), W.LPCWSTR, W.HWND, W.DWORD]
    api.SetupDiGetClassDevsW.restype = W.HANDLE
    api.SetupDiEnumDeviceInfo.argtypes = [W.HANDLE, W.DWORD, C.POINTER(INFO)]
    api.SetupDiGetDeviceRegistryPropertyW.argtypes = [W.HANDLE, C.POINTER(INFO), W.DWORD, C.c_void_p, C.c_void_p, W.DWORD, C.c_void_p]
    api.SetupDiGetDeviceInstanceIdW.argtypes = [W.HANDLE, C.POINTER(INFO), W.LPWSTR, W.DWORD, C.c_void_p]
    api.SetupDiOpenDevRegKey.argtypes = [W.HANDLE, C.POINTER(INFO), W.DWORD, W.DWORD, W.DWORD, W.DWORD]
    api.SetupDiOpenDevRegKey.restype = W.HANDLE
    api.SetupDiDestroyDeviceInfoList.argtypes = [W.HANDLE]
    guid = GUID(0x4D36E978, 0xE325, 0x11CE, (C.c_ubyte * 8)(0xBF, 0xC1, 0x08, 0, 0x2B, 0xE1, 3, 0x18))
    handle = api.SetupDiGetClassDevsW(C.byref(guid), None, None, 2)
    if handle == C.c_void_p(-1).value:
        raise C.WinError(C.get_last_error())
    found = []
    try:
        index = 0
        while True:
            info = INFO()
            info.cbSize = C.sizeof(INFO)
            if not api.SetupDiEnumDeviceInfo(handle, index, C.byref(info)):
                code = C.get_last_error()
                if code != 259:
                    raise C.WinError(code)
                break
            index += 1

            def prop(number):
                buf = C.create_unicode_buffer(4096)
                if not api.SetupDiGetDeviceRegistryPropertyW(handle, C.byref(info), number, None, buf, C.sizeof(buf), None):
                    return ""
                return buf.value

            instance = C.create_unicode_buffer(4096)
            api.SetupDiGetDeviceInstanceIdW(handle, C.byref(info), instance, len(instance), None)
            key = api.SetupDiOpenDevRegKey(handle, C.byref(info), 1, 0, 1, winreg.KEY_READ)
            if key == C.c_void_p(-1).value:
                continue
            try:
                port = winreg.QueryValueEx(key, "PortName")[0]
            except OSError:
                continue
            finally:
                winreg.CloseKey(key)
            if not re.fullmatch(r"COM[1-9][0-9]*", port, re.I):
                continue
            hardware = prop(1)
            found.append({"port": port.upper(), "name": prop(12), "hardware_id": hardware,
                          "instance": instance.value, "bridge_candidate": VID_PID in hardware.upper()})
    finally:
        api.SetupDiDestroyDeviceInfoList(handle)
    return sorted(found, key=lambda x: int(x["port"][3:]))


def select_port(items, explicit=None, allow_other=False):
    if explicit:
        name = explicit.upper()
        if not re.fullmatch(r"COM[1-9][0-9]*", name):
            raise ValueError("Port must be COM followed by a positive number")
        matches = [d for d in items if d["port"] == name]
        if not allow_other and (len(matches) != 1 or not matches[0]["bridge_candidate"]):
            raise ValueError("Selected port is not the P13001L private-test VID/PID; inspect devices first")
        return name
    matches = [d for d in items if d["bridge_candidate"]]
    if len(matches) != 1:
        raise ValueError(f"Expected one bridge, found {len(matches)}; use devices, then --port COMn")
    return matches[0]["port"]


class WinSerial:
    def __init__(self, port):
        if os.name != "nt":
            raise OSError("Serial transport requires Windows")

        class DCB(C.Structure):
            _fields_ = [("length", W.DWORD), ("baud", W.DWORD), ("flags", W.DWORD),
                        ("reserved", W.WORD), ("xonlim", W.WORD), ("xofflim", W.WORD),
                        ("bytesize", C.c_ubyte), ("parity", C.c_ubyte), ("stopbits", C.c_ubyte),
                        ("xonchar", C.c_char), ("xoffchar", C.c_char), ("errorchar", C.c_char),
                        ("eofchar", C.c_char), ("evtchar", C.c_char), ("reserved1", W.WORD)]

        class TIMEOUTS(C.Structure):
            _fields_ = [(x, W.DWORD) for x in ("interval", "readmult", "readconst", "writemult", "writeconst")]

        self.api = api = C.WinDLL("kernel32", use_last_error=True)
        api.CreateFileW.argtypes = [W.LPCWSTR, W.DWORD, W.DWORD, C.c_void_p, W.DWORD, W.DWORD, W.HANDLE]
        api.CreateFileW.restype = W.HANDLE
        for fn in ("GetCommState", "SetCommState", "SetCommTimeouts"):
            getattr(api, fn).argtypes = [W.HANDLE, C.c_void_p]
        for fn in ("ReadFile", "WriteFile"):
            getattr(api, fn).argtypes = [W.HANDLE, C.c_void_p, W.DWORD, C.POINTER(W.DWORD), C.c_void_p]
        api.CloseHandle.argtypes = [W.HANDLE]
        api.EscapeCommFunction.argtypes = [W.HANDLE, W.DWORD]
        api.SetupComm.argtypes = [W.HANDLE, W.DWORD, W.DWORD]
        self.handle = api.CreateFileW("\\\\.\\" + port, 0xC0000000, 0, None, 3, 0x80, None)
        if self.handle == C.c_void_p(-1).value:
            self.handle = None
            raise C.WinError(C.get_last_error())
        try:
            # USB can burst far faster than the nominal CDC baud setting. The
            # default small serial RX queue can overflow while a screenshot is
            # compressed or a log is written, even though firmware reports no
            # TX drops. Request enough buffering for a complete capture burst.
            self.check(api.SetupComm(self.handle, 1024 * 1024, 65536))
            dcb = DCB()
            dcb.length = C.sizeof(DCB)
            self.check(api.GetCommState(self.handle, C.byref(dcb)))
            dcb.baud, dcb.flags, dcb.bytesize, dcb.parity, dcb.stopbits = 115200, 0x1011, 8, 0, 0
            self.check(api.SetCommState(self.handle, C.byref(dcb)))
            self.check(api.SetCommTimeouts(self.handle, C.byref(TIMEOUTS(0xFFFFFFFF, 0, 0, 2, 500))))
            self.check(api.EscapeCommFunction(self.handle, 5))  # SETDTR
        except BaseException:
            self.close()
            raise

    @staticmethod
    def check(result):
        if not result:
            raise C.WinError(C.get_last_error())

    def read(self):
        buf = C.create_string_buffer(8192)
        count = W.DWORD()
        self.check(self.api.ReadFile(self.handle, buf, len(buf), C.byref(count), None))
        return buf.raw[:count.value]

    def write(self, data):
        count = W.DWORD()
        self.check(self.api.WriteFile(self.handle, data, len(data), C.byref(count), None))
        if count.value != len(data):
            raise OSError(f"Short USB write: {count.value}/{len(data)} bytes")

    def close(self):
        if self.handle is not None:
            self.api.CloseHandle(self.handle)
            self.handle = None


def decode_request(line):
    if len(line) > MAX_MESSAGE:
        raise ValueError("JSON message exceeds 65536 characters")
    request = json.loads(line)
    if not isinstance(request, dict):
        raise ValueError("Expected a JSON object")
    op = request.get("op")
    data = b""
    if op in ("send", "command"):
        text = request.get("text")
        if not isinstance(text, str):
            raise ValueError("text must be a string")
        if op == "command" and any(c in text for c in "\r\n\x00"):
            raise ValueError("command accepts one Shell command; use send for raw terminal text")
        data = text.encode("utf-8") + (b"\r" if op == "command" else b"")
    elif op == "bytes":
        data = base64.b64decode(request.get("data_b64", ""), validate=True)
    elif op not in ("status", "quit"):
        raise ValueError("op must be command, send, bytes, status, or quit")
    if len(data) > 32768:
        raise ValueError("Write exceeds 32768 bytes")
    return request, data


def run_session(serial, requests, output, log, eof_event, idle=time.sleep):
    started = time.monotonic()
    rx = tx = 0
    eof_at = None
    while True:
        for _ in range(16):
            try:
                line = requests.get_nowait()
            except queue.Empty:
                break
            request_id = None
            try:
                request, data = decode_request(line)
                request_id = request.get("id")
                if request["op"] == "quit":
                    output("closed", id=request_id, rx=rx, tx=tx)
                    return
                if request["op"] == "status":
                    output("status", id=request_id, rx=rx, tx=tx, uptime_s=round(time.monotonic() - started, 3))
                else:
                    serial.write(data)
                    tx += len(data)
                    output("sent", id=request_id, bytes=len(data))
            except (ValueError, TypeError, KeyError) as error:
                output("request_error", id=request_id, message=str(error))
        data = serial.read()
        if data:
            rx += len(data)
            log.write(data)
            log.flush()
            output("rx", text=data.decode("utf-8", errors="replace"), data_b64=base64.b64encode(data).decode(), bytes=len(data))
        if eof_event.is_set() and requests.empty():
            if eof_at is None:
                eof_at = time.monotonic()
            if time.monotonic() - eof_at > 0.25:
                output("closed", reason="stdin_eof", rx=rx, tx=tx)
                return
        idle(0.01)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    sub.add_parser("devices", help="List present COM devices and USB identity as JSON")
    session = sub.add_parser("session", help="Read JSON requests from stdin; emit console RX as JSON")
    session.add_argument("--port", help="COM port; auto-select only when exactly one bridge is present")
    session.add_argument("--allow-other-port", action="store_true", help="Explicitly allow a different hardware identity")
    session.add_argument("--stdio", action="store_true", help="JSON-lines interface (also the default)")
    session.add_argument("--log", type=Path, default=Path("p13001l-usb-console.log"), help="Append-only raw RX log")
    args = parser.parse_args(argv)
    serial = None
    try:
        items = devices()
        if args.action == "devices":
            emit("devices", devices=items)
            return 0
        port = select_port(items, args.port, args.allow_other_port)
        args.log.parent.mkdir(parents=True, exist_ok=True)
        serial = WinSerial(port)
        requests = queue.Queue(maxsize=128)
        ended = threading.Event()

        def read_stdin():
            try:
                while True:
                    line = sys.stdin.readline(MAX_MESSAGE + 2)
                    if not line:
                        break
                    if not line.endswith("\n") and len(line) > MAX_MESSAGE:
                        while line and not line.endswith("\n"):
                            line = sys.stdin.readline(MAX_MESSAGE + 2)
                        requests.put('{"op":"invalid_oversized_message"}')
                    else:
                        requests.put(line)
            finally:
                ended.set()

        threading.Thread(target=read_stdin, daemon=True).start()
        emit("connected", port=port, log=str(args.log.resolve()), protocol=1)
        with args.log.open("ab") as logfile:
            run_session(serial, requests, emit, logfile, ended)
        return 0
    except KeyboardInterrupt:
        emit("closed", reason="interrupt")
        return 130
    except (OSError, ValueError) as error:
        emit("error", message=str(error))
        return 1
    finally:
        if serial:
            serial.close()


if __name__ == "__main__":
    raise SystemExit(main())
