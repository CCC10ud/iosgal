#!/usr/bin/env bash
# build_tjs2_ios.sh — Milestone 1b：为 iOS arm64 编译官方 TJS2 核心 + GalBox 宿主层。
#
# 产物：
#   native/TJS2/build/libtjs2_ios.a        （iphoneos arm64，供 app 链接）
#   native/TJS2/build/HostStreams_ios.o   （宿主层，app 已有 XP3Archive.o，勿重复）
#   native/TJS2/build/HostBridge_ios.o    （M1c：app 侧纯 C 适配层）
#
# 与 M0 的区别：M0 用 macOS host clang 编译并运行自测（证明逻辑正确）；
# 本脚本用 iphoneos SDK 交叉编译（证明 Apple 移动工具链能编、能链）。
# M2d 起**不再**定义 TJS_NO_REGEXP：正则层已由 GalBox 用 std::regex 重写
# （native/TJS2/src/tjsRegExp.{h,cpp}，见 PATCHES.md P7），tjsRegExp.cpp 正常编译。
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/native/TJS2/src"
STUB="$ROOT/native/TJS2/stub"
COMPAT="$ROOT/native/TJS2/tjs2_compat.h"
HOST="$ROOT/native/Host"
ENGINE="$ROOT/native/Engine"
OUT="$ROOT/native/TJS2/build"
IOSOUT="$OUT/ios"

if [ ! -d "$SRC" ] || [ -z "$(ls -A "$SRC" 2>/dev/null | grep -E '\.(cpp|c)$')" ]; then
  echo "[tjs2-ios] 源码未 vendoring，跳过 (exit 0)"
  exit 0
fi
if ! xcrun --sdk iphoneos --show-sdk-path >/dev/null 2>&1; then
  echo "[tjs2-ios] iPhoneOS SDK 不可用，跳过 (exit 0)"
  exit 0
fi

SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
mkdir -p "$IOSOUT"
echo "[tjs2-ios] iPhoneOS SDK: $SDK"
echo "[tjs2-ios] 编译 官方 TJS2 核心 -> arm64"

CXXFLAGS=(-std=c++14 -O1 -fexceptions
          -DTJS2_POSIX
          -arch arm64 -miphoneos-version-min=15.0 -isysroot "$SDK"
          -I"$STUB" -I"$SRC" -I"$HOST" -I"$ENGINE"
          -include "$COMPAT")

ERR=0
OBJS=()
for f in "$SRC"/*.cpp; do
  [ -e "$f" ] || continue
  b="$(basename "$f")"
  o="$IOSOUT/${b%.cpp}.o"
  if ! xcrun --sdk iphoneos clang++ "${CXXFLAGS[@]}" -c "$f" -o "$o" 2>"$o.log"; then
    echo "[tjs2-ios] 编译失败: $b"; cat "$o.log"; ERR=1
  else
    OBJS+=("$o")
  fi
done
[ $ERR -ne 0 ] && { echo "[tjs2-ios] iOS 核心编译存在错误"; exit 1; }

ar rcs "$OUT/libtjs2_ios.a" "${OBJS[@]}"
echo "[tjs2-ios] libtjs2_ios.a: $(wc -c < "$OUT/libtjs2_ios.a" | tr -d ' ') bytes, ${#OBJS[@]} objs"

if ! xcrun --sdk iphoneos clang++ "${CXXFLAGS[@]}" \
      -c "$HOST/HostStreams.cpp" -o "$OUT/HostStreams_ios.o" 2>"$OUT/host_ios.log"; then
  echo "[tjs2-ios] HostStreams.cpp iOS 编译失败:"; cat "$OUT/host_ios.log"; exit 1
fi
echo "[tjs2-ios] HostStreams_ios.o OK"

# Milestone 1c：app 侧纯 C 适配层，与 HostStreams.cpp 用完全相同的 flags，
# 避免 ObjC++ target 直接 include 官方 tjs.h 时编译开关不一致。
if ! xcrun --sdk iphoneos clang++ "${CXXFLAGS[@]}" \
      -c "$HOST/HostBridge.cpp" -o "$OUT/HostBridge_ios.o" 2>"$OUT/bridge_ios.log"; then
  echo "[tjs2-ios] HostBridge.cpp iOS 编译失败:"; cat "$OUT/bridge_ios.log"; exit 1
fi
echo "[tjs2-ios] HostBridge_ios.o OK (M1c)"

# 架构硬校验：必须是 Mach-O arm64，杜绝"编出来却是 x86_64"的假通过。
ARCHINFO="$(file "${OBJS[0]}")"
echo "[tjs2-ios] arch check: $ARCHINFO"
if ! echo "$ARCHINFO" | grep -qi "arm64"; then
  echo "[tjs2-ios] 目标文件不是 arm64"; exit 1
fi
if command -v lipo >/dev/null 2>&1; then
  lipo -info "$OUT/libtjs2_ios.a" || true
fi
echo "M1B_RESULT=OK"
exit 0
