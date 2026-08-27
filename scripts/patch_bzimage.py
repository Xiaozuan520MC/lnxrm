#!/usr/bin/env python3
"""Patch the bzImage setup header after concatenation:
  - offset 0x1F1: setup_sects (number of setup sectors - 1)
  - offset 0x1F4: syssize (payload size in 16-byte paragraphs)
"""
import struct
import sys

def main():
    path = sys.argv[1]
    with open(path, "rb") as f:
        data = bytearray(f.read())
    assert data[0x202:0x206] == b"HdrS", "bad header magic"

    total = len(data)
    # find payload start: setup is padded so that payload begins at a
    # multiple of 512; we pass the boundary explicitly as argv[2]
    payload_off = int(sys.argv[2], 0)

    assert payload_off % 512 == 0
    setup_sects = payload_off // 512 - 1
    payload = total - payload_off
    syssize = (payload + 15) // 16

    data[0x1F1] = setup_sects
    struct.pack_into("<I", data, 0x1F4, syssize)

    with open(path, "wb") as f:
        f.write(bytes(data))
    print(f"[bzimage] setup_sects={setup_sects} syssize={syssize} "
          f"payload={payload} bytes, total={total}")

if __name__ == "__main__":
    main()
