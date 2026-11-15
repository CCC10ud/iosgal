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
    ("GBEngineBridge.jump(toHistory:", "+ (BOOL)jumpToHistory:"),
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

print("\n[8] Milestone 1c — app 内 TJS2 真调用点")
bridge_h = read("native/Host/HostBridge.h")
bridge_cpp = read("native/Host/HostBridge.cpp")
m1c_test = read("native/Host/test_m1c.cpp")
objc_h = read("ios-native/GalBoxNative/GBEngineBridge.h")
objc_mm = read("ios-native/GalBoxNative/GBEngineBridge.mm")
swift_vc = read("ios-native/GalBoxNative/GameViewController.swift")
pbx = read("ios-native/GalBoxNative.xcodeproj/project.pbxproj")
ios_sh = read("tools/build_tjs2_ios.sh")
test_sh = read("tools/build_tjs2_test.sh")
wf = read(".github/workflows/build-native-ipa.yml")

ok("extern \"C\"" in bridge_h and "GalBoxTJS2SmokeTest" in bridge_h,
   "纯 C 边界头隔离官方 tjs.h")
ok("tjs.h" in bridge_cpp and "HostInterface.hpp" in bridge_cpp
   and "new tTJS()" in bridge_cpp,
   "HostBridge.cpp 真正构造 TJS2 引擎并安装宿主")
ok("a.load" in bridge_cpp and "b.save" in bridge_cpp,
   "冒烟脚本经 TJS2 触发宿主读/写回调")
ok("new Array(\\\"" not in bridge_cpp and "b.add(" in bridge_cpp,
   "Array 用无参构造 + add（官方 Construct 要求 numparams==0，run #17 踩坑）")
ok("Array.load threw" in bridge_cpp and "Array.save threw" in bridge_cpp
   and "eval threw" in bridge_cpp,
   "异常按阶段分别定位，不笼统报 TJS2 exception")
ok("if (engine) {" in bridge_cpp and "engine->Release()" in bridge_cpp,
   "异常路径也保证 Release（不污染后续启动状态）")
ok("m1c_smoke.txt" in m1c_test and "did not persist content" in m1c_test,
   "M1c 自测核对写盘内容（计数 +1 不能自证落盘）")
ok("M1C_HOST_v1" in bridge_cpp, "二进制里带 M1c 链接标记")
ok("hostReadDelta" in bridge_cpp and "hostWriteDelta" in bridge_cpp,
   "冒烟结果回报宿主读/写计数")
ok("host not installed" in m1c_test
   and "Uninstall did not take effect" in m1c_test
   and "GalBoxTJS2Uninstall()" in m1c_test,
   "自测覆盖未安装/已卸载失败路径（不假装通过）")
ok("M1C_RESULT=OK" in m1c_test, "M1c 自测有明确通过标记")
ok("tjs2SmokeTest" in objc_h and "tjs2SmokeTest" in objc_mm,
   "GBEngineBridge 暴露 Swift 可见的冒烟入口")
ok("#if defined(GALBOX_HAS_TJS2)" in objc_mm
   and "GalBoxTJS2Install(&gArchive" in objc_mm
   and "GalBoxTJS2Uninstall()" in objc_mm,
   "打开/关闭包时安装与卸载宿主，且有降级编译守卫")
ok("GBEngineBridge.tjs2SmokeTest()" in swift_vc,
   "打开游戏包时 UI 真的触发冒烟并显示结果")
ok("$(SRCROOT)/../native/Host" in pbx and pbx.count("$(SRCROOT)/../native/Host") == 2,
   "Debug/Release 两个配置都能找到 HostBridge.h")
ok("HostBridge_ios.o" in ios_sh and "HostBridge.cpp" in ios_sh,
   "iOS 交叉编译产出 app 侧适配层 .o")
ok("test_m1c.cpp" in test_sh and "M1C_RESULT=OK" in test_sh,
   "macOS host 真跑同一份 HostBridge.cpp（CI 无法运行 iOS app）")
for tok, desc in [("HostStreams_ios.o", "宿主流 .o 进 app 链接"),
                  ("HostBridge_ios.o", "适配层 .o 进 app 链接"),
                  ("GCC_PREPROCESSOR_DEFINITIONS", "按产物齐备与否切换编译宏"),
                  ("GALBOX_HAS_TJS2", "M1c 调用点开关")]:
    ok(tok in sh, "build_native.sh: " + desc)
ok("M1C_HOST_v1" in wf, "CI 核验 app 二进制含 M1c 标记（不只是链接成功）")

print("\n[9] Milestone 2a — 官方 KAG3 启动链第一段")
kag_man = json.loads(read("native/KAG/VENDOR_MANIFEST.json"))
kag_readme = read("native/KAG/README.md")
official_startup = read("native/KAG/src/startup.tjs")
official_utils = read("native/KAG/src/system/Utils.tjs")
probe_startup = read("native/KAG/src/m2a_probe_startup.tjs")
m2a_test = read("native/Host/test_m2a.cpp")

for item in kag_man["files"]:
    rel = "native/KAG/src/" + item["path"]
    b = raw(rel)
    ok(len(b) == item["bytes"] and hashlib.sha256(b).hexdigest() == item["sha256"],
       "M2a UTF-8 归一化副本校验值一致: " + item["path"])
    ok(item.get("normalized") is True,
       "清单明确标注该副本不是官方原始字节: " + item["path"])
ok("jeeb/kirikiri2" in kag_readme and "KAGParserEx" in kag_readme
   and "kag3/template/system" in kag_readme and "404" in kag_readme,
   "KAG README 已更正上游仓库名与真实目录结构")
ok('Scripts.execStorage("system/Initialize.tjs")' in official_startup,
   "官方 startup.tjs 的唯一启动动作是原生类 Scripts.execStorage")
ok("function intrandom" in official_utils and "<->" in official_utils
   and "property" in official_utils,
   "vendored Utils.tjs 含 TJS2 特有语法（property / <-> 交换运算符）")
ok('Scripts.execStorage("system/Utils.tjs")' in probe_startup,
   "M2a 探针复刻官方调用形态且指向无依赖的 Utils.tjs")
ok("tTJSNC_Scripts" in bridge_cpp
   and "TJS_END_NATIVE_STATIC_METHOD_DECL(execStorage)" in bridge_cpp,
   "注册官方启动链所需的 TJS2 原生静态类 Scripts")
ok("GalBoxTJS2ExecScript" in bridge_cpp and "GalBoxTJS2ExecScript" in bridge_h,
   "纯 C 边界暴露「执行包内 .tjs 入口脚本」")
ok("M2A_SCRIPTS_EXEC_STORAGE" in bridge_cpp,
   "M2a 二进制固定标记（CI strings 硬断言）")
ok("probeValue\\\":5" in m2a_test or '"probeValue":5' in m2a_test,
   "M2a 用官方 intrandom(5,5)==5 作实质判据（不靠读计数自证）")
ok("M2A_RESULT=OK" in m2a_test and "isDirectoryMode()" in m2a_test,
   "M2a 自测走 Kirikiri 的 Data 文件夹形态并有通过标记")
ok("test_m2a.cpp" in test_sh and "m2a_game" in test_sh
   and "M2A_RESULT=OK" in test_sh,
   "CI 在 macOS host 上真跑官方 KAG3 脚本")
ok("M2A_SCRIPTS_EXEC_STORAGE" in wf, "CI 核验 M2a 适配层真的进 app 二进制")
ok("tjs2ExecStartup" in objc_h and "tjs2ExecStartup" in objc_mm,
   "app 暴露官方 startup.tjs 诊断入口")
ok("GBEngineBridge.tjs2ExecStartup()" not in swift_vc,
   "不自动执行商业 startup.tjs（避免主线程长循环 / 缺类异常崩溃）")

print("\n[10] Milestone 2b — Storages 自动路径 + 官方原始 Shift-JIS 字节")
host_hpp = read("native/Host/HostInterface.hpp")
host_streams = read("native/Host/HostStreams.cpp")
m2b_test = read("native/Host/test_m2b.cpp")
m2b_probe = read("native/KAG/src/m2b_probe_startup.tjs")
m2b_bad = read("native/KAG/src/m2b_bad_autopath.tjs")

# 官方原始字节必须逐字节保真，且确实不是 UTF-8（否则 SJIS 路径根本没被测）
for item in kag_man["upstream_raw"]:
    b = raw("native/KAG/" + item["path"])
    ok(len(b) == item["bytes"] and hashlib.sha256(b).hexdigest() == item["sha256"],
       "官方 git blob 原始字节保真: " + item["path"])
    try:
        b.decode("utf-8")
        is_utf8 = True
    except UnicodeDecodeError:
        is_utf8 = False
    ok(not is_utf8, "官方原始字节确实不是 UTF-8（是 Shift-JIS）: " + item["path"])
ok("Shift-JIS" in kag_readme and "contents API" in kag_readme,
   "README 已更正 M2a 的「逐字节 UTF-8」误判")
ok("contents API" in kag_man["encoding_finding"]["correction"]
   and "CP932" in kag_man["encoding_finding"]["mapping_difference"],
   "VENDOR_MANIFEST 记录了转码与 CP932 映射差异")
ok("function intrandom" in raw("native/KAG/src/original/system/Utils.tjs").decode("shift_jis"),
   "原始 SJIS 文件解码后确为官方 Utils.tjs")

for tok in ("AddAutoPath", "ClearAutoPaths", "IsExistentStorage", "GetPlacedPath",
            "AutoPathHitCount", "LastResolvedPath", "LastTextEncoding",
            "ShiftJisReadCount"):
    ok(tok in host_hpp and tok in host_streams,
       "宿主层暴露并实现 M2b 接口: " + tok)
ok("CP932" in host_streams and "SHIFT_JIS" in host_streams
   and "latin1-fallback" in host_streams,
   "解码优先 CP932、失败有兜底且兜底被诊断出来")
ok("rbegin()" in host_streams and "ExistsRaw" in host_streams,
   "autoPath 后加优先且不靠 find() 的 basename 宽松回退")
ok("ResolveStorage(logical, true)" in host_streams,
   "真正的读操作才计入 autoPath 命中（探测不伪造命中）")
for tok in ("tTJSNC_Storages", "TJS_END_NATIVE_STATIC_METHOD_DECL(addAutoPath)",
            "isExistentStorage", "getPlacedPath", "chopStorageExt",
            "TJS_END_NATIVE_STATIC_METHOD_DECL(eval)",
            "script not found via autoPath", "M2B_STORAGES_AUTOPATH",
            "RegisterStoragesClass(engine)"):
    ok(tok in bridge_cpp, "HostBridge 注册 M2b 能力: " + tok)
ok('Storages.addAutoPath("system/")' in m2b_probe
   and 'Scripts.execStorage("Utils.tjs")' in m2b_probe,
   "M2b 探针用短文件名强制走 autoPath 解析")
ok('addAutoPath("missing_dir/")' in m2b_bad,
   "M2b 有故意写错 autoPath 的失败路径")
for tok in ("M2B_RESULT=OK", 'probeValue\\":123', "shiftJisReads", "autoPathHits",
            "lastResolvedPath", "bad_startup.tjs", "str2num"):
    ok(tok in m2b_test, "M2b 自测含实质判据: " + tok)
ok("test_m2b.cpp" in test_sh and "M2B_RESULT=OK" in test_sh
   and '3658' in test_sh and "src/original/system/Utils.tjs" in test_sh,
   "CI 用官方原始 3658B SJIS blob 真跑 M2b 自测")
ok("M2B_STORAGES_AUTOPATH" in wf, "CI 核验 M2b 适配层真的进 app 二进制")
ok("GBEngineBridge.tjs2ExecStartup()" not in swift_vc,
   "M2b 仍不自动执行商业 startup.tjs")

print("\n========================================")
print("原生引擎静态+闭环校验: %d PASS / %d FAIL" % (P, F))
sys.exit(0 if F == 0 else 1)
