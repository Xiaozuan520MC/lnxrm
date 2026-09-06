#!/usr/bin/env python3
"""Build a cpio 'newc' archive (the format Linux initramfs uses).

Usage: mkcpio.py <output.cpio> spec...
where each spec is:  path=content-file  or  dir=path
"""
import os
import sys

def _type(mode):
    t = mode & 0o170000
    if t in (0o040000, 0o020000, 0o060000):
        return t
    return 0o100000

def cpio_entry(name, mode, data):
    name_bytes = name.encode()
    fields = [
        b"070701",
        b"%08X" % (hash(name) & 0xFFFFFFFF),   # ino
        b"%08X" % (mode & 0o7777 | _type(mode)),
        b"%08X" % 0,                          # uid
        b"%08X" % 0,                          # gid
        b"%08X" % 1,                          # nlink
        b"%08X" % 0,                          # mtime
        b"%08X" % len(data),                  # filesize
        b"%08X" % (3 if False else 0x2A),     # devmajor placeholder
        b"%08X" % 0,                          # devminor
        b"%08X" % 0,                          # rdevmajor
        b"%08X" % 0,                          # rdevminor
        b"%08X" % (len(name_bytes) + 1),      # namesize
        b"%08X" % 0,                          # check
    ]
    out = b"".join(fields)
    assert len(out) == 110, len(out)
    out += name_bytes + b"\x00"
    while len(out) % 4:
        out += b"\x00"
    out += data
    while len(out) % 4:
        out += b"\x00"
    return out

def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    out_path = sys.argv[1]
    blob = bytearray()

    def add_dir(path):
        p = path.strip("/")
        if not p:
            return
        blob.extend(cpio_entry(p, 0o0040755 | 0o755, b""))

    for spec in sys.argv[2:]:
        kind, rest = spec.split("=", 1)
        if kind == "dir":
            add_dir(rest)
        elif kind == "char":
            p = rest.strip("/")
            parent = os.path.dirname(p)
            if parent:
                parts = parent.split("/")
                acc = []
                for part in parts:
                    acc.append(part)
                    add_dir("/".join(acc))
            blob.extend(cpio_entry(p, 0o020600, b""))
        elif kind == "file":
            path, src = rest.split("@", 1)
            with open(src, "rb") as f:
                data = f.read()
            p = path.strip("/")
            parent = os.path.dirname(p)
            if parent:
                # ensure parent dirs exist in archive order
                parts = parent.split("/")
                acc = []
                for part in parts:
                    acc.append(part)
                    add_dir("/".join(acc))
            mode = 0o100755          # regular file, executable
            if p.endswith(".txt") or p.endswith("motd"):
                mode = 0o100644
            blob.extend(cpio_entry(p, mode, data))
        else:
            raise SystemExit(f"unknown spec {spec}")

    trailer = b"TRAILER!!!\x00"
    fields = [b"070701"] + [b"%08X" % 0] * 11 + [b"%08X" % len(trailer),
                                                 b"%08X" % 0]
    ent = b"".join(fields) + trailer
    while len(ent) % 4:
        ent += b"\x00"
    blob.extend(ent)
    while len(blob) % 512:
        blob.append(0)

    with open(out_path, "wb") as f:
        f.write(bytes(blob))
    print(f"[mkcpio] {out_path}: {len(blob)} bytes")

if __name__ == "__main__":
    main()
