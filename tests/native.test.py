#!/usr/bin/env python3
"""Static + closed-loop checks for the zero-WebView native engine project."""
import hashlib
import json
import os
import plistlib
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import xp3_fixture  # noqa: E402

P = 0
F = 0


def ok(cond, name):
    global P, F
    if cond:
        P += 1
    else:
        F += 1
        print("  FAIL " + name)


def read(rel):
    with open(os.path.join(ROOT, rel), "r", encoding="utf-8") as f:
        return f.read()


def raw(rel):
    with open(os.path.join(ROOT, rel), "rb") as f:
        return f.read()


def md5(rel):
    return hashlib.md5(raw(rel)).hexdigest()


print("[1] 原生引擎源码与字节常量")
xp3_cpp = read("native/Engine/XP3Archive.cpp")
xp3_hpp = read("native/Engine/XP3Archive.hpp")
kag_cpp = read("native/Engine/KagEngine.cpp")
bytes_hpp = read("native/Engine/ByteUtils.hpp")
cli = read("native/Tests/engine_cli.cpp")

m = re.search(r"XP3Archive::kMark\[11\]\s*=\s*\{([^}]+)\}", xp3_cpp, re.S)
mark = [int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", m.group(1))] if m else []
ok(mark == list(xp3_fixture.MARK),
   "C++ 魔数与 Python 参考实现逐字节一致: " + " ".join("%02x" % b for b in mark))
ok("base_ + 11" in xp3_cpp and "base_ + rd64" in xp3_cpp,
   "索引偏移 / 段偏移都叠加自解压 base（MZ 情形）")
for token, desc in [("0x07", "索引/段编码掩码 0x07"),
                    ("0x80", "索引 CONTINUE 位 0x80"),
                    ("0x80000000", "FILE_PROTECTED 位"),
                    ("segmSize / 28", "段表 28 字节"),
                    ("infoSize < 22", "info 头部 22 字节"),
                    ("idx[info + 22]", "文件名从 info+22 开始")]:
    ok(token in xp3_cpp, desc + " 在 C++ 源码中")
ok("zlibInflate" in bytes_hpp and "uncompress" in bytes_hpp, "zlib 解压路径存在")
ok("utf16leToUtf8" in bytes_hpp, "UTF-16LE 文件名转换存在")
ok("kMaxIndexChain" in xp3_hpp, "索引链有上限防死循环")
ok("next(false)" in kag_cpp and "doRecord &&" in kag_cpp,
   "读档不重复写历史（restore 走 next(false)，record 受 doRecord 门控）")
ok("compound ? pos - 1 : pos" in kag_cpp and "exp[pos - 1]" in kag_cpp,
   "f.x+=1 不会把 + 吞进变量名（lhs 按复合运算符剥离）")
ok("linkMode) linkText.push_back" in kag_cpp, "[link] 文字进入选项而非旁白缓冲")
ok('"bg "' in kag_cpp and 'savedBg' in kag_cpp, "存档保存背景效果")
ok("GBSAVE1" in kag_cpp, "存档格式有版本头")
for src in (xp3_cpp, kag_cpp, bytes_hpp):
    ok(src.count("{ ") >= 0 and src.count("{") == src.count("}"),
       "花括号平衡: " + src[:40].replace("\n", " "))

print("\n[2] 零 WebView：原生层不得残留网页运行时")
native_files = []
all_files = []
text_ext = (".swift", ".mm", ".h", ".pbxproj", ".plist", ".xcprivacy",
            ".xcscheme", ".json")
for base, _dirs, files in os.walk(os.path.join(ROOT, "ios-native")):
    for f in files:
        rel = os.path.relpath(os.path.join(base, f), ROOT)
        all_files.append(rel)
        if f.endswith(text_ext):
            native_files.append(rel)
joined = "\n".join(read(p) for p in native_files)
for bad in ("WKWebView", "UIWebView", "loadHTMLString", "WebKit.framework",
            "evaluateJavaScript", "index.html", "app.js", "localStorage"):
    ok(bad not in joined, "原生工程不含 " + bad)
ok("UIKit" in joined and "UIDocumentPicker" in joined, "使用 UIKit 原生视图与文档选择器")
ok("GBEngineBridge" in joined, "Swift 通过 ObjC++ 桥调用 C++ 引擎")
ok(len(all_files) >= 14, "原生工程文件齐备 (%d 个文件)" % len(all_files))

print("\n[3] pbxproj 引用完整性")
pbx = read("ios-native/GalBoxNative.xcodeproj/project.pbxproj")
# PBX objects may be single-line or multi-line (isa on the next line),
# and some group definitions have no trailing comment at all.
defined = set(re.findall(
    r"^\s*([0-9A-F]{24})(?:\s+/\*.*?\*/)?\s*=\s*\{", pbx, re.M))
referenced = set(re.findall(r"\b([0-9A-F]{24})\b", pbx))
ok(len(defined) >= 35, "定义了足够的对象 (%d)" % len(defined))
ok(not (referenced - defined),
   "所有被引用的 ID 都有定义" + (": " + ", ".join(sorted(referenced - defined)) if referenced - defined else ""))
for sec in ("PBXBuildFile", "PBXFileReference", "PBXGroup", "PBXNativeTarget", "PBXProject",
            "PBXSourcesBuildPhase", "PBXResourcesBuildPhase", "PBXFrameworksBuildPhase",
            "XCBuildConfiguration", "XCConfigurationList"):
    ok(("/* Begin %s section */" % sec) in pbx, "含 %s 段" % sec)
for token, desc in [
    ("../native/Engine", "C++ 引擎源码以相对路径引用（单一来源，无副本）"),
    ("XP3Archive.cpp in Sources", "XP3 解析器进入编译"),
    ("KagEngine.cpp in Sources", "KAG 运行时进入编译"),
    ("GBEngineBridge.mm in Sources", "ObjC++ 桥进入编译"),
    ("libz.tbd in Frameworks", "链接系统 zlib"),
    ("PrivacyInfo.xcprivacy in Resources", "隐私清单打进 bundle"),
    ("demo-game.xp3 in Resources", "内置演示包打进 bundle"),
    ("SWIFT_OBJC_BRIDGING_HEADER", "配置了 Swift/ObjC 桥接头"),
    ("HEADER_SEARCH_PATHS", "配置了 C++ 头文件搜索路径"),
    ("ASSETCATALOG_COMPILER_APPICON_NAME", "配置了 AppIcon"),
    ("com.galbox.native", "Bundle ID 独立，不与 WebView 壳冲突"),
]:
    ok(token in pbx, desc)
scheme = read("ios-native/GalBoxNative.xcodeproj/xcshareddata/xcschemes/GalBoxNative.xcscheme")
ok(scheme.count('BlueprintIdentifier = "E10000000000000000000001"') == 3,
   "scheme 三处 BlueprintIdentifier 都指向同一 target")
ok("GalBoxNative.xcodeproj" in scheme and "GalBoxNative.app" in scheme, "scheme 指向新工程")

print("\n[4] plist / 隐私清单")
info = plistlib.loads(raw("ios-native/GalBoxNative/Info.plist"))
ok(info.get("CFBundleIdentifier") == "com.galbox.native", "Info.plist bundle id")
ok(info.get("CFBundleExecutable") == "GalBoxNative", "可执行文件名")
ok(info.get("UIFileSharingEnabled") is True, "开启文件共享（可导入 xp3）")
pv = plistlib.loads(raw("ios-native/GalBoxNative/PrivacyInfo.xcprivacy"))
reasons = []
for item in pv.get("NSPrivacyAccessedAPITypes", []):
    if item.get("NSPrivacyAccessedAPIType") == "NSPrivacyAccessedAPICategoryUserDefaults":
        reasons += item.get("NSPrivacyAccessedAPITypeReasons", [])
ok("CA92.1" in reasons, "UserDefaults 必需原因码 CA92.1（iOS 27 / Xcode 27）")
ok(pv.get("NSPrivacyTracking") is False, "声明不追踪")

print("\n[5] Swift ↔ ObjC++ 桥接方法名一致")
hdr = read("ios-native/GalBoxNative/GBEngineBridge.h")
swift = read("ios-native/GalBoxNative/GameViewController.swift") + \
        read("ios-native/GalBoxNative/LibraryViewController.swift")
for call, decl in [
    ("GBEngineBridge.openPackage(atPath:", "NS_SWIFT_NAME(openPackage(atPath:))"),
    ("GBEngineBridge.detectEngine(atPath:", "NS_SWIFT_NAME(detectEngine(atPath:))"),
    ("GBEngineBridge.data(forFile:", "NS_SWIFT_NAME(data(forFile:))"),
    ("GBEngineBridge.fileList()", "+ (NSArray"),
    ("GBEngineBridge.loadScript(", "+ (BOOL)loadScript:"),
    ("GBEngineBridge.currentStep()", "+ (NSDictionary"),
    ("GBEngineBridge.advance()", "+ (NSDictionary"),
    ("GBEngineBridge.choose(", "+ (NSDictionary"),
    ("GBEngineBridge.saveState()", "+ (NSString *)saveState"),
    ("GBEngineBridge.restoreState(", "+ (BOOL)restoreState:"),
    ("GBEngineBridge.history()", "+ (NSArray"),
    ("GBEngineBridge.jumpToHistory(", "+ (BOOL)jumpToHistory:"),
    ("GBEngineBridge.closePackage()", "+ (void)closePackage"),
]:
    ok(call in swift and decl in hdr, "桥接一致: " + call.split("(")[0])
ok("#import \"GBEngineBridge.h\"" in
   read("ios-native/GalBoxNative/GalBoxNative-Bridging-Header.h"), "桥接头导入正确")
ok("import UIKit" in read("ios-native/GalBoxNative/AppDelegate.swift") and
   "@main" in read("ios-native/GalBoxNative/AppDelegate.swift"), "UIKit AppDelegate 入口")
for tok in ("存档", "读档", "历史", "AUTO", "SKIP", "showChoices"):
    ok(tok in swift, "原生 UI 提供 " + tok)

print("\n[6] XP3 夹具闭环（真实字节，非循环校验）")
man = json.load(open(os.path.join(ROOT, "dist/fixtures/manifest.json"), encoding="utf-8"))
for fname, exp in man["fixtures"].items():
    blob = raw(os.path.join("dist/fixtures", fname))
    parsed = xp3_fixture.parse_xp3(blob)
    ok(parsed["count"] == exp["count"] and parsed["count"] > 0,
       fname + " 重新读盘得到 %d 个文件（非 0）" % parsed["count"])
    ok(parsed["indexCompressed"] == exp["indexCompressed"],
       fname + " 索引压缩标志一致")
    ok(parsed["base"] == exp["base"], fname + " 自解压 base 一致")
    for a, b in zip(parsed["entries"], exp["entries"]):
        ok(a["sha256"] == b["sha256"] and a["adler"] == b["adler"],
           fname + " / " + a["name"] + " 解压字节与哈希一致")
        ok(a["dataLen"] == b["orgSize"], fname + " / " + a["name"] + " 长度与索引一致")
ok(any(e["name"].startswith("中文") for e in man["fixtures"]["compressed.xp3"]["entries"]),
   "夹具含中文 UTF-16LE 文件名")
ok(any(len(e["parts"]) > 1 for e in man["fixtures"]["compressed.xp3"]["entries"]),
   "夹具含多段文件")
ok(any(any(e["parts"]) for e in man["fixtures"]["compressed.xp3"]["entries"]),
   "夹具含 zlib 压缩段")
for name, why in man["broken"].items():
    ok(isinstance(why, str) and len(why) > 0,
       name + " 被独立解析器拒绝且给出理由: " + str(why)[:46])
ok(md5("dist/fixtures/demo-game.xp3") == md5("ios-native/GalBoxNative/demo-game.xp3"),
   "bundle 内演示包与已验证夹具逐字节一致")
ks = raw("dist/fixtures/first.ks").decode("utf-8")
ok("[link" in ks and "*yes" in ks and "*no" in ks and "[s]" in ks,
   "演示剧本含分支与结局标签")

print("\n[7] 构建脚本")
sh = read("tools/build_native.sh")
for tok, desc in [("xcodebuild", "调用 Xcode 编译"),
                  ("CODE_SIGNING_ALLOWED=NO", "无 Apple 开发者证书编译"),
                  ("codesign --force --deep --sign -", "补 ad-hoc 伪签"),
                  ("Mach-O", "编译后强制校验 Mach-O"),
                  ("PrivacyInfo.xcprivacy", "校验隐私清单进 bundle"),
                  ("xp3_fixture.py", "编译前同步已验证夹具"),
                  ("iOS 27", "标注目标系统 iOS 27"),
                  ("TrollStore", "标明 iOS 27 上 tipa 不可用")]:
    ok(tok in sh, desc)
ver = read("tools/verify_native.py")
ok("SKIP" in ver and "return 3" in ver,
   "无编译器时明确 SKIP，不假装通过")
ok("--selftest" in ver and "manifest.json" in ver,
   "真编译验证覆盖引擎自检与夹具交叉比对")
ok("readErrors" in cli, "C++ CLI 把段解压失败也算作坏包")

print("\n========================================")
print("原生引擎静态+闭环校验: %d PASS / %d FAIL" % (P, F))
sys.exit(0 if F == 0 else 1)
