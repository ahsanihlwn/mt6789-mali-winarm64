#!/usr/bin/env python3
"""Generate the edk2 C array header for an ACPI table compiled by iasl.

The firmware pattern on this board is .asl -> iasl -> .aml -> xxd-style C array
(see edk2-UEFI-mtk .../Applications/UfsAcpi/UfsExperimentalAml.h). This script
does the last step and keeps the output reproducible: it records the AML size
and SHA-256 in the header so a later regeneration can be diffed meaningfully
instead of "the bytes changed".

Usage, with iasl available (Linux/WSL, or a Windows build of acpica-tools):

    python gen-aml-header.py MaliGpu.asl --out MaliGpuAml.h

Usage when the AML was produced elsewhere:

    python gen-aml-header.py MaliGpu.asl --aml MaliGpu.aml --out MaliGpuAml.h

Exit status is non-zero if the AML signature or its Length field is wrong, which
is the two mistakes that silently produce an ACPI table Windows ignores.
"""

import argparse
import hashlib
import os
import shutil
import struct
import subprocess
import sys

VALID_SIGNATURES = (b"SSDT", b"DSDT")


def find_iasl(explicit):
    if explicit:
        return explicit
    return shutil.which("iasl")


def compile_asl(asl_path, iasl):
    """Run iasl next to the .asl; return the .aml path it wrote."""
    stem = os.path.splitext(asl_path)[0]
    aml_path = stem + ".aml"
    # -p sets the output prefix, -ve prints warnings as errors, -f overwrites.
    cmd = [iasl, "-p", stem, "-ve", "-f", asl_path]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout)
        sys.stderr.write(proc.stderr)
        raise SystemExit("iasl failed with status %d" % proc.returncode)
    if not os.path.exists(aml_path):
        raise SystemExit("iasl reported success but %s is missing" % aml_path)
    return aml_path, (proc.stdout or "").strip().splitlines()


def check_aml(blob, path):
    if len(blob) < 36:
        raise SystemExit("%s: %d bytes, too short for an ACPI header" % (path, len(blob)))
    sig = blob[0:4]
    if sig not in VALID_SIGNATURES:
        raise SystemExit("%s: signature %r is not SSDT/DSDT" % (path, sig))
    declared = struct.unpack_from("<I", blob, 4)[0]
    if declared != len(blob):
        raise SystemExit(
            "%s: header Length says %d but the file is %d bytes; Windows would "
            "truncate or reject it" % (path, declared, len(blob))
        )
    return sig.decode("ascii")


def emit(blob, sig, asl_name, asl_stem, iasl_version, out):
    array = "m%sAml" % os.path.basename(asl_stem)
    lines = [
        "/* SPDX-License-Identifier: BSD-2-Clause-Patent; generated from %s */"
        % asl_name,
        "/* %s, %d bytes, sha256 %s */"
        % (iasl_version or "iasl (version unknown)", len(blob),
           hashlib.sha256(blob).hexdigest()),
        "STATIC CONST UINT8 %s[]={" % array,
    ]
    for offset in range(0, len(blob), 16):
        chunk = blob[offset:offset + 16]
        lines.append("  " + ",".join("0x%02x" % b for b in chunk) + ",")
    lines.append("};")
    text = "\n".join(lines) + "\n"
    if out:
        with open(out, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
        print("wrote %s (%s, %d bytes, sha256 %s)"
              % (out, sig, len(blob), hashlib.sha256(blob).hexdigest()))
    else:
        sys.stdout.write(text)


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("asl", help="the .asl source (also names the array)")
    parser.add_argument("--aml", help="use this .aml instead of running iasl")
    parser.add_argument("--iasl", help="path to iasl (default: search PATH)")
    parser.add_argument("--out", help="write the header here (default: stdout)")
    args = parser.parse_args(argv)

    if not os.path.exists(args.asl):
        raise SystemExit("%s: no such file" % args.asl)

    if args.aml:
        aml_path, version = args.aml, None
    else:
        iasl = find_iasl(args.iasl)
        if not iasl:
            raise SystemExit(
                "iasl not found: pass --iasl, or compile the .asl yourself and "
                "pass --aml (acpica-tools ships iasl)"
            )
        aml_path, iasl_log = compile_asl(args.asl, iasl)
        version = next((l for l in iasl_log if "version" in l.lower()), None)

    with open(aml_path, "rb") as handle:
        blob = handle.read()

    sig = check_aml(blob, aml_path)
    emit(blob, sig, os.path.basename(args.asl),
         os.path.splitext(args.asl)[0], version, args.out)


if __name__ == "__main__":
    main(sys.argv[1:])
