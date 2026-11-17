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
_i_bg = swift_vc.find("DispatchQueue.global(qos: .userInitiated).async")
_i_call = swift_vc.find("GBEngineBridge.tjs2ExecStartup()")
ok(_i_bg != -1 and _i_call != -1 and _i_bg < _i_call,
   "M2c 自动跑官方 Initialize.tjs，但调用点在后台队列内（不在主线程同步执行）")

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
    # M2e-1 起 upstream_raw 还含官方 png/asd 资源；它们不是文本脚本，
    # 「不是 UTF-8」这条编码判据只适用于 .tjs（二进制/ASCII 资源只验保真）。
    if item["path"].endswith(".tjs"):
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
ok("System / Debug" in swift_vc,
   "M2b 阶段 UI 文案已随 M2c 更新（不再说「System 未移植」）")

print("\n[11] Milestone 2c — 官方 Initialize.tjs 原文 + System / Debug")
m2c_test = read("native/Host/test_m2c.cpp")
m2c_cfg = read("native/KAG/src/m2c_probe_config.tjs")
official_init = raw("native/KAG/src/original/system/Initialize.tjs").decode("shift_jis")

# 本轮实现的两个原生类必须逐条对上官方 Initialize.tjs 的真实语句。
for tok in ("tTJSNC_System", "tTJSNC_Debug",
            "TJS_END_NATIVE_STATIC_PROP_DECL(exceptionHandler)",
            "TJS_END_NATIVE_STATIC_PROP_DECL(exePath)",
            "TJS_END_NATIVE_STATIC_METHOD_DECL(getTickCount)",
            "TJS_END_NATIVE_STATIC_METHOD_DECL(inform)",
            "TJS_END_NATIVE_STATIC_METHOD_DECL(createAppLock)",
            "TJS_END_NATIVE_STATIC_METHOD_DECL(getArgument)",
            "TJS_END_NATIVE_STATIC_METHOD_DECL(notice)",
            "TJS_END_NATIVE_STATIC_METHOD_DECL(message)",
            "RegisterSystemClass(engine)", "RegisterDebugClass(engine)",
            "M2C_SYSTEM_DEBUG"):
    ok(tok in bridge_cpp, "HostBridge 注册 M2c 能力: " + tok)

# 官方脚本里出现过的语句，必须在实现里有对应成员（防「凭印象瞎写」）。
for src_call, member in (("System.exceptionHandler =", "exceptionHandler"),
                         ("System.exePath", "exePath"),
                         ("System.getTickCount", "getTickCount"),
                         ("System.inform", "inform"),
                         ("System.createAppLock", "createAppLock"),
                         ("System.getArgument", "getArgument"),
                         ("Debug.notice", "notice"),
                         ("var dm = Debug.message", "message")):
    ok(src_call in official_init and member in bridge_cpp,
       "官方 Initialize.tjs 用到的成员已实现: " + member)

# 诊断能力：真实异常正文 / 脚本名 / 行号必须被捕获，而不是笼统一句。
for tok in ("CaptureCurrentException", "fatalException", "fatalLine",
            "fatalBlock", "eTJSScriptException", "GetSourceLine"):
    ok(tok in bridge_cpp, "异常定位字段齐全: " + tok)
ok("CaptureCurrentException()" in bridge_cpp,
   "execStorage 的 catch 不再吞掉内层脚本的真实异常")
ok("ClearEngineScopedDiagnostics" in bridge_cpp,
   "引擎销毁前清掉持有 TJS 对象的字段（避免变量比引擎活得久）")

# 自测必须两个场景都覆盖，且明确接受 ok=false（不假装跑通）。
for tok in ("M2C_RESULT=OK", "m2c_game_cfg", "M2C_SYSTEM_DEBUG",
            "exceptionHandlerSet", "autoPathCount", "debugNotices",
            "tickCalls", "fatalBlock", "fatalLine", "fatalException",
            "debugMessages", "execStorageAttempts"):
    ok(tok in m2c_test, "M2c 自测含实质判据: " + tok)
# M2d 补上正则后，M2c 的 B 场景判据从「失败原因含 RegExp」改成「推过 createAppLock」，
# 否则「修好了」这件事本身会让 M2c 回归变红。
ok('appLock\\":true' in m2c_test,
   "M2c 自测随 M2d 更新：断言推过 createAppLock 而不是卡在 RegExp")
ok('ok\\":false' in m2c_test,
   "M2c 自测明确断言 ok=false（本轮不假装 Initialize.tjs 跑通）")
ok('global.config_version = "3.29-dev.20071022"' in m2c_cfg,
   "探针 Config.tjs 用 global. 前缀创建版本号（裸赋值不创建成员，会抛错）")
_cfg_code = [l for l in m2c_cfg.splitlines() if not l.strip().startswith("//")]
ok(all(l.strip() == "" or l.strip().startswith("global.config_version")
       for l in _cfg_code),
   "探针 Config.tjs 只做版本核对，没有夹带其他语义")
ok("不是**官方文件**" in m2c_cfg or "不是" in m2c_cfg,
   "探针 Config.tjs 明确标注为非官方文件")
ok("test_m2c.cpp" in test_sh and "M2C_RESULT=OK" in test_sh
   and "7246" in test_sh and "Initialize.tjs" in test_sh,
   "CI 用官方 7246B SJIS Initialize.tjs 原文真跑 M2c 自测")
ok("M2C_SYSTEM_DEBUG" in wf, "CI 核验 System/Debug 原生类真的进 app 二进制")

# M2c 当轮的最外层阻塞点（正则）在 M2d 已解决；注释必须记录这件事，
# 否则下一个人会以为 TJS_NO_REGEXP 还在用。
ok("M2d" in bridge_cpp and "std::regex" in bridge_cpp,
   "注释记录了 M2d 拆掉正则阻塞点（不再定义 TJS_NO_REGEXP）")

print("\n[12] Milestone 2d — 正则层（std::regex 重写的 RegExp 原生类）")
re_h = read("native/TJS2/src/tjsRegExp.h")
re_cpp = read("native/TJS2/src/tjsRegExp.cpp")
m2d_test = read("native/Host/test_m2d.cpp")
m2d_probe = read("native/KAG/src/m2d_regex_probe.tjs")
patches_md = read("native/TJS2/PATCHES.md")

# 1) 不能再引 boost（否则 runner 要装依赖，iOS 侧还要能编）
ok("boost/regex.hpp" not in re_h and "boost/regex.hpp" not in re_cpp,
   "正则层不再 include boost/regex.hpp")
ok("std::basic_regex" in re_h and "std::regex_search" in re_cpp,
   "正则层后端是 std::basic_regex / std::regex_search")
ok("<regex>" in re_h and "<regex>" in re_cpp, "显式包含 <regex>")

# 2) 上游的接入点必须原样保留（否则 tjs.cpp / tjsArray.cpp 会链错）
for tok, desc in (("tTJSNC_RegExp", "类名与上游一致"),
                  ("tTJSNI_RegExp", "实例类名与上游一致"),
                  ("TJSCreateRegExpClass", "工厂函数名与上游一致"),
                  ("static tjs_uint32 ClassID", "ClassID 供 tjsArray.cpp 判定"),
                  ("void Split(iTJSDispatch2 ** array, const ttstr &target, bool purgeempty)",
                   "Split 签名兼容 Array.split 的 regexp 分支"),
                  ("TJS_END_NATIVE_METHOD_DECL(/*func. name*/_compile)",
                   "_compile 存在（正则字面量唯一入口）"),
                  ("TJS_END_NATIVE_METHOD_DECL(/*func. name*/replace)", "replace 成员"),
                  ("TJS_END_NATIVE_METHOD_DECL(/*func. name*/split)", "split 成员"),
                  ("TJS_END_NATIVE_METHOD_DECL(/*func. name*/test)", "test 成员"),
                  ("TJS_END_NATIVE_METHOD_DECL(/*func. name*/exec)", "exec 成员"),
                  ("TJS_END_NATIVE_METHOD_DECL(/*func. name*/match)", "match 成员"),
                  ("TJS_END_NATIVE_PROP_DECL(lastMatch)", "lastMatch 属性"),
                  ("TJS_END_NATIVE_PROP_DECL(lastParen)", "lastParen 属性"),
                  ("TJS_END_NATIVE_PROP_DECL(leftContext)", "leftContext 属性"),
                  ("TJS_END_NATIVE_PROP_DECL(rightContext)", "rightContext 属性")):
    ok(tok in re_cpp or tok in re_h, "正则层保留上游接口: " + desc)

ok("regex_grep(" not in re_cpp and "bind1st(" not in re_cpp,
   "不再调用 boost::regex_grep / std::bind1st（注释里提到型号不算）")

# 3) 与 boost 的语义差异必须有处理 + 有判据
ok("[\\s\\S]" in re_cpp, "把 boost 的「. 吃换行」翻成 [\\s\\S]")
ok("dot-matches-newline" in m2d_probe, "探针覆盖「. 匹配换行」")
ok("boost" in patches_md and "P7" in patches_md, "PATCHES.md 记录了 P7 正则层重写")

# 4) 编译开关：不再定义 TJS_NO_REGEXP，且 tjsRegExp.cpp 必须参与编译
ok("-DTJS_NO_REGEXP" not in test_sh and "-DTJS_NO_REGEXP" not in ios_sh,
   "构建脚本不再定义 TJS_NO_REGEXP（否则全局 RegExp 不会被注册）")
ok('if [ "$b" = "tjsRegExp.cpp" ]; then' not in test_sh
   and 'if [ "$b" = "tjsRegExp.cpp" ]; then continue; fi' not in ios_sh,
   "tjsRegExp.cpp 不再被排除出编译列表")

# 5) 宿主侧证据：appLockKey 必须来自脚本侧正则，exePath 必须非空
ok("appLockKey" in bridge_cpp and "M2D_REGEXP_STDREGEX" in bridge_cpp,
   "HostBridge 记录 createAppLock 键值并带 M2d 固定标记")
ok("StorageRootPath" in read("native/Host/HostInterface.hpp")
   and "StorageRootPath" in read("native/Host/HostStreams.cpp")
   and "StorageRootPath" in bridge_cpp,
   "System.exePath 由宿主真实上报（正则才有非空输入）")

# 6) 判据本身要防「0 失败靠没跑凑出来」
ok("ExpectedLockKey" in m2d_test and "exePath.replace" in m2d_test,
   "自测独立实现 exePath→键值 的逐字符换算（非循环校验）")
ok("probe < 0" in m2d_test and "probe % 100" in m2d_test and "probe / 100 < 20" in m2d_test,
   "自测能区分「探针没执行」「有失败」「断言太少」三种情况")
ok("global.m2dFails" in m2d_probe and "global.m2dChecks" in m2d_probe,
   "探针把失败数与检查数都写回 global（不被裸赋值坑）")
ok("KAGLayer.tjs" in m2d_test, "自测断言「下一个缺口」是 KAGLayer.tjs 而非正则")
ok("M2D_RESULT=OK" in test_sh and "test_m2d.cpp" in test_sh,
   "CI 运行 M2d 自测")
ok("M2D_REGEXP_STDREGEX" in wf, "CI 核验 std::regex 正则层真的进 app 二进制")


print("\n[13] Milestone 2e-1 — 官方 KAG 系统脚本全量 vendoring")
kag_manifest = kag_man
m2e_test = read("native/Host/test_m2e.cpp")
m2e_probe = read("native/KAG/src/m2e_manifest_probe.tjs")
streams_cpp = read("native/Host/HostStreams.cpp")
swift_vc2 = read("ios-native/GalBoxNative/GameViewController.swift")

sys_items = [e for e in kag_manifest["upstream_raw"]
             if e["path"].startswith("src/original/system/")]
ok(len(sys_items) == 28, "清单列出上游 system/ 的全部 28 个 blob")
ok(sum(e["bytes"] for e in sys_items) == 448729, "28 个 blob 合计 448,729 B")
ok(len([e for e in sys_items if e["path"].endswith(".tjs")]) == 20,
   "其中 20 个是官方 .tjs 系统脚本")
ok(kag_manifest.get("m2e_full_vendoring", {}).get("total_bytes") == 448729,
   "清单单独记录本轮全量 vendoring 的总字节数")

# 真闭环：逐文件重新算 sha256 + 字节数，与清单和磁盘比对。
# 只检查「文件存在」的话，M2a 那次 contents API 静默转码就会蒙混过关。
_bad = []
for e in sys_items:
    fp = "native/KAG/" + e["path"]
    blob = raw(fp)
    if len(blob) != e["bytes"] or hashlib.sha256(blob).hexdigest() != e["sha256"]:
        _bad.append(e["path"])
ok(not _bad, "28 个官方文件与清单 sha256/bytes 逐字节一致: " + str(_bad[:3]))
# 明确断言不是 UTF-8 转码副本：Initialize.tjs 必须仍是 7246 B 的 SJIS 原文
_init = raw("native/KAG/src/original/system/Initialize.tjs")
ok(len(_init) == 7246 and b"\x83" in _init[:64],
   "Initialize.tjs 仍是官方 SJIS 原始字节（非 contents API 转码副本）")

ok("sha256" in test_sh and "448729" in test_sh and "M2E_COUNT" in test_sh,
   "构建脚本逐文件校验官方 blob 并核对夹具文件数")
ok("M2E_RESULT=OK" in test_sh and "test_m2e.cpp" in test_sh,
   "CI 运行 M2e 自测")
ok("M2E_SYSTEM_SCRIPTS_VENDORED" in wf, "CI 核验 M2e 标记真的进 app 二进制")
ok("M2E_SYSTEM_SCRIPTS_VENDORED" in bridge_cpp, "HostBridge 带 M2e 固定标记")

# 历史标记一个都不能被升级覆盖掉（M2c 差点踩过）
for tag in ("M1C_HOST_v1", "M2A_SCRIPTS_EXEC_STORAGE",
            "M2B_STORAGES_AUTOPATH", "M2C_SYSTEM_DEBUG",
            "M2D_REGEXP_STDREGEX"):
    ok(tag in bridge_cpp, "升级后历史二进制标记仍保留: " + tag)

# scriptTrace 必须只在「流成功读到正文之后」追加，否则「文件没找到」也会算命中
ok("gScriptTrace" in bridge_cpp and "gScriptsRead" in bridge_cpp,
   "HostBridge 记录脚本读取轨迹")
ok("gScriptTrace += nameU8" in bridge_cpp and "scriptTrace" in bridge_cpp,
   "轨迹只在读流成功后写入，并作为 JSON 字段暴露")
ok("gShiftJisReads = 0" in streams_cpp,
   "每次 Install 清零 SJIS 计数（否则后一次启动会借上一次的判据）")

# 判据本身要能区分三种情况，且必须有负向对照
ok("probe != 2000" in m2e_test and "scriptsRead\") != 0" in m2e_test,
   "清单探针能区分「文件数不对」与「存在性检查恒真」")
ok("__m2e_no_such_file__.tjs" in m2e_probe and "global.m2eProbe" in m2e_probe,
   "探针含必然不存在的哨兵，结果写回 global（不被裸赋值坑）")
ok("negative control" in m2e_test and "script not found via autoPath: KAGLayer.tjs" in m2e_test,
   "自测有无系统脚本的负向对照，证明 scriptTrace 不是恒真计数器")
ok("KAGLayer.tjs was never actually read" in m2e_test
   and "block.find(\"KAGLayer\")" in m2e_test,
   "自测断言阻塞点从「缺文件」变成「KAGLayer 里缺 Layer 原生类」")
ok("shiftJisReads\") < 4" in m2e_test
   and "M2d regression: createAppLock no longer reached" in m2e_test,
   "自测同时守住 SJIS 解码链与 M2d 正则回归")
ok("448729" in m2e_test and "VerifyVendoredBytes" in m2e_test,
   "C++ 自测独立校验 28 个文件的精确字节数")
ok("scriptsRead" in swift_vc2, "app 诊断 toast 展示真实读到的系统脚本数")


print("\n[14] Milestone 2e-2 — GalBox 的 Layer 骨架（纯 TJS2 shim）")
m2f_test = read("native/Host/test_m2f.cpp")
m2f_probe = read("native/KAG/src/m2f_layer_probe.tjs")
shim = read("native/KAG/src/system/GalBoxLayer.tjs")

# 1) 骨架必须是 GalBox 自写、且**不是**官方原文目录下的东西
ok("class Layer" in shim and "非官方" in shim,
   "Layer 骨架是 GalBox 自写的脚本类，并明确标注非官方")
# M2e-2 时这些常量是 GalBox 自造的（还把 ststStaySrc/Dest 弄反了）；
# M2e-4 起改由官方常量表逐字提供，见 [16] 段。
ok("GalBoxConsts.tjs" in read("native/KAG/src/system/GalBoxConsts.tjs"),
   "颜色键 / 转场枚举改由官方常量表提供（M2e-2 的自造值已作废）")
for member in ("visible", "left", "top", "opacity", "absolute", "type",
               "width", "height", "imageLeft", "imageTop", "isPrimary",
               "parent", "window"):
    ok(member in shim, "骨架占位成员: " + member)
for fn in ("function Layer(", "function finalize(", "function loadImages(",
           "function assignImages(", "function setPos(", "function setImagePos(",
           "function setMode(", "function focus(", "function removeMode(",
           "function beginTransition(", "function onTransitionCompleted("):
    ok(fn in shim, "骨架占位方法: " + fn)

# 2) 宿主必须能加载它，且**不污染**官方脚本加载序列的三条判据
ok("LoadEngineShim" in bridge_cpp and "gShimStatus" in bridge_cpp,
   "HostBridge 会加载 Layer 骨架并单独记录状态")
ok("system/GalBoxLayer.tjs" in bridge_cpp and "M2E2_LAYER_SKELETON" in bridge_cpp,
   "骨架路径与 M2e-2 固定标记都在 HostBridge 里")
_shim_start = bridge_cpp.index("static bool LoadEngineShim")
_shim_fn = bridge_cpp[_shim_start:bridge_cpp.index("\n}\n", _shim_start)]
for forbidden in ("gExecStorageAttempts", "gScriptTrace +=", "gLastScriptError ="):
    ok(forbidden not in _shim_fn,
       "加载骨架不污染官方加载序列的判据: " + forbidden)
ok("CaptureCurrentException" not in _shim_fn,
   "骨架自身的失败不占用 fatalBlock/fatalLine（那是入口脚本的定位）")
for tag in ("M1C_HOST_v1", "M2A_SCRIPTS_EXEC_STORAGE", "M2B_STORAGES_AUTOPATH",
            "M2C_SYSTEM_DEBUG", "M2D_REGEXP_STDREGEX",
            "M2E_SYSTEM_SCRIPTS_VENDORED"):
    ok(tag in bridge_cpp, "升级后历史二进制标记仍保留: " + tag)

# 3) 判据要能区分「继承成立」与「探针压根没跑」，并且必须有负向对照
ok("new KAGLayer(null, null)" in m2f_probe and "global.m2fProbe = 1" in m2f_probe,
   "探针真的实例化官方脚本类，并把结果写回 global")
ok('typeof m2fProbe == \\"undefined\\"' in m2f_test or
   "typeof m2fProbe" in m2f_test,
   "自测能区分「实例化抛异常」(probeValue=-1) 与「实例化成功」(probeValue=1)")
ok("negative control" in m2f_test
   and "did not reproduce the Layer blocker" in m2f_test,
   "自测有无骨架的负向对照，证明骨架真的在起作用")
ok("still blocked by the missing Layer class" in m2f_test,
   "自测断言 Layer 缺口在有骨架时消失")
ok("scripts < 10" in m2f_test,
   "自测断言官方脚本加载序列不比 M2e-1 倒退")
ok("M2F_RESULT=OK" in test_sh and "test_m2f.cpp" in test_sh,
   "CI 运行 M2e-2 自测")
ok("M2E2_LAYER_SKELETON" in wf, "CI 核验 Layer 骨架层真的进 app 二进制")


print("\n[15] Milestone 2e-3 — GalBox 的 Window 骨架（纯 TJS2 shim）+ System.dataPath / Debug.logLocation")
m2g_test = read("native/Host/test_m2g.cpp")
m2g_probe = read("native/KAG/src/m2g_window_probe.tjs")
win_shim = read("native/KAG/src/system/GalBoxWindow.tjs")

ok("class Window" in win_shim and "非官方" in win_shim,
   "Window 骨架是 GalBox 自写的脚本类，并明确标注非官方")
# run #38 踩过的两个语法雷：TJS2 不支持 `...` 变参定义，也没有
# `function invalidate()` 这种以关键字命名的函数。撞上会让**整个** shim
# 语法错误、连带 Window 完全装不上。
_win_code = "\n".join(l for l in win_shim.splitlines()
                      if not l.strip().startswith("//"))
ok("(...)" not in _win_code,
   "Window 骨架不用 `...` 变参定义（TJS2 没有这个记号）")
ok("function invalidate(" not in win_shim,
   "Window 骨架不用 TJS2 关键字 invalidate 作函数名")
for member in ("caption", "visible", "width", "height", "layer", "primaryLayer",
               "children", "parent", "menu", "fullScreen", "cursor"):
    ok(member in win_shim, "Window 骨架占位属性: " + member)
for fn in ("function Window(", "function finalize(", "function close(",
           "function onCloseQuery(", "function onMouseDown(", "function onKeyDown(",
           "function onMouseWheel(", "function setCaption(", "function setSize(",
           "function setCursor(", "function createLayer("):
    ok(fn in win_shim, "Window 骨架占位方法: " + fn)

# 宿主侧：Window 骨架路径 + 固定标记 + 这轮顺带补的两个成员
ok("system/GalBoxWindow.tjs" in bridge_cpp and "M2E3_WINDOW_SKELETON" in bridge_cpp,
   "HostBridge 会加载 Window 骨架并带 M2e-3 固定标记")
# run #38 踩过：骨架语法错误时状态串里照样出现了 tag，判据差点被骗过去。
# 现在 tag 只在 ExecScript 成功后才进 status，失败另进 failed。
_shim_start2 = bridge_cpp.index("static bool LoadEngineShim")
_shim_fn2 = bridge_cpp[_shim_start2:bridge_cpp.index("\n}\n", _shim_start2)]
ok("status += kTags[i];" in _shim_fn2
   and _shim_fn2.index("status += kTags[i];") >
       _shim_fn2.index("engine->ExecScript(body.c_str()"),
   "骨架 tag 只在真正装载成功后才写入状态串（防「标记在但没装上」）")
ok("failed:" in _shim_fn2 and "GetSourceLine()" in _shim_fn2,
   "骨架装载失败会记录 block/line（run #38 只看到一句 Syntax error）")
ok("TJS_END_NATIVE_STATIC_PROP_DECL(dataPath)" in bridge_cpp
   and "TJS_END_NATIVE_STATIC_PROP_DECL(logLocation)" in bridge_cpp,
   "补上 System.dataPath 与 Debug.logLocation（KAGWindow 实例化路径要用）")
ok("StorageRootPath()" in bridge_cpp and "logLocation" in bridge_cpp,
   "dataPath 来自宿主真实上报的目录，不是硬编码空串")

# 判据：围绕「实例化时的父类查找」，不依赖「脚本被读完」
ok("new KAGWindow()" in m2g_probe and "global.m2gProbe = 1" in m2g_probe,
   "探针真的实例化官方 KAGWindow（TJS2 的 super 是惰性的，必须 new 一次）")
ok("m2gLoaded" in m2g_probe and "-2" in m2g_test,
   "判据能区分「脚本没读到」(-2) 与「实例化失败」(-1) 与「成功」(1)")
ok("negative control" in m2g_test
   and "did not reproduce the Window blocker" in m2g_test,
   "自测有无 Window 骨架的负向对照，证明骨架真的在起作用")
ok("still blocked by the missing Window class" in m2g_test,
   "自测断言 Window 缺口在有骨架时消失")
# 补上 Window 骨架后，Initialize.tjs 第 232 行的 `new KAGWindow()` 真的会执行，
# 断点从「加载 MainWindow.tjs」前移到「KAGWindow 类体/构造器」内部。
# ⇒ 「脚本数不倒退」不再是本轮的硬判据（本轮硬判据是「Window 缺口消除」），
#    但必须把观测值打全，供下一轮定位这个新现象。
ok("autoPathCount" in m2g_test and "hostReadDelta" in m2g_test
   and "scriptsRead" in m2g_test,
   "自测把场景 B 的观测值打全（脚本数/attempts/calls/autoPathCount/hostReadDelta）")
ok("the official script chain got shorter than in M2e-2" not in m2g_test,
   "自测不再把「脚本数不倒退」当硬判据（断点前移属新课题，另开一轮定位）")
ok("M2G_RESULT=OK" in test_sh and "test_m2g.cpp" in test_sh,
   "CI 运行 M2e-3 自测")
ok("M2E3_WINDOW_SKELETON" in wf, "CI 核验 Window 骨架层真的进 app 二进制")
# macOS runner 队列拥堵会把迭代卡死（实测排队 15 分钟+ 仍被 cancel）；
# 而 TJS2 的真编译真运行自测**不需要 Xcode**，只要 clang++ 与 zlib。
ok("ubuntu-latest" in wf and "build_tjs2_test.sh" in wf,
   "workflow 里有一个 Linux runner 的 TJS2 自测 job（绕开 macOS runner 排队）")
ok("ICONV_LIB" in test_sh and "glibc" in test_sh,
   "自测脚本按平台探测 iconv 链接参数（glibc 没有独立 libiconv）")
compat_h = read("native/TJS2/tjs2_compat.h")
# run #47：Linux runner 首次真编译暴露 macOS libc++ 隐式引入的 3 个标准头。
# glibc 不隐式给 std::string / wcscmp / INT_MAX，必须在注入头里显式补齐。
for hdr in ("<cwchar>", "<climits>", "<string>"):
    ok("#include " + hdr in compat_h,
       "兼容头显式引入 " + hdr + "（Linux/glibc 编译必需，macOS 靠隐式引入会漏）")
for tag in ("M1C_HOST_v1", "M2A_SCRIPTS_EXEC_STORAGE", "M2B_STORAGES_AUTOPATH",
            "M2C_SYSTEM_DEBUG", "M2D_REGEXP_STDREGEX",
            "M2E_SYSTEM_SCRIPTS_VENDORED", "M2E2_LAYER_SKELETON"):
    ok(tag in bridge_cpp, "升级后历史二进制标记仍保留: " + tag)


print("\n[16] Milestone 2e-4 — 官方全局常量表（摘自 ScriptMgnIntf.cpp）")
consts = read("native/KAG/src/system/GalBoxConsts.tjs")
layer_shim2 = read("native/KAG/src/system/GalBoxLayer.tjs")
win_shim2 = read("native/KAG/src/system/GalBoxWindow.tjs")

ok("ScriptMgnIntf.cpp" in consts and "TVPInitTJSScript" in consts,
   "常量表注明来源：官方 ScriptMgnIntf.cpp 的 TVPInitTJSScript")
# 官方那批常量**不是** C++ 宏，而是一段 TJS 初始化脚本 —— 逐字照抄才不会搞错值。
ok(consts.count("=") > 300, "常量表规模与官方相当（300+ 个定义）")
for pair in ("VK_SPACE=0x20", "crArrow=-2", "clNone=0x1fffffff",
             "clAdapt=0x01ffffff", "ststStayDest=1", "ststStaySrc=2",
             "VK_PAD1=0x1C0", "mbLeft=0", "dfBoth=0"):
    ok(pair in consts.replace(" ", ""), "官方常量值逐字保留: " + pair)
# M2e-2 自造过这批常量，还把 ststStaySrc/ststStayDest 弄反了 ——
# 现在只能留一份，否则 TJS2 的 const 与 global 成员同名会冲突。
for dup in ("global.clNone", "global.clAdapt", "global.sttLeft",
            "global.ststStaySrc", "global.crArrow"):
    ok(dup not in layer_shim2 and dup not in win_shim2,
       "骨架里不再自造官方常量（避免与 const 重复定义）: " + dup)
# 常量表必须最先装载
_shim_s = bridge_cpp.index("static const char *kShims[]")
_shim_order = bridge_cpp[_shim_s:_shim_s + 400]
ok(_shim_order.index("GalBoxConsts.tjs") < _shim_order.index("GalBoxLayer.tjs")
   and _shim_order.index("GalBoxLayer.tjs") < _shim_order.index("GalBoxWindow.tjs"),
   "官方常量表最先装载（const 与 global 同名会冲突）")
ok("M2E4_OFFICIAL_CONSTS" in bridge_cpp and "M2E4_OFFICIAL_CONSTS" in wf,
   "官方常量表层有固定标记，且 CI 核验它真的进 app 二进制")
# 注意：静态测试**只能**读仓库内的文件。run #43 就是因为这里读了
# ../push_via_api.py（本地有、CI 的 checkout 里没有）而红的 —— 本地绿、CI 红。
ok("GalBoxConsts.tjs" in test_sh,
   "构建脚本把官方常量表装进夹具")
ok("GalBoxConsts.tjs" in read("native/KAG/README.md"),
   "KAG README 登记了官方常量表（来源与两处自造错值）")


print("\n[17] Milestone 2e-5 — 夹具换成官方 Config.tjs + 同进程状态泄漏探针")
m2h_test = read("native/Host/test_m2h.cpp")
cfg_new = raw("native/KAG/src/original/system/Config.~new")

# KAGWindow_config 必须真的在官方 Config.~new 里（这是本轮缺口的根因）
try:
    cfg_text = cfg_new.decode("cp932")
except UnicodeDecodeError:
    cfg_text = cfg_new.decode("shift_jis", errors="replace")
ok("function KAGWindow_config()" in cfg_text,
   "官方 Config.~new 里定义了 KAGWindow_config（run #45 缺口的根因）")
ok(";global.config_version" in cfg_text,
   "官方 Config.~new 的版本行是被注释掉的模板写法")

# 构建脚本：官方原文 + 前置 ASCII 版本号，且必须校验拼接结果
ok("Config.~new" in test_sh and "global.config_version" in test_sh,
   "构建脚本用官方 Config.~new 拼出 Config.tjs")
ok("function KAGWindow_config" in test_sh,
   "构建脚本校验夹具 Config.tjs 里确实有 KAGWindow_config")
ok("m2c_probe_config.tjs" in test_sh,
   "构建脚本备了探针版 Config.tjs 作负向对照")
ok("M2H_RESULT=OK" in test_sh and "test_m2h.cpp" in test_sh,
   "CI 运行 M2e-5 自测")

# 自测三场景 + 负向对照
ok("the official Config.tjs did not take effect" in m2h_test,
   "自测断言换了官方 Config.tjs 后 KAGWindow_config 缺口消失")
ok("negative control did not reproduce the KAGWindow_config gap" in m2h_test,
   "自测有负向对照（探针版 Config 必须复现该缺口）")
ok("scripts < 15" in m2h_test,
   "自测断言加载脚本数不比 M2e-4 的 15 个倒退")
ok("LEAK verdict" in m2h_test and "LEAK REPRODUCED" in m2h_test,
   "自测带「同进程第二次执行」的诊断探针（状态泄漏定位用）")
ok("LEAK2" in m2h_test and "warmup" in m2h_test,
   "自测带「先跑一个抛异常的脚本」的变体（区分异常污染与多引擎问题）")
# 每个场景必须独立进程 —— 否则前一个场景的污染会让负向对照假红。
ok("M2H_CASE in A B LEAK LEAK2" in test_sh or
   "for M2H_CASE in A B LEAK LEAK2" in test_sh,
   "构建脚本把四个场景拆成独立进程跑（避免场景间污染）")

print("\n========================================")
print("原生引擎静态+闭环校验: %d PASS / %d FAIL" % (P, F))
sys.exit(0 if F == 0 else 1)
