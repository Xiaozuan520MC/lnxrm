#!/usr/bin/env python3
"""Build a cpio 'newc' archive (the format Linux initramfs uses).

Usage: mkcpio.py <output.cpio> spec...
where each spec is one of:
    path=content-file      (archive path @ source file)
    dir=path               (create empty directory)
    char=path              (create char device node)
    copy=dst@src           (recursively copy src dir into dst)
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

    def add_file(path, src):
        with open(src, "rb") as f:
            data = f.read()
        p = path.strip("/")
        parent = os.path.dirname(p)
        if parent:
            parts = parent.split("/")
            acc = []
            for part in parts:
                acc.append(part)
                add_dir("/".join(acc))
        mode = 0o100755
        if p.endswith(".txt") or p.endswith("motd"):
            mode = 0o100644
        blob.extend(cpio_entry(p, mode, data))

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
            add_file(path, src)
        elif kind == "copy":
            # copy=<archive-dir>@<source-dir>   (recursive)
            dst, src = rest.split("@", 1)
            dst = dst.strip("/")
            src = src.rstrip("/")
            if not os.path.isdir(src):
                raise SystemExit(f"copy: source not a directory: {src}")
            # 先建目标目录本身
            if dst:
                add_dir(dst)
            for root, dirs, files in os.walk(src):
                rel = os.path.relpath(root, src)
                base = dst if rel == "." else f"{dst}/{rel}"
                # 当前层的子目录先建，保证父在子前
                for d in sorted(dirs):
                    add_dir(f"{base}/{d}")
                # 当前层的文件
                for fn in sorted(files):
                    full = os.path.join(root, fn)
                    p = f"{base}/{fn}" if base else fn
                    add_file(p, full)
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

if __name__ == "__main__":
    main()
