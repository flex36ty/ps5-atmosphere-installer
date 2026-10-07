"""Synthetic filesystem/parser tests; never reads the user's SMB share.

Build tools/image-probe.c + backend/image_metadata.c, then run this script.
IMAGE_PROBE may select an ASan/UBSan build of that executable.
"""
import base64
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
PROBE = os.environ.get("IMAGE_PROBE", str(ROOT / "build/image-probe"))
PARAM = json.dumps({"titleId": "PPSA12345", "contentId": "EP0000-PPSA12345_00-ABCDEFGHIJKLMNOP", "requiredSystemSoftwareVersion": "0x1260000000000000", "localizedParameters": {"defaultLanguage": "en-GB", "en-GB": {"titleName": "Parser fixture"}}}).encode()
ICON = base64.b64decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aZ1sAAAAASUVORK5CYII=") + b"\0" * 900
BLOCK = 65536
DDS = b'DDS ' + bytes(160)


def ex_entry(name, cluster, size, directory=False, contiguous=False):
    names = name.encode("utf-16le")
    count = (len(name) + 14) // 15
    result = bytearray((count + 2) * 32)
    result[0:2] = bytes([0x85, count + 1])
    struct.pack_into("<H", result, 4, 16 if directory else 32)
    result[32:36] = bytes([0xc0, 3 if contiguous else 1, 0, len(name)])
    struct.pack_into("<Q", result, 40, size)
    struct.pack_into("<IQ", result, 52, cluster, size)
    for i in range(count):
        offset = 64 + i * 32
        result[offset] = 0xc1
        chunk = names[i * 30:(i + 1) * 30]
        result[offset + 2:offset + 2 + len(chunk)] = chunk
    return result


def exfat(contiguous=False, backport=False):
    data = bytearray(133 * 512)
    data[3:11] = b"EXFAT   "
    struct.pack_into("<IIIIII", data, 80, 1, 4, 5, 128, 2, 123)
    data[108:111] = bytes([9, 0, 1])
    data[510:512] = b"\x55\xaa"
    def put(cluster, payload):
        offset = (5 + cluster - 2) * 512
        data[offset:offset + len(payload)] = payload
    def fat(cluster, nxt): struct.pack_into("<I", data, 512 + cluster * 4, nxt)
    put(2, ex_entry("sce_sys", 3, 1024, True)+(ex_entry("fakelib", 40, 512, True) if backport else b""))
    fat(2, 0xffffffff); fat(3, 9); fat(9, 0xffffffff)
    records = b"".join(ex_entry(f"dummy{i}", 4, len(PARAM)) for i in range(4))
    records += ex_entry("param.json", 4, len(PARAM), contiguous=contiguous)
    records += ex_entry("icon0.png", 7, len(ICON), contiguous=contiguous)
    records += ex_entry("pic0.png", 20, len(ICON), contiguous=True)
    records += ex_entry("pic0.dds", 30, len(DDS), contiguous=True)
    put(30,DDS)
    put(20, ICON)
    # icon0's entry set straddles a directory cluster boundary.
    put(3, records[:512]); put(9, records[512:]); put(4, PARAM)
    fat(4, 0xffffffff)
    put(7, ICON[:512]); put(8 if contiguous else 11, ICON[512:])
    fat(7, 8 if contiguous else 11); fat(8 if contiguous else 11, 0xffffffff)
    return data


def dirent(inode, typ, name):
    encoded = name.encode()
    size = (16 + len(encoded) + 1 + 7) // 8 * 8
    return struct.pack("<IIII", inode, typ, len(encoded), size) + encoded + bytes(size - 16 - len(encoded))


def pfsc(payload, raw=False):
    payload = bytes(payload) + bytes((-len(payload)) % BLOCK)
    count = len(payload) // BLOCK
    data = bytearray(BLOCK)
    struct.pack_into("<IIIIQQQQ", data, 0, 0x43534650, 0, 6, BLOCK, BLOCK, 0x400, BLOCK, len(payload))
    offsets = []
    for i in range(count):
        offsets.append(len(data))
        block = payload[i * BLOCK:(i + 1) * BLOCK]
        compressed = zlib.compress(block)
        data += block if raw or len(compressed) >= BLOCK else compressed
    offsets.append(len(data))
    struct.pack_into(f"<{len(offsets)}Q", data, 0x400, *offsets)
    return data, len(payload)


def inode(data, number, mode, flags, size, logical, first):
    offset = BLOCK + number * 168
    struct.pack_into("<HHIQQ", data, offset, mode, 1, flags, size, logical)
    struct.pack_into("<II", data, offset + 96, (size + BLOCK - 1) // BLOCK, first)


def pfs(payload, compressed=True, raw_blocks=False):
    original = len(payload)
    if compressed: payload, logical = pfsc(payload, raw_blocks)
    else: logical = original
    data = bytearray(4 * BLOCK)
    struct.pack_into("<QQ", data, 0, 2, 20130315)
    struct.pack_into("<H", data, 28, 8)
    struct.pack_into("<I", data, 32, BLOCK)
    struct.pack_into("<QQQQ", data, 40, 1, 4, 4 + (len(payload) + BLOCK - 1) // BLOCK, 1)
    superroot = dirent(2, 3, "uroot")
    root = dirent(3, 2, "inner.exfat")
    data[2 * BLOCK:2 * BLOCK + len(superroot)] = superroot
    data[3 * BLOCK:3 * BLOCK + len(root)] = root
    inode(data, 0, 0x4000, 0, len(superroot), len(superroot), 2)
    inode(data, 2, 0x4000, 0, len(root), len(root), 3)
    inode(data, 3, 0x8000, int(compressed), len(payload) if compressed else original, logical, 4)
    return data + payload


def main():
    with tempfile.TemporaryDirectory(prefix="atmosphere-image-") as tmp:
        root = Path(tmp)
        def check(name, data, success=True, background=ICON, dds=DDS):
            source = root / (name + ".image")
            source.write_bytes(data)
            output = root / name
            run = subprocess.run([PROBE, "--local", str(source), str(output)], capture_output=True, text=True, timeout=15)
            assert run.returncode == (0 if success else 1), (name, run.returncode, run.stdout, run.stderr)
            assert "runtime error:" not in run.stderr and "AddressSanitizer" not in run.stderr, run.stderr
            if success:
                assert output.with_suffix(".json").read_bytes() == PARAM, name
                assert output.with_suffix(".png").read_bytes() == ICON, name
                if os.environ.get('SKIP_BACKGROUND'):
                    assert not output.with_suffix('.background.png').exists(),name
                    if os.environ.get('STREAM_DDS'):assert output.with_suffix('.background.dds').stat().st_size==0,name
                elif os.environ.get('STREAM_DDS'):assert output.with_suffix('.background.dds').read_bytes()==dds,name
                else:assert output.with_suffix(".background.png").read_bytes() == background, name
            print("PASS", name)
        check("exfat-fragmented", exfat())
        check("exfat-contiguous", exfat(True))
        check("pfsc-compressed", pfs(exfat()))
        check("pfsc-raw-blocks", pfs(exfat(), raw_blocks=True))
        check("pfs-uncompressed", pfs(exfat(), compressed=False))
        check("nested-pfs", pfs(pfs(exfat())))
        # Large artwork in a FAT-chained exFAT nested in PFSC repeatedly alternates
        # between FAT and file blocks. A one-block decoder cache exhausts the budget.
        art = ICON + bytes(11*1024*1024-len(ICON))
        count=24000; fat_sectors=(4*(count+2)+511)//512; heap=1+fat_sectors
        large=bytearray((heap+count)*512)
        large[:512]=exfat()[:512]
        struct.pack_into("<IIIIII",large,80,1,fat_sectors,heap,count,2,123)
        def put(c,b): large[(heap+c-2)*512:(heap+c-2)*512+len(b)]=b
        def chain(c,n):struct.pack_into('<I',large,512+c*4,n)
        put(2,ex_entry('sce_sys',3,512,True));chain(2,0xffffffff);chain(3,0xffffffff)
        put(3,ex_entry('param.json',4,len(PARAM))+ex_entry('icon0.png',7,len(ICON),contiguous=True)+ex_entry('pic0.png',20,len(art))+ex_entry('pic0.dds',10,len(DDS),contiguous=True))
        put(10,DDS)
        put(4,PARAM);put(7,ICON);put(20,art)
        for c in range(20,20+len(art)//512):chain(c,c+1)
        chain(20+len(art)//512-1,0xffffffff)
        check('large-background-fat-pfsc',pfs(large),background=art)
        if os.environ.get('STREAM_DDS'):
            bigdds=DDS+bytes(8294548-len(DDS))
            put(20,bigdds)
            put(3,ex_entry('param.json',4,len(PARAM))+ex_entry('icon0.png',7,len(ICON),contiguous=True)+ex_entry('pic0.dds',20,len(bigdds)))
            check('streamed-4k-dds-fat-pfsc',pfs(large),dds=bigdds)
        padded = pfs(exfat())
        struct.pack_into("<QQ", padded, BLOCK + 2 * 168 + 8, BLOCK, BLOCK)
        end = 3 * BLOCK + len(dirent(3, 2, "inner.exfat"))
        padded[end:4 * BLOCK] = b"\xbe\x1a\x99\x3f\0\0\0\0" * ((4 * BLOCK - end) // 8)
        check("legacy-directory-padding", padded)
        check("depth-limit", pfs(pfs(pfs(pfs(exfat())))), False)
        check("truncated", b"PFS", False)
        bad = exfat(); bad[108] = 255
        check("invalid-sector-shift", bad, False)
        bad = exfat(); struct.pack_into("<I", bad, 96, 0xffffff00)
        check("out-of-range-root", bad, False)
        bad = pfs(exfat()); struct.pack_into("<H", bad, 28, 9)
        check("signed-pfs-fallback", bad, False)
        bad = pfs(exfat()); struct.pack_into("<Q", bad, 4 * BLOCK + 0x400, len(bad) + 1)
        check("invalid-pfsc-offsets", bad, False)
        bad = pfs(exfat()); bad[5 * BLOCK:5 * BLOCK + 4] = b"bad!"
        check("corrupt-deflate", bad, False)
        bad = pfs(exfat()); struct.pack_into("<I", bad, 2 * BLOCK + 12, 0xfffffff8)
        check("invalid-dirent", bad, False)
        bad = exfat(); bad[(5 + 2 - 2)*512:(5 + 3 - 2)*512] = b"\x01"*512; struct.pack_into("<I", bad, 512 + 2*4, 2)
        check("cyclic-fat-bounded", bad, False)
    print("All image metadata parser tests passed.")


if __name__ == "__main__": main()
