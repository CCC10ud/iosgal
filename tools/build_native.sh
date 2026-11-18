#!/usr/bin/env bash
# Build the *native* GalBox iOS app (no WKWebView) into side-loadable ipa.
# Requires macOS + Xcode. Target device: iOS 27.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "ERROR: 原生 iOS App 只能在 macOS 上编译（需要 Xcode + ARM64 工具链）。" >&2
  echo "       本机可跑 python tools/verify_native.py 验证 C++ 内核与 XP3 夹具。" >&2
  exit 2
fi
command -v xcodebuild >/dev/null || { echo "ERROR: 找不到 xcodebuild"; exit 2; }

SDK="$(xcrun --sdk iphoneos --show-sdk-version 2>/dev/null || echo unknown)"
echo "Xcode: $(xcodebuild -version 2>/dev/null | tr '\n' ' ')"
echo "iPhoneOS SDK: $SDK"
echo "说明: 部署目标 15.0，用任意较新 SDK 编出的 arm64 包都能在 iOS 27 上运行；不要求 SDK 正好是 27。"
echo "工程: ios-native/GalBoxNative.xcodeproj   Bundle ID: com.galbox.native"

# 内置游戏包：优先用自测夹具打出来的真 KAG 包（M2e-12b）。
# 此前这里固定用 xp3_fixture 的 demo-game.xp3，其 startup.tjs 只有一行注释 ——
# 于是 M2e 各轮的骨架只活在自测夹具里，app 里根本没有系统脚本，所谓「出包」
# 什么也没验证。现在内置包由 build_tjs2_test.sh 用同一份夹具现场打包，
# 自测与设备从同一个源出，不会再漂。
if [ -f dist/fixtures/galbox-kag.xp3 ]; then
  cp dist/fixtures/galbox-kag.xp3 ios-native/GalBoxNative/demo-game.xp3
  KAG_BYTES="$(wc -c < ios-native/GalBoxNative/demo-game.xp3 | tr -d ' ')"
  echo "已同步内置 KAG 包（galbox-kag.xp3, ${KAG_BYTES} B）"
  if [ "$KAG_BYTES" -lt 100000 ]; then
    echo "ERROR: 内置 KAG 包只有 ${KAG_BYTES} B，官方系统脚本没打进去。" >&2
    exit 1
  fi
  # 先落成清单文件再 grep：set -o pipefail 下 grep -q 提前退出会让 python
  # 收到 SIGPIPE，整条管道被判定失败（假红）。
  KAG_LIST="$(mktemp)"
  python3 tools/xp3_fixture.py --list ios-native/GalBoxNative/demo-game.xp3 > "$KAG_LIST"
  if ! grep -q "^system/Initialize.tjs" "$KAG_LIST"; then
    echo "ERROR: 内置 KAG 包里没有 system/Initialize.tjs" >&2
    rm -f "$KAG_LIST"; exit 1
  fi
  if ! grep -q "^system/GalBoxAsyncTrigger.tjs" "$KAG_LIST"; then
    echo "ERROR: 内置 KAG 包里没有最新骨架 GalBoxAsyncTrigger.tjs" >&2
    rm -f "$KAG_LIST"; exit 1
  fi
  if ! grep -q "^system/GalBoxSound.tjs" "$KAG_LIST"; then
    echo "ERROR: 内置 KAG 包里没有最新骨架 GalBoxSound.tjs (M2e-25)" >&2
    rm -f "$KAG_LIST"; exit 1
  fi
  echo "内置包条目数: $(wc -l < "$KAG_LIST" | tr -d ' ')"
  rm -f "$KAG_LIST"
else
  python3 tools/xp3_fixture.py >/dev/null
  cp dist/fixtures/demo-game.xp3 ios-native/GalBoxNative/demo-game.xp3
  echo "::warning::没有 galbox-kag.xp3（build_tjs2_test.sh 未跑？），内置包退化为注释版 demo-game.xp3"
fi

BUILD="$ROOT/build-native"
ARCHIVE="$BUILD/GalBoxNative.xcarchive"
rm -rf "$BUILD"
mkdir -p "$BUILD"

# Milestone 1b/1c: 若 tools/build_tjs2_ios.sh 已生成 iPhoneOS arm64 产物，
# 用 -force_load 把 TJS2 核心完整链接进 app，并追加宿主层两个 .o。
# XP3Archive.cpp 由 app target 自己编译，绝不在 .o 里重复打包。
# 只有三者齐备才定义 GALBOX_HAS_TJS2，让 GBEngineBridge.mm 生成真实调用点；
# 否则降级为原配置出包 —— 本地直接跑本脚本不应被 TJS2 阻塞。
TJS2_IOS_LIB="$ROOT/native/TJS2/build/libtjs2_ios.a"
TJS2_HOST_OBJ="$ROOT/native/TJS2/build/HostStreams_ios.o"
TJS2_BRIDGE_OBJ="$ROOT/native/TJS2/build/HostBridge_ios.o"
OTHER_LDFLAGS_VALUE="-lz"
TJS2_DEFS="GALBOX_M1C_DISABLED=1"
if [ -f "$TJS2_IOS_LIB" ] && [ -f "$TJS2_HOST_OBJ" ] && [ -f "$TJS2_BRIDGE_OBJ" ]; then
  OTHER_LDFLAGS_VALUE="-lz -liconv -force_load $TJS2_IOS_LIB $TJS2_HOST_OBJ $TJS2_BRIDGE_OBJ"
  TJS2_DEFS="GALBOX_HAS_TJS2=1"
  echo "已启用 TJS2 iOS arm64 + 宿主层 (M1c): lib + HostStreams + HostBridge"
elif [ -f "$TJS2_IOS_LIB" ]; then
  OTHER_LDFLAGS_VALUE="-lz -liconv -force_load $TJS2_IOS_LIB"
  echo "提示: 缺 M1c 宿主 .o，按 M1b 配置只链核心（跑 tools/build_tjs2_ios.sh 可升级）"
else
  echo "提示: 未找到 libtjs2_ios.a，按原配置出包（跑 tools/build_tjs2_ios.sh 可启用）"
fi

echo "--- xcodebuild archive (无 Apple 开发者签名) ---"
xcodebuild \
  -project ios-native/GalBoxNative.xcodeproj \
  -scheme GalBoxNative \
  -configuration Release \
  -destination 'generic/platform=iOS' \
  -archivePath "$ARCHIVE" \
  CODE_SIGNING_ALLOWED=NO \
  CODE_SIGNING_REQUIRED=NO \
  CODE_SIGN_IDENTITY="" \
  CODE_SIGN_STYLE=Manual \
  DEVELOPMENT_TEAM="" \
  PROVISIONING_PROFILE_SPECIFIER="" \
  IPHONEOS_DEPLOYMENT_TARGET=15.0 \
  OTHER_LDFLAGS="$OTHER_LDFLAGS_VALUE" \
  GCC_PREPROCESSOR_DEFINITIONS="$TJS2_DEFS" \
  clean archive 2>&1 | tee "$BUILD/xcodebuild.log"

APP="$ARCHIVE/Products/Applications/GalBoxNative.app"
[[ -d "$APP" ]] || { echo "ERROR: archive 里没有 GalBoxNative.app"; exit 1; }

# A missing Mach-O is exactly the mistake the old shell-only ipa made.
python3 - "$APP" <<'PY'
import os, sys, plistlib
app = sys.argv[1]
p = plistlib.load(open(os.path.join(app, "Info.plist"), "rb"))
exe = p.get("CFBundleExecutable", "GalBoxNative")
path = os.path.join(app, exe)
assert os.path.isfile(path), "缺少可执行文件 " + path
with open(path, "rb") as f:
    head = f.read(4)
assert head in (b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe"), "不是 Mach-O: %r" % head
print("Mach-O 校验通过: %s (%d bytes)" % (exe, os.path.getsize(path)))
assert os.path.isfile(os.path.join(app, "PrivacyInfo.xcprivacy")), "缺 PrivacyInfo.xcprivacy"
assert os.path.isfile(os.path.join(app, "demo-game.xp3")), "缺内置演示包"
print("Bundle 资源校验通过")
PY

mkdir -p "$BUILD/Payload" "$ROOT/dist"
cp -R "$APP" "$BUILD/Payload/"
(cd "$BUILD" && zip -qr "$ROOT/dist/GalBoxNative-unsigned.ipa" Payload)
echo "→ dist/GalBoxNative-unsigned.ipa (裸包)"

# ad-hoc pseudo signature: no Apple ID, no certificate, no provisioning profile.
codesign --force --deep --sign - --timestamp=none "$BUILD/Payload/GalBoxNative.app"
(cd "$BUILD" && zip -qr "$ROOT/dist/GalBoxNative-adhoc.ipa" Payload)
echo "→ dist/GalBoxNative-adhoc.ipa (ad-hoc 伪签，供重签工具使用)"

# The old WebView shell once shipped a zip with no Mach-O. Never let that pass.
python3 tools/verify_ipa.py \
  "$ROOT/dist/GalBoxNative-unsigned.ipa" \
  "$ROOT/dist/GalBoxNative-adhoc.ipa"

if [[ -n "${GALBOX_TEAM_ID:-}" ]]; then
  echo "--- 使用 Team ID 导出开发者签名包 ---"
  plutil -replace teamID -string "$GALBOX_TEAM_ID" ios-native/ExportOptions.plist
  xcodebuild -exportArchive -archivePath "$ARCHIVE" \
    -exportPath "$BUILD/export" \
    -exportOptionsPlist ios-native/ExportOptions.plist
  cp "$BUILD/export/GalBoxNative.ipa" "$ROOT/dist/GalBoxNative-signed.ipa"
  echo "→ dist/GalBoxNative-signed.ipa (1 年有效)"
fi

echo
echo "iOS 27 侧载：Sideloadly / AltStore / SideStore 用免费 Apple ID 重签"
echo "GalBoxNative-adhoc.ipa（7 天续签），或付费开发者账号一年签名。"
echo "注意：iOS 27 上 TrollStore / .tipa 不可用（CoreTrust 漏洞已在 17.0.1 修复）。"
echo "构建日志: build-native/xcodebuild.log（失败时可由 Actions 上传）"
