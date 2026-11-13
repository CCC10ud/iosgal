#!/usr/bin/env python3
"""Generate *real* XP3 archives and independently verify them.

Byte layout is taken from Kirikiroid2 src/core/base/XP3Archive.cpp / .h:
  header[11] = 58 50 33 0d 0a 20 0a 1a 8b 67 01
  +11        uint64 LE index offset (relative to archive base)
  index      1 byte flag (0x07: 0=raw, 1=zlib; 0x80=continue)
  chunk      4-byte ASCII tag + uint64 LE size + payload
  File chunk payload = info + segm + adlr chunks
  info       flags(4) OrgSize(8) ArcSize(8) nameLen(2) UTF-16LE name
  segm       N * 28 bytes: flags(4) start(8) orgSize(8) arcSize(8)
  adlr       uint32 hash

The reader below is deliberately written from the spec, NOT by reusing the
builder's in-memory tables, so "builder and reader agree" is a real check.
"""
import hashlib
import json
import os
import struct
import sys
import zlib

MARK = bytes([0x58, 0x50, 0x33, 0x0D, 0x0A, 0x20, 0x0A, 0x1A, 0x8B, 0x67, 0x01])
FIXTURES = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                        "dist", "fixtures")


def chunk(tag, payload):
    assert len(tag) == 4
    return tag + struct.pack("<Q", len(payload)) + payload


def xor_crypt(b, h):
    """Kirikiri built-in XP3 XOR (TVP standard). Matches XP3Archive.cpp exactly;
    the same operation encrypts and decrypts."""
    b = bytearray(b)
    h &= 0xFFFFFFFF
    i = 0
    while i + 4 <= len(b):
        v = int.from_bytes(b[i:i + 4], "little") ^ h
        b[i:i + 4] = v.to_bytes(4, "little")
        h = (h * 21311941) & 0xFFFFFFFF
        i += 4
    while i < len(b):
        b[i] ^= (h >> ((i & 3) * 8)) & 0xFF
        i += 1
    return bytes(b)


def build_xp3(entries, index_compressed=False, mz_prefix=False):
    """entries: [{name, data(bytes), parts:[bool compressed per segment]}]."""
    base = 16 * 3 if mz_prefix else 0
    out = bytearray()
    if mz_prefix:
        out += b"MZ" + b"\x00" * (base - 2)

    # Reserve header + index pointer; segment data starts right after it.
    out += MARK + struct.pack("<Q", 0)  # patched later

    file_chunks = []
    for e in entries:
        data = e["data"]
        parts = e.get("parts") or [False]
        encrypt = bool(e.get("encrypt"))
        n = len(parts)
        # split payload evenly across requested segment count
        sizes = [len(data) // n] * n
        sizes[-1] = len(data) - sum(sizes[:-1])
        seg_records = []
        pos = 0
        for i, sz in enumerate(sizes):
            plain = data[pos:pos + sz]
            pos += sz
            comp = parts[i]
            stored = zlib.compress(plain) if comp else plain
            if encrypt:
                stored = xor_crypt(stored, zlib.adler32(data) & 0xFFFFFFFF)
            start_rel = len(out) - base
            out += stored
            seg_records.append(struct.pack("<IQQQ",
                                           1 if comp else 0,
                                           start_rel, len(plain), len(stored)))
        name = e["name"].encode("utf-16-le")
        flags = 0x80000000 if encrypt else 0
        info = struct.pack("<IQQH", flags, len(data), len(data), len(name) // 2) + name
        adlr = struct.pack("<I", zlib.adler32(data) & 0xFFFFFFFF)
        payload = chunk(b"info", info) + chunk(b"segm", b"".join(seg_records)) + \
                  chunk(b"adlr", adlr)
        file_chunks.append(chunk(b"File", payload))

    index = b"".join(file_chunks)
    if index_compressed:
        body = struct.pack("<QQ", len(zlib.compress(index)), len(index)) + zlib.compress(index)
        flag = 1
    else:
        body = struct.pack("<Q", len(index)) + index
        flag = 0
    idx_abs = len(out)
    out += bytes([flag]) + body
    struct.pack_into("<Q", out, base + 11, idx_abs - base)
    return bytes(out)


def read_i64(b, p):
    return struct.unpack_from("<Q", b, p)[0]


def read_i32(b, p):
    return struct.unpack_from("<I", b, p)[0]


def read_i16(b, p):
    return struct.unpack_from("<H", b, p)[0]


def find_chunk(b, name, cursor, limit):
    p = cursor
    while p + 12 <= limit:
        sz = read_i64(b, p + 4)
        if sz > limit - p - 12:
            return None
        if b[p:p + 4] == name:
            return p + 12, sz
        p += 12 + sz
    return None


def parse_xp3(data):
    """Independent reader; raises ValueError on malformed input."""
    if len(data) < 19:
        raise ValueError("文件过短")
    base = 0
    if data[0:2] == b"MZ":
        p = 16
        while p + 11 <= len(data):
            if data[p:p + 11] == MARK:
                base = p
                break
            p += 16
        else:
            raise ValueError("MZ 内未找到 XP3 标记")
    elif data[0:11] != MARK:
        raise ValueError("XP3 魔数不匹配")
    idx_abs = base + read_i64(data, base + 11)
    if idx_abs + 1 > len(data):
        raise ValueError("索引偏移越界")
    flag = data[idx_abs]
    method = flag & 0x07
    p = idx_abs + 1
    if method == 0:
        sz = read_i64(data, p)
        p += 8
        index = data[p:p + sz]
    elif method == 1:
        csz = read_i64(data, p)
        rsz = read_i64(data, p + 8)
        p += 16
        index = zlib.decompress(data[p:p + csz])
        if len(index) != rsz:
            raise ValueError("索引解压长度不符")
    else:
        raise ValueError("未知索引编码 %d" % method)

    entries = []
    cursor = 0
    while True:
        hit = find_chunk(index, b"File", cursor, len(index))
        if not hit:
            break
        payload, size = hit
        end = payload + size
        info = find_chunk(index, b"info", payload, end)
        segm = find_chunk(index, b"segm", payload, end)
        adlr = find_chunk(index, b"adlr", payload, end)
        if not (info and segm):
            raise ValueError("File 缺 info/segm 子块")
        ip, isz = info
        flags = read_i32(index, ip)
        org = read_i64(index, ip + 4)
        arc = read_i64(index, ip + 12)
        nlen = read_i16(index, ip + 20)
        name = index[ip + 22:ip + 22 + nlen * 2].decode("utf-16-le")
        sp, ssz = segm
        # adlr hash must be known BEFORE the segment loop: it is the XOR key.
        # (Previously computed only after the loop, which crashed for any
        # encrypted entry via UnboundLocalError.)
        h = read_i32(index, adlr[0]) if adlr else 0
        parts = []
        plain = bytearray()
        for i in range(ssz // 28):
            s = sp + i * 28
            sf = read_i32(index, s)
            start = base + read_i64(index, s + 4)
            osz = read_i64(index, s + 12)
            asz = read_i64(index, s + 20)
            if start + asz > len(data):
                raise ValueError("段数据越界")
            stored = data[start:start + asz]
            if flags & 0x80000000:
                stored = xor_crypt(stored, h)
            piece = zlib.decompress(stored) if (sf & 0x07) == 1 else stored
            if len(piece) != osz:
                raise ValueError("段解压长度不符 %s" % name)
            plain += piece
            parts.append(bool(sf & 0x07))
        if len(plain) != org:
            raise ValueError("文件长度不符 %s" % name)
        entries.append({
            "name": name,
            "orgSize": org,
            "arcSize": arc,
            "flags": flags,
            "hash": h,
            "parts": parts,
            "dataLen": len(plain),
            "adler": zlib.adler32(bytes(plain)) & 0xFFFFFFFF,
            "head": bytes(plain[:16]).hex(),
            "sha256": hashlib.sha256(bytes(plain)).hexdigest()[:16],
        })
        cursor = payload + size
    return {"base": base, "indexCompressed": bool(method == 1),
            "indexContinue": bool(flag & 0x80), "count": len(entries),
            "entries": entries}


def fixture_defs():
    return [
        {
            "id": "basic", "file": "basic.xp3", "compressedIndex": False, "mz": False,
            "entries": [
                {"name": "startup.tjs", "data": b"// Kirikiri startup\nScripts.execStorage(\"main.ks\");\n"},
                {"name": "main.ks", "data":
                 "*start\n[bg storage=\"bg/room.png\"]\n【小明】你好。[l][s]\n".encode("utf-8")},
                {"name": "bg/room.bin", "data": bytes(range(256)) * 3},
            ],
        },
        {
            "id": "compressed", "file": "compressed.xp3", "compressedIndex": True, "mz": False,
            "entries": [
                {"name": "中文/剧本.ks", "data": ("*start\n【小樱】这是中文文件名与压缩段的测试。[l]\n"
                                                  "[eval exp=\"f.clue+=1\"]\n[s]\n").encode("utf-8")},
                {"name": "bg/aurora.png", "data": (b"\x89PNG\r\n\x1a\n" + b"A" * 5000),
                 "parts": [True, False, True]},
                {"name": "bgm/theme.ogg", "data": b"OggS" + bytes(range(128)) * 40,
                 "parts": [True]},
            ],
        },
        {
            "id": "selfextract", "file": "selfextract.xp3", "compressedIndex": False, "mz": True,
            "entries": [
                {"name": "data.xp3.inner", "data": b"self-extracting archive payload" * 12},
                {"name": "readme.txt", "data": b"XP3 appended to an MZ executable"},
            ],
        },
        {
            "id": "game", "file": "demo-game.xp3", "compressedIndex": True, "mz": False,
            "entries": [
                {"name": "startup.tjs", "data": b"// demo game package\n"},
                {"name": "scenario/first.ks", "data": (
                    "*start\n"
                    "[bg storage=\"bg/room.png\"]\n"
                    "【小樱】你终于来了。[l]\n"
                    "[eval exp=\"f.clue+=1\"]\n"
                    "[link target=*yes]陪她走[endlink][r]\n"
                    "[link target=*no]留下来[endlink][r]\n"
                    "[s]\n"
                    "*yes\n【小樱】那就一起走吧。[l][s]\n"
                    "*no\n【小樱】也好，再坐一会儿。[l][s]\n"
                ).encode("utf-8")},
                {"name": "bg/room.png", "data": b"\x89PNG\r\n\x1a\n" + b"R" * 2048,
                 "parts": [True]},
                {"name": "fg/sakura.png", "data": b"\x89PNG\r\n\x1a\n" + b"S" * 1024,
                 "parts": [True]},
            ],
        },
        {
            "id": "encrypted", "file": "encrypted.xp3", "compressedIndex": False, "mz": False,
            "entries": [
                # XOR + zlib combined: decrypt the compressed stream, then inflate.
                {"name": "secret.ks", "data": (
                    "*start\n"
                    "[bg storage=\"bg/lock.png\"]\n"
                    "【系统】已验证内置 XOR 解密路径。[l][s]\n"
                ).encode("utf-8"), "encrypt": True, "parts": [True]},
                # raw (uncompressed) XOR segment
                {"name": "cfg.dat", "data": bytes(range(200)) * 5, "encrypt": True},
            ],
        },
    ]


def main():
    os.makedirs(FIXTURES, exist_ok=True)
    manifest = {}
    defs = fixture_defs()
    for d in defs:
        blob = build_xp3(d["entries"], d["compressedIndex"], d["mz"])
        path = os.path.join(FIXTURES, d["file"])
        with open(path, "wb") as f:
            f.write(blob)
        parsed = parse_xp3(blob)
        manifest[d["file"]] = parsed
        print("%-20s %6d bytes  base=%-3d compressedIndex=%-5s files=%d" %
              (d["file"], len(blob), parsed["base"], parsed["indexCompressed"],
               parsed["count"]))

    # Deliberately broken archives: the reader must reject each one for the
    # RIGHT reason. A "clean run" with 0 entries proves nothing here.
    broken = {}
    good = bytearray(open(os.path.join(FIXTURES, "basic.xp3"), "rb").read())
    b1 = bytearray(good); b1[3] = 0x00
    b2 = bytearray(good); struct.pack_into("<Q", b2, 11, len(good) + 9999)
    b4 = bytearray(good); idx = read_i64(b4, 11); b4[idx] = 0x07

    zblob = bytearray(build_xp3(
        [{"name": "only.bin", "data": b"Z" * 4000, "parts": [True]}],
        index_compressed=False))
    zpos = zblob.find(b"\x78\x9c", 19)  # first zlib stream in a data segment
    assert zpos > 0, "夹具构造失败：找不到压缩段"
    zblob[zpos + 12] ^= 0xFF
    b3 = zblob

    for name, blob in (("bad_magic.xp3", b1), ("bad_offset.xp3", b2),
                       ("bad_zlib.xp3", b3), ("bad_flag.xp3", b4)):
        with open(os.path.join(FIXTURES, name), "wb") as f:
            f.write(bytes(blob))
        broken[name] = None
        try:
            parse_xp3(bytes(blob))
        except Exception as ex:
            broken[name] = str(ex)
    # Same bytes as the .ks inside demo-game.xp3, extracted for --kag CLI tests.
    demo = next(d["entries"] for d in defs if d["id"] == "game")
    ks = next(e["data"] for e in demo if e["name"] == "scenario/first.ks")
    with open(os.path.join(FIXTURES, "first.ks"), "wb") as f:
        f.write(ks)
    with open(os.path.join(FIXTURES, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump({"fixtures": manifest, "broken": broken,
                   "kagScript": "first.ks"}, f, ensure_ascii=False, indent=2)
    print("\nbroken archives:", json.dumps(broken, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
