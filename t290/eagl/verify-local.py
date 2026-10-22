#!/usr/bin/env python3
"""Check local Mach-O architecture, iOS target and ad-hoc code-page hashes.

This verifies packaging, not execution or GPU pixels on the tablet.
"""
import hashlib
import pathlib
import plistlib
import struct
import sys


def verify(path, filetype, expected_entitlements=None):
    data = path.read_bytes()
    magic, cpu, subtype, kind, ncmds, size, flags, reserved = struct.unpack_from('<8I', data)
    assert magic == 0xfeedfacf and cpu == 0x100000c and subtype == 0, 'expected baseline ARM64'
    assert kind == filetype
    offset = 32
    signature = None
    ios = False
    for _ in range(ncmds):
        cmd, length = struct.unpack_from('<2I', data, offset)
        assert length >= 8 and offset + length <= 32 + size
        if cmd == 0x32:  # LC_BUILD_VERSION
            platform, minimum = struct.unpack_from('<2I', data, offset + 8)
            ios = platform == 2 and minimum == 0x100000  # iOS 16.0
        if cmd == 0x1d:  # LC_CODE_SIGNATURE
            start, count = struct.unpack_from('<2I', data, offset + 8)
            signature = data[start:start + count]
        offset += length
    assert ios and signature is not None
    magic, length, count = struct.unpack_from('>3I', signature)
    assert magic == 0xfade0cc0 and length <= len(signature)
    verified = False
    entitlements = None
    entitlement_blob = None
    directory = None
    for index in range(count):
        slot, start = struct.unpack_from('>2I', signature, 12 + index * 8)
        magic, length = struct.unpack_from('>2I', signature, start)
        blob = signature[start:start + length]
        if slot == 5:  # CSSLOT_ENTITLEMENTS
            assert magic == 0xfade7171
            entitlement_blob = blob
            entitlements = plistlib.loads(blob[8:])
        if slot != 0:
            continue
        assert magic == 0xfade0c02
        version, flags, hashes, ident, special, slots, limit = struct.unpack_from('>7I', blob, 8)
        hash_size, hash_type, platform, page_shift = struct.unpack_from('>4B', blob, 36)
        assert flags & 2, 'expected ad-hoc signature'
        assert hash_type == 2 and hash_size == 32, 'expected SHA256'
        directory = (blob, hashes, special)
        page_size = 1 << page_shift
        assert slots == (limit + page_size - 1) // page_size and limit <= len(data)
        for page in range(slots):
            digest = hashlib.sha256(data[page * page_size:min((page + 1) * page_size, limit)]).digest()
            assert blob[hashes + page * hash_size:hashes + (page + 1) * hash_size] == digest, page
        verified = True
    assert verified
    if expected_entitlements is not None:
        assert entitlements == expected_entitlements, 'test entitlements changed'
        blob, hashes, special = directory
        assert special >= 5
        assert blob[hashes - 5 * 32:hashes - 4 * 32] == hashlib.sha256(entitlement_blob).digest()
    print(f'PASS {path.name}: ARM64, iOS 16.0, SHA256 code pages' +
          (', test entitlements' if expected_entitlements is not None else ''))


if __name__ == '__main__':
    folder = pathlib.Path(sys.argv[1])
    expected = plistlib.loads(pathlib.Path(sys.argv[2]).read_bytes())
    verify(folder / 't290eagl.dylib', 6)
    verify(folder / 'eagltest', 2, expected)
