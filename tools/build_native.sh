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

# Refresh the bundled demo package from the verified XP3 fixture.
python3 tools/xp3_fixture.py >/dev/null
cp dist/fixtures/demo-game.xp3 ios-native/GalBoxNative/demo-game.xp3
echo "已同步内置演示包 demo-game.xp3"

BUILD="$ROOT/build-native"
ARCHIVE="$BUILD/GalBoxNative.xcarchive"
rm -rf "$BUILD"
mkdir -p "$BUILD"

# Milestone 1b: 若 tools/build_tjs2_ios.sh 已生成 iPhoneOS arm64 版 TJS2 静态库，
# 用 -force_load 把它完整链接进 app，验证 iOS 链接器能解析全部 TJS2 符号。
# 库不存在时降级为原配置出包 —— 本地直接跑本脚本不应被 TJS2 阻塞。
TJS2_IOS_LIB="$ROOT/native/TJS2/build/libtjs2_ios.a"
OTHER_LDFLAGS_VALUE="-lz"
if [ -f "$TJS2_IOS_LIB" ]; then
  OTHER_LDFLAGS_VALUE="-lz -liconv -force_load $TJS2_IOS_LIB"
  echo "已启用 TJS2 iOS arm64 链接 (M1b): $TJS2_IOS_LIB"
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
