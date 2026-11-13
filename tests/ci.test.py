#!/usr/bin/env python3
"""Pre-flight the 'no Mac' GitHub Actions release path from Windows.

Everything that can be checked without Xcode is checked here, including a
closed-loop test of verify_ipa.py with BOTH a synthetic valid ipa and several
deliberately broken ones. A verifier that only ever sees empty input proves
nothing, so the fake-good case is mandatory.
"""
import contextlib
import io
import os
import plistlib
import re
import struct
import sys
import tempfile
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import verify_ipa  # noqa: E402

P = F = 0


def ok(cond, name, extra=""):
    global P, F
    if cond:
        P += 1
    else:
        F += 1
        print("  FAIL " + name + ((" — " + extra) if extra else ""))


def read(rel):
    with open(os.path.join(ROOT, rel), "r", encoding="utf-8") as f:
        return f.read()


def exists(rel):
    return os.path.isfile(os.path.join(ROOT, rel))


def auto_triggered(text):
    i = text.find("\non:")
    j = text.find("\njobs:", i)
    seg = text[i:j] if i >= 0 and j > i else ""
    return any(k in seg for k in ("push:", "pull_request:", "schedule:"))


print("[1] 没有 Mac 时的唯一出包链路")
wf_dir = os.path.join(ROOT, ".github", "workflows")
ymls = sorted(f for f in os.listdir(wf_dir) if f.endswith((".yml", ".yaml")))
auto = [f for f in ymls if auto_triggered(read(".github/workflows/" + f))]
ok(auto == ["build-native-ipa.yml"],
   "自动触发的工作流有且只有原生出包这一条", ", ".join(auto))
wf = read(".github/workflows/build-native-ipa.yml")
for token, desc in [("macos-latest", "跑在 macOS runner"),
                    ("tools/xp3_fixture.py", "先生成 XP3 夹具"),
                    ("tools/verify_native.py", "真编译 C++ 内核自检"),
                    ("tests/native.test.py", "原生工程静态校验"),
                    ("tests/ci.test.py", "CI 链路自检"),
                    ("bash tools/build_native.sh", "调用编译脚本"),
                    ("GalBoxNative-ios27-sideload", "上传侧载产物"),
                    ("build-native/xcodebuild.log", "失败时上传构建日志"),
                    ("if-no-files-found: error", "产物缺失必须让 CI 失败")]:
    ok(token in wf, desc)
ok("workflow_dispatch" in wf, "可在 Actions 页面手动重跑")
ok("WANTED_XCODE" in wf and "xcode-select" in wf, "SDK 过旧时可手动指定 Xcode")
legacy = read(".github/workflows/build-ipa.yml") if exists(".github/workflows/build-ipa.yml") else ""
if legacy:
    ok(not auto_triggered(legacy), "已废弃的网页壳工作流不会自动触发")

print("\n[2] Xcode 工程在陌生 runner 上的可编译性")
pbx = read("ios-native/GalBoxNative.xcodeproj/project.pbxproj")
ok("CODE_SIGN_STYLE = Automatic" not in pbx,
   "未设 Automatic 签名（无 team 时会直接报 requires a development team）")
ok(pbx.count("CODE_SIGN_STYLE = Manual;") == 4, "四个构建配置全部 Manual 签名")
ok(pbx.count('DEVELOPMENT_TEAM = "";') == 2, "target 两级显式清空 Team ID")
ok(pbx.count('CODE_SIGN_IDENTITY = "";') == 2, "target 两级显式清空签名身份")
ok('"-lz"' in pbx and "libz.tbd in Frameworks" in pbx, "zlib 双保险链接")
ok("VALIDATE_PRODUCT = NO" in pbx, "关掉 Release 的 archive 校验（无签名会失败）")
ok("LastUpgradeCheck = 1600" in pbx and 'CreatedOnToolsVersion = 16.0' in pbx,
   "工程不标记为由 Xcode 27 创建，旧版 runner 也能打开")
ok('LastUpgradeVersion="1600"' in read(
    "ios-native/GalBoxNative.xcodeproj/xcshareddata/xcschemes/GalBoxNative.xcscheme"),
   "scheme 版本标记同步降级")
ok("../native/Engine" in pbx, "C++ 源码仍以相对路径单一来源引用")

bs = read("tools/build_native.sh")
for token, desc in [("tools/verify_ipa.py", "出包后强制做 ipa 结构校验"),
                    ('tee "$BUILD/xcodebuild.log"', "xcodebuild 日志落盘"),
                    ("CODE_SIGN_STYLE=Manual", "命令行覆盖签名方式"),
                    ('DEVELOPMENT_TEAM=""', "命令行清空 Team ID"),
                    ('"$(uname -s)" != "Darwin"', "非 macOS 明确拒绝而不是假装成功")]:
    ok(token in bs, desc)

print("\n[3] 出包所需文件齐备（可被整包上传 GitHub）")
required = [
    "native/Engine/ByteUtils.hpp", "native/Engine/XP3Archive.hpp",
    "native/Engine/XP3Archive.cpp", "native/Engine/KagEngine.hpp",
    "native/Engine/KagEngine.cpp", "native/Tests/engine_cli.cpp",
    "native/Tests/win32_zlib_stub.cpp", "native/Tests/include/zlib.h",
    "tools/xp3_fixture.py", "tools/verify_native.py", "tools/verify_ipa.py",
    "tools/build_native.sh", "tests/native.test.py", "tests/ci.test.py",
    ".github/workflows/build-native-ipa.yml",
    "ios-native/GalBoxNative.xcodeproj/project.pbxproj",
    "ios-native/GalBoxNative.xcodeproj/xcshareddata/xcschemes/GalBoxNative.xcscheme",
    "ios-native/GalBoxNative/AppDelegate.swift",
    "ios-native/GalBoxNative/LibraryViewController.swift",
    "ios-native/GalBoxNative/GameViewController.swift",
    "ios-native/GalBoxNative/GBEngineBridge.h",
    "ios-native/GalBoxNative/GBEngineBridge.mm",
    "ios-native/GalBoxNative/GalBoxNative-Bridging-Header.h",
    "ios-native/GalBoxNative/Info.plist",
    "ios-native/GalBoxNative/PrivacyInfo.xcprivacy",
    "ios-native/GalBoxNative/demo-game.xp3",
    "ios-native/ExportOptions.plist",
]
missing = [r for r in required if not exists(r)]
ok(not missing, "构建链文件无缺失", ", ".join(missing))

print("\n[4] verify_ipa.py 闭环：真包要过，假包要被拒")
# Incompressible padding: 30000 identical bytes deflate to <1KB and would
# trip the (correct) "ipa 非空" size guard for the wrong reason.
PAD = os.urandom(30000)
ARM64 = struct.pack("<II", 0xFEEDFACF, 0x0100000C) + b"\x00" * 24 + PAD
X86 = struct.pack("<II", 0xFEEDFACF, 0x01000007) + b"\x00" * 24 + PAD

APP = "Payload/GalBoxNative.app/"


def info(bundle="com.galbox.native", exe="GalBoxNative"):
    return plistlib.dumps({"CFBundleIdentifier": bundle,
                           "CFBundleExecutable": exe,
                           "CFBundleName": "GalBoxNative",
                           "CFBundlePackageType": "APPL"})


def make_ipa(path, exe=ARM64, bundle="com.galbox.native", with_resources=True,
             with_payload=True, web=False):
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        if not with_payload:
            z.writestr("readme.txt", "not an ipa")
            return
        z.writestr(APP + "Info.plist", info(bundle))
        z.writestr(APP + "GalBoxNative", exe)
        if with_resources:
            z.writestr(APP + "PrivacyInfo.xcprivacy", b"<plist/>")
            z.writestr(APP + "demo-game.xp3", b"XP3\r\n \n")
        if web:
            z.writestr(APP + "index.html", b"<html>")
            z.writestr(APP + "app.js", b"// web runtime")


def verdict(path):
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        p, f = verify_ipa.check_one(path)
    return p, f, buf.getvalue()


with tempfile.TemporaryDirectory(prefix="galbox-ci-") as tmp:
    cases = [
        ("good.ipa", {}, True),
        ("no_payload.ipa", {"with_payload": False}, False),
        ("not_macho.ipa", {"exe": b"#!/bin/sh\necho fake\n" * 3000}, False),
        ("x86.ipa", {"exe": X86}, False),
        ("web_shell.ipa", {"web": True}, False),
        ("wrong_bundle.ipa", {"bundle": "com.example.legacy"}, False),
        ("no_resources.ipa", {"with_resources": False}, False),
    ]
    for name, kwargs, should_pass in cases:
        path = os.path.join(tmp, name)
        make_ipa(path, **kwargs)
        p, f, out = verdict(path)
        passed = f == 0
        ok(passed == should_pass,
           ("真包通过" if should_pass else "坏包被拒") + ": " + name,
           "%d PASS / %d FAIL" % (p, f))
        # A no-Payload zip legitimately short-circuits early; for everything
        # else the verifier must have exercised real checks, not just "ran".
        floor = 3 if should_pass or kwargs.get("with_payload", True) else 1
        ok(p >= floor, name + " 确实跑出了检查项（不是空跑）", str(p))
        ok("FAIL" in out or should_pass, name + " 判据输出与预期一致")

    # Real-world negative: the old WKWebView shell ipa must never pass.
    old = os.path.join(ROOT, "dist", "GalBox-shell.ipa")
    if os.path.isfile(old):
        p, f, out = verdict(old)
        ok(f > 0, "真实旧网页壳 ipa 被原生校验拒绝", "%d FAIL" % f)

print("\n========================================")
print("CI 出包链路校验: %d PASS / %d FAIL" % (P, F))
sys.exit(0 if F == 0 else 1)
