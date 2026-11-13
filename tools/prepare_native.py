#!/usr/bin/env python3
"""Package the complete native engine project into dist/GalBoxNative-project.zip.

The zip is directly usable: unzip on a Mac and run `bash tools/build_native.sh`,
or push its contents to GitHub to trigger build-native-ipa.yml.
"""
import os
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PREFIX = "GalBoxNative"
OUT = os.path.join(ROOT, "dist", "GalBoxNative-project.zip")

FILES = [
    "native/Engine/ByteUtils.hpp",
    "native/Engine/XP3Archive.hpp",
    "native/Engine/XP3Archive.cpp",
    "native/Engine/KagEngine.hpp",
    "native/Engine/KagEngine.cpp",
    "native/Tests/engine_cli.cpp",
    "native/Tests/win32_zlib_stub.cpp",
    "native/Tests/include/zlib.h",
    "tools/build_native.sh",
    "tools/verify_native.py",
    "tools/xp3_fixture.py",
    "tools/prepare_native.py",
    "tests/native.test.py",
    ".github/workflows/build-native-ipa.yml",
    "native.html",
    "dist/fixtures/manifest.json",
    "dist/fixtures/first.ks",
]
for name in ("basic.xp3", "compressed.xp3", "selfextract.xp3", "demo-game.xp3",
             "bad_magic.xp3", "bad_offset.xp3", "bad_zlib.xp3", "bad_flag.xp3"):
    FILES.append("dist/fixtures/" + name)

for base, _dirs, files in os.walk(os.path.join(ROOT, "ios-native")):
    for f in files:
        rel = os.path.relpath(os.path.join(base, f), ROOT).replace("\\", "/")
        FILES.append(rel)

README = """GalBox 原生引擎（Kirikiroid2 同类 / iOS 27）
============================================

零 WKWebView：C++ XP3 归档解析 + KAG 运行时 + UIKit 原生界面。

macOS 一键编译：
    bash tools/build_native.sh
    → dist/GalBoxNative-unsigned.ipa
    → dist/GalBoxNative-adhoc.ipa

只有 Windows：
    把本目录推到 GitHub，Actions「Build GalBox Native iOS IPA」
    下载 Artifact「GalBoxNative-ios27-sideload」后重签侧载。

先验证 C++ 内核（不需要 iOS 设备）：
    python3 tools/xp3_fixture.py
    python3 tests/native.test.py
    python3 tools/verify_native.py

边界：不含 TJS2 虚拟机 / 完整 KAG 指令集 / 音视频解码 / 商用加密包滤镜。
要完整兼容商业 galgame，请编译 zeas2/Kirikiroid2 源码。

Bundle ID: com.galbox.native   最低系统: iOS 15.0   架构: arm64
iOS 27 上 TrollStore / .tipa 不可用（CoreTrust 漏洞已在 17.0.1 修复）。
"""


def main():
    missing = [f for f in FILES if not os.path.isfile(os.path.join(ROOT, f))]
    if missing:
        print("ERROR 缺少文件: " + ", ".join(missing))
        return 1
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with zipfile.ZipFile(OUT, "w", zipfile.ZIP_DEFLATED) as z:
        for rel in FILES:
            z.write(os.path.join(ROOT, rel), PREFIX + "/" + rel)
        z.writestr(PREFIX + "/_README.txt", README.encode("utf-8"))

    with zipfile.ZipFile(OUT) as z:
        names = z.namelist()
        required = [
            PREFIX + "/ios-native/GalBoxNative.xcodeproj/project.pbxproj",
            PREFIX + "/ios-native/GalBoxNative/GameViewController.swift",
            PREFIX + "/native/Engine/XP3Archive.cpp",
            PREFIX + "/native/Engine/KagEngine.cpp",
            PREFIX + "/tools/build_native.sh",
            PREFIX + "/.github/workflows/build-native-ipa.yml",
            PREFIX + "/dist/fixtures/demo-game.xp3",
        ]
        for r in required:
            assert r in names, "zip 缺 " + r
        pbx = z.read(PREFIX + "/ios-native/GalBoxNative.xcodeproj/project.pbxproj").decode()
        assert "../native/Engine" in pbx, "工程必须用相对路径引用 C++ 源码"
        assert PREFIX + "/native/Engine/XP3Archive.cpp" in names, \
            "pbxproj 引用的 C++ 源码必须真的在 zip 里"
    print("OK  %s  (%d files, %d bytes)" %
          (OUT, len(names), os.path.getsize(OUT)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
