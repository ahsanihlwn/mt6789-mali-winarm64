#!/usr/bin/env python3
"""dtb_read.py - minimal, dependency-free flattened device tree reader.

Used to pull the *panel configuration* out of the stock device tree of the
itel P13001L, so the Windows display driver can carry the same numbers as
data (the way the Android device tree carries them, and the way Qualcomm
carries them in an extension INF).

The stock DTB lives outside this repository (the firmware workspace keeps it
under .cache/.../evidence/stock-contract/vendor-main.dtb). Point this script at
a copy; nothing is written outside the repository.

    python dtb_read.py vendor-main.dtb --path /lcds
    python dtb_read.py vendor-main.dtb --path /lcds/cust_lk_panel/display-timings/timing0
    python dtb_read.py vendor-main.dtb --path /lcds --props cust,dsi-lane-number

DTB format: FDT v17, big-endian, see the Devicetree Specification.
"""

import argparse
import hashlib
import struct
import sys

FDT_MAGIC = 0xD00DFEED
FDT_BEGIN_NODE = 0x1
FDT_END_NODE = 0x2
FDT_PROP = 0x3
FDT_NOP = 0x4
FDT_END = 0x9


class Prop:
    __slots__ = ("name", "data")

    def __init__(self, name, data):
        self.name = name
        self.data = data

    def u32(self, index=0):
        if len(self.data) < 4 * (index + 1):
            return None
        return struct.unpack_from(">I", self.data, 4 * index)[0]

    def u32_list(self):
        return [struct.unpack_from(">I", self.data, i)[0] for i in range(0, len(self.data) - 3, 4)]

    def string(self):
        return self.data.split(b"\x00", 1)[0].decode("utf-8", "replace")

    def hex(self):
        return self.data.hex()


class Node:
    __slots__ = ("name", "props", "children", "parent")

    def __init__(self, name, parent=None):
        self.name = name
        self.props = []
        self.children = []
        self.parent = parent

    def path(self):
        if self.parent is None:
            return "/"
        base = self.parent.path()
        if base == "/":
            return "/" + self.name
        return base + "/" + self.name

    def find(self, path):
        """Find a node by absolute path.

        Matching is done on the computed path, not on the node name, because
        overlay DTBs wrap nodes in unnamed intermediate nodes.
        """
        target = path.rstrip("/") or "/"
        stack = [self]
        while stack:
            node = stack.pop()
            if node.path() == target:
                return node
            stack.extend(node.children)
        return None

    def prop(self, name):
        for p in self.props:
            if p.name == name:
                return p
        return None


def parse_dtb(blob):
    if len(blob) < 40:
        raise ValueError("file too small to be a DTB")
    magic, totalsize, off_struct, off_strings, off_rsv, version, last_comp, \
        boot_cpuid, size_strings, size_struct = struct.unpack_from(">10I", blob, 0)
    if magic != FDT_MAGIC:
        raise ValueError("bad DTB magic 0x%08x" % magic)
    strings = blob[off_strings:off_strings + size_strings]

    def name_at(off):
        end = strings.index(b"\x00", off)
        return strings[off:end].decode("utf-8", "replace")

    root = Node("")
    cur = root
    pos = off_struct
    end_struct = off_struct + size_struct
    while pos < end_struct:
        (tok,) = struct.unpack_from(">I", blob, pos)
        pos += 4
        if tok == FDT_BEGIN_NODE:
            end = blob.index(b"\x00", pos)
            name = blob[pos:end].decode("utf-8", "replace")
            pos = end + 1
            pos = (pos + 3) & ~3
            node = Node(name, cur)
            cur.children.append(node)
            cur = node
        elif tok == FDT_END_NODE:
            cur = cur.parent
            if cur is None:
                break
        elif tok == FDT_PROP:
            length, nameoff = struct.unpack_from(">II", blob, pos)
            pos += 8
            data = blob[pos:pos + length]
            pos = (pos + length + 3) & ~3
            cur.props.append(Prop(name_at(nameoff), data))
        elif tok == FDT_NOP:
            continue
        elif tok == FDT_END:
            break
        else:
            raise ValueError("bad token 0x%x at 0x%x" % (tok, pos - 4))
    return root, {"version": version, "size": totalsize}


def format_value(prop, show_hex=True):
    data = prop.data
    if len(data) == 0:
        return "<empty>"
    if b"\x00" in data and all(32 <= b < 127 or b == 0 for b in data):
        return '"%s"' % data.split(b"\x00", 1)[0].decode("utf-8", "replace")
    if len(data) % 4 == 0 and len(data) <= 64:
        words = prop.u32_list()
        return "[%s]" % ", ".join(str(w) for w in words)
    if show_hex:
        return "<%d bytes> %s" % (len(data), data.hex())
    return "<%d bytes>" % len(data)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dtb")
    ap.add_argument("--path", default=None, help="only show this node and its subtree")
    ap.add_argument("--props", default=None,
                    help="comma-separated property names to show (default: all)")
    ap.add_argument("--hex", action="store_true", help="also print the raw bytes")
    ap.add_argument("--sha256", action="store_true", help="print the file SHA-256 and exit")
    args = ap.parse_args()

    with open(args.dtb, "rb") as f:
        blob = f.read()

    if args.sha256:
        print(hashlib.sha256(blob).hexdigest(), args.dtb)
        return 0

    root, info = parse_dtb(blob)
    wanted = set(args.props.split(",")) if args.props else None

    start = root
    if args.path:
        start = root.find(args.path)
        if start is None:
            print("path not found: %s" % args.path, file=sys.stderr)
            return 2

    print("# %s  (%d bytes, DTB v%d)  sha256=%s"
          % (args.dtb, info["size"], info["version"], hashlib.sha256(blob).hexdigest()))

    stack = [(start, 0)]
    while stack:
        node, depth = stack.pop()
        if depth:
            print("%s%s {   /* %s */" % ("  " * depth, node.name, node.path()))
        else:
            print("%s {" % node.path())
        for p in node.props:
            if wanted and p.name not in wanted:
                continue
            line = "%s  %s = %s" % ("  " * depth, p.name, format_value(p))
            if args.hex:
                line += "   [%s]" % p.hex()
            print(line)
        for c in reversed(node.children):
            stack.append((c, depth + 1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
