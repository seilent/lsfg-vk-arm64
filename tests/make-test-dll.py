#!/usr/bin/env python3

import json
import struct
import sys
from pathlib import Path

SPIRV_MAGIC = 0x07230203
DIRECTORY_BIT = 0x80000000
RT_ICON = 3
RT_RCDATA = 10
RT_GROUP_ICON = 14
RT_VERSION = 16
PE_OFFSET = 0x80
OPTIONAL_SIZE = 240
SECTION_FILE = 0x200
SECTION_RVA = 0x1000


def spirv():
    return struct.pack("<5I", SPIRV_MAGIC, 0x00010000, 0, 1, 0)


def directory(entries):
    out = struct.pack("<IIHHHH", 0, 0, 0, 0, 0, len(entries))
    for ident, offset in entries:
        out += struct.pack("<II", ident, offset)
    return out


def resource_section(types, blobs, data_rva=None):
    data_rva = data_rva or {}
    ids = sorted(blobs)
    empty = 16 + (8 * len(types))
    rcdata = empty + 16
    languages = rcdata + 16 + (8 * len(ids))
    entries = languages + (24 * len(ids))
    offset = entries + (16 * len(ids))

    out = directory([(t, DIRECTORY_BIT | (rcdata if t == RT_RCDATA else empty)) for t in sorted(types)])
    out += directory([])
    out += directory([(ident, DIRECTORY_BIT | (languages + (24 * n))) for n, ident in enumerate(ids)])
    for n in range(len(ids)):
        out += directory([(1033, entries + (16 * n))])
    for ident in ids:
        out += struct.pack("<IIII", data_rva.get(ident, SECTION_RVA + offset), len(blobs[ident]), 0, 0)
        offset += (len(blobs[ident]) + 3) & ~3
    for ident in ids:
        out += blobs[ident] + bytes(-len(blobs[ident]) % 4)
    return out


def pe(section, resources=True):
    raw_size = (len(section) + 0x1FF) & ~0x1FF

    dos = bytearray(PE_OFFSET)
    dos[0:2] = b"MZ"
    struct.pack_into("<i", dos, 0x3C, PE_OFFSET)

    coff = b"PE\0\0" + struct.pack("<HHIIIHH", 0x8664, 1, 0, 0, 0, OPTIONAL_SIZE, 0x2022)

    optional = bytearray(OPTIONAL_SIZE)
    struct.pack_into("<H", optional, 0, 0x20B)
    struct.pack_into("<II", optional, 32, 0x1000, 0x200)
    struct.pack_into("<II", optional, 56, SECTION_RVA + ((len(section) + 0xFFF) & ~0xFFF), SECTION_FILE)
    struct.pack_into("<I", optional, 108, 16)
    if resources:
        struct.pack_into("<II", optional, 128, SECTION_RVA, len(section))

    name = b".rsrc\0\0\0" if resources else b".data\0\0\0"
    header = name + struct.pack("<IIIIIIHHI", len(section), SECTION_RVA, raw_size, SECTION_FILE,
                                0, 0, 0, 0, 0x40000040)

    head = bytes(dos) + coff + bytes(optional) + header
    return head + bytes(SECTION_FILE - len(head)) + section + bytes(raw_size - len(section))


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: make-test-dll.py <shader_ids.json> <output dir>")

    requests = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
    out = Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)

    shaders = {r["resource"]: spirv() for r in requests}
    fp16 = {r["resource"] for r in requests if r["fp16"]}
    perf = {r["resource"] for r in requests if r["perf"]}
    mipmaps = requests[0]
    if mipmaps["fp16"] or mipmaps["perf"]:
        sys.exit("the first request must be the fp32 mipmaps shader")
    picked = mipmaps["resource"]
    picked_fp16 = next(r["resource"] for r in requests if r["id"] == mipmaps["id"] and r["fp16"])
    types = [RT_ICON, RT_RCDATA, RT_VERSION]
    extra = {1: b"version"}

    def without(drop):
        return {k: v for k, v in shaders.items() if k not in drop} | extra

    good = pe(resource_section(types, shaders | extra))
    fixtures = {
        "good": good,
        "truncated": good[:len(good) // 2],
        "no-resources": pe(bytes(16), resources=False),
        "no-rcdata": pe(resource_section([RT_ICON, RT_GROUP_ICON, RT_VERSION], shaders)),
        "missing-id": pe(resource_section(types, without({picked}))),
        "not-spirv": pe(resource_section(types, shaders | extra | {picked: b"DXBC" + bytes(28)})),
        "short-blob": pe(resource_section(types, shaders | extra | {picked: struct.pack("<I", SPIRV_MAGIC) + bytes(12)})),
        "odd-blob": pe(resource_section(types, shaders | extra | {picked: spirv() + bytes(1)})),
        "bad-fp16": pe(resource_section(types, shaders | extra | {picked_fp16: b"DXBC" + bytes(28)})),
        "no-fp32": pe(resource_section(types, without(set(shaders) - fp16))),
        "no-fp16": pe(resource_section(types, without(fp16))),
        "no-perf": pe(resource_section(types, without(perf))),
        "bad-entry": pe(resource_section(types, shaders | extra | {2: spirv()}, {2: 0x7FFF0000})),
    }
    for name, data in fixtures.items():
        (out / f"{name}.dll").write_bytes(data)


if __name__ == "__main__":
    main()
