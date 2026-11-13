#!/usr/bin/env python3
"""Verify that an .ipa really contains a signed-or-unsigned iOS ARM64 app.

The old WebView shell once produced a zip that looked like an ipa but had no
Mach-O at all. This check is what stops that from ever being called a build.

Usage:
    python3 tools/verify_ipa.py dist/GalBoxNative-unsigned.ipa [more.ipa ...]
"""
import os
import plistlib
import struct
import sys
import zipfile

CPU_TYPE_ARM64 = 0x0100000C
CPU_TYPE_ARM64_32 = 0x0200000C
MH_MAGIC = 0xFEEDFACE
MH_MAGIC_64 = 0xFEEDFACF
FAT_MAGIC = 0xCAFEBABE
FAT_MAGIC_64 = 0xCAFEBABF

WEB_MARKERS = ("index.html", "app.js", "engine.js", "data.js",
               "manifest.webmanifest", "styles.css")


def macho_cpu_types(data):
    """Return (cpu_types, note). Empty cpu_types means 'not a Mach-O'."""
    if len(data) < 8:
        return [], "文件过短 (%d bytes)" % len(data)
    magic = struct.unpack_from("<I", data, 0)[0]
    if magic in (MH_MAGIC, MH_MAGIC_64):
        return [struct.unpack_from("<I", data, 4)[0]], ""
    if struct.unpack_from(">I", data, 0)[0] in (FAT_MAGIC, FAT_MAGIC_64):
        n = struct.unpack_from(">I", data, 4)[0]
        if n > 32:
            return [], "fat 头声明了 %d 个架构，拒绝" % n
        out, wide = [], struct.unpack_from(">I", data, 0)[0] == FAT_MAGIC_64
        step = 32 if wide else 20
        for i in range(n):
            off = 8 + i * step
            if off + step > len(data):
                return [], "fat 架构表越界"
            cputype = struct.unpack_from(">I", data, off)[0]
            # offset field sits at +8 in both fat_arch (20B) and fat_arch_64 (32B)
            slice_off = struct.unpack_from(">Q" if wide else ">I", data, off + 8)[0]
            if slice_off + 4 > len(data):
                return [], "fat 切片偏移越界"
            sm = struct.unpack_from("<I", data, slice_off)[0]
            if sm not in (MH_MAGIC, MH_MAGIC_64):
                return [], "fat 切片不是 Mach-O"
            out.append(cputype)
        return out, ""
    return [], "magic 不是 Mach-O: %08x" % magic


def check_one(path):
    print("\n=== %s ===" % path)
    passed = failed = 0

    def ok(cond, name, extra=""):
        nonlocal passed, failed
        if cond:
            passed += 1
            print("  PASS " + name + ((" — " + extra) if extra else ""))
        else:
            failed += 1
            print("  FAIL " + name + ((" — " + extra) if extra else ""))

    if not os.path.isfile(path):
        ok(False, "ipa 文件存在", path)
        return 0, 1
    ok(os.path.getsize(path) > 1024, "ipa 非空", "%d bytes" % os.path.getsize(path))
    try:
        z = zipfile.ZipFile(path)
    except Exception as ex:
        ok(False, "是合法 zip", str(ex))
        return passed, failed + 1
    with z:
        bad = z.testzip()
        ok(bad is None, "zip 条目 CRC 完好", bad or "")
        names = z.namelist()
        payload = [n for n in names if n.startswith("Payload/") and not n.endswith("/")]
        ok(bool(payload), "含 Payload/ 目录", "%d 个文件" % len(payload))
        if not payload:
            return passed, failed
        apps = sorted({n[len("Payload/"):].split("/")[0]
                       for n in payload if "/" in n[len("Payload/"):]})
        ok(len(apps) == 1, "Payload 下有且只有一个 .app", ", ".join(apps))
        if not apps:
            return passed, failed
        app = "Payload/" + apps[0] + "/"
        ok(apps[0].endswith(".app"), "产物是 .app 包", apps[0])

        plist_path = app + "Info.plist"
        ok(plist_path in names, "含 Info.plist")
        if plist_path not in names:
            return passed, failed
        try:
            info = plistlib.loads(z.read(plist_path))
        except Exception as ex:
            ok(False, "Info.plist 可解析", str(ex))
            return passed, failed
        exe = info.get("CFBundleExecutable", "")
        ok(bool(exe), "Info.plist 有 CFBundleExecutable", exe)
        ok(info.get("CFBundleIdentifier") == "com.galbox.native",
           "Bundle ID 是原生 App", str(info.get("CFBundleIdentifier")))

        exe_path = app + exe
        ok(exe_path in names, "可执行文件在包内", exe_path)
        if exe_path in names:
            data = z.read(exe_path)
            cpus, note = macho_cpu_types(data)
            ok(bool(cpus), "可执行文件是 Mach-O", note or "%d bytes" % len(data))
            ok(bool(cpus) and all(c in (CPU_TYPE_ARM64, CPU_TYPE_ARM64_32) for c in cpus),
               "架构是 iOS arm64", ", ".join("0x%08x" % c for c in cpus))
            ok(len(data) > 20000, "可执行文件不是占位空壳", "%d bytes" % len(data))

        for res, desc in [("PrivacyInfo.xcprivacy", "隐私清单打进 bundle"),
                          ("demo-game.xp3", "内置 XP3 演示包打进 bundle")]:
            ok(app + res in names, desc)

        web = [n for n in payload if any(n.lower().endswith(m) for m in WEB_MARKERS)
               or "/www/" in n.lower()]
        ok(not web, "包内无网页运行时资源（零 WebView 交付）",
           ", ".join(web[:3]) if web else "")

    print("  → %d PASS / %d FAIL" % (passed, failed))
    return passed, failed


def main():
    args = sys.argv[1:]
    if not args:
        print("用法: python3 tools/verify_ipa.py <ipa> [ipa ...]")
        return 2
    total_p = total_f = 0
    for p in args:
        a, b = check_one(p)
        total_p += a
        total_f += b
    print("\n========================================")
    print("ipa 结构校验: %d PASS / %d FAIL" % (total_p, total_f))
    return 0 if total_f == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
