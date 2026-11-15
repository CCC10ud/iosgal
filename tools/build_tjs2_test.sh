#!/usr/bin/env bash
# build_tjs2_test.sh — 编译官方 TJS2 核心为静态库并运行 eval 自测（Milestone 0）
#                      + 编译 GalBox 宿主层并真跑 XP3 文件读取自测（Milestone 1a）
#                      + 真跑 app 侧纯 C 边界 HostBridge（Milestone 1c）
#                      + 真跑官方 KAG3 系统脚本经 Scripts.execStorage（Milestone 2a）
#
# 由 CI 在 xcodebuild 之前调用。设计原则：
#   - 源码尚未 vendoring 时直接跳过（exit 0），绝不破坏现有出包链路。
#   - 源码就位后：逐个 clang++ 编译 native/TJS2/src/*.cpp -> libtjs2.a，
#     再编译 test_m0.cpp 链接，运行 eval("1+1") 自测。任何错误都让 CI 失败，
#     把真实移植问题暴露出来。
#   - 使用官方原生开关 TJS_NO_REGEXP 规避 boost regex 依赖（M3 再补真正则）。
#   - ENABLE_DEBUGGER 未定义 => Windows Debugger 代码自动跳过。
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/native/TJS2/src"
STUB="$ROOT/native/TJS2/stub"
COMPAT="$ROOT/native/TJS2/tjs2_compat.h"
OUT="$ROOT/native/TJS2/build"

if [ ! -d "$SRC" ] || [ -z "$(ls -A "$SRC" 2>/dev/null | grep -E '\.(cpp|c)$')" ]; then
  echo "[tjs2] 源码未 vendoring，跳过 M0 编译 (exit 0)"
  exit 0
fi

mkdir -p "$OUT"
echo "[tjs2] M0: 编译官方 TJS2 核心 (TJS_NO_REGEXP) -> $OUT/libtjs2.a"
ERR=0
OBJS=()
for f in "$SRC"/*.cpp; do
  [ -e "$f" ] || continue
  b="$(basename "$f")"
  # tjsRegExp.cpp 是 boost regex 唯一入口；官方 TJS_NO_REGEXP 模式下不需要它。
  if [ "$b" = "tjsRegExp.cpp" ]; then
    echo "[tjs2]   skip  $b (boost regex, disabled via TJS_NO_REGEXP)"
    continue
  fi
  o="$OUT/${b%.cpp}.o"
  if ! clang++ -std=c++14 -O1 -fexceptions -DTJS_NO_REGEXP -DTJS2_POSIX \
        -I"$STUB" -I"$SRC" -include "$COMPAT" \
        -c "$f" -o "$o" 2>"$o.log"; then
    echo "[tjs2] 编译失败: $b"; cat "$o.log"; ERR=1
  else
    OBJS+=("$o")
  fi
done
[ $ERR -ne 0 ] && { echo "[tjs2] M0 核心编译存在错误"; exit 1; }

ar rcs "$OUT/libtjs2.a" "${OBJS[@]}"
echo "[tjs2] libtjs2.a 构建成功 ($(wc -c < "$OUT/libtjs2.a" | tr -d ' ') bytes, ${#OBJS[@]} objs)"

# eval 自测
if [ -f "$ROOT/native/TJS2/test_m0.cpp" ]; then
  echo "[tjs2] 编译并运行 eval 自测"
  if clang++ -std=c++14 -O1 -fexceptions -DTJS_NO_REGEXP -DTJS2_POSIX \
        -I"$STUB" -I"$SRC" -include "$COMPAT" \
        "$ROOT/native/TJS2/test_m0.cpp" "$OUT/libtjs2.a" -lpthread -o "$OUT/tjs2test" 2>"$OUT/link.log"; then
    OUTPUT=$("$OUT/tjs2test" 2>&1)
    echo "$OUTPUT" | sed 's/^/[tjs2]   /'
    if echo "$OUTPUT" | grep -q "M0_RESULT=2"; then
      echo "[tjs2] M0 自测通过 (eval '1+1' == 2)"
    else
      echo "[tjs2] M0 自测结果不符预期"; exit 1
    fi
  else
    echo "[tjs2] M0 自测链接失败:"; cat "$OUT/link.log"; exit 1
  fi
fi

# ---- Milestone 1a: 宿主流工厂桥到 XP3Archive，在 runner 上真跑 ----------
HOST="$ROOT/native/Host"
if [ -f "$HOST/HostStreams.cpp" ] && [ -f "$ROOT/native/TJS2/test_m1.cpp" ]; then
  echo "[tjs2] M1: 编译 GalBox 宿主流实现"
  if ! clang++ -std=c++14 -O1 -fexceptions -DTJS_NO_REGEXP -DTJS2_POSIX \
        -I"$STUB" -I"$SRC" -I"$HOST" -I"$ROOT/native/Engine" -include "$COMPAT" \
        -c "$HOST/HostStreams.cpp" -o "$OUT/HostStreams.o" 2>"$OUT/host_m1.log"; then
    echo "[tjs2] M1 宿主编译失败:"; cat "$OUT/host_m1.log"; exit 1
  fi
  echo "[tjs2] HostStreams.o OK"

  # XP3Archive 是 GalBox 自己的代码，独立编译（不注入 TJS2 compat 头）。
  if ! clang++ -std=c++14 -O1 -I"$ROOT/native/Engine" \
        -c "$ROOT/native/Engine/XP3Archive.cpp" -o "$OUT/XP3Archive.o" 2>"$OUT/xp3_m1.log"; then
    echo "[tjs2] M1 XP3Archive 编译失败:"; cat "$OUT/xp3_m1.log"; exit 1
  fi

  echo "[tjs2] M1: 编译并运行宿主自测（读 XP3 内文件 / Array.load）"
  if clang++ -std=c++14 -O1 -fexceptions -DTJS_NO_REGEXP -DTJS2_POSIX \
        -I"$STUB" -I"$SRC" -I"$HOST" -I"$ROOT/native/Engine" -include "$COMPAT" \
        "$ROOT/native/TJS2/test_m1.cpp" "$OUT/HostStreams.o" "$OUT/XP3Archive.o" \
        "$OUT/libtjs2.a" -lz -liconv -lpthread -o "$OUT/tjs2test_m1" 2>"$OUT/link_m1.log"; then
    OUTPUT_M1=$("$OUT/tjs2test_m1" "$ROOT/dist/fixtures/demo-game.xp3" "$OUT" 2>&1)
    echo "$OUTPUT_M1" | sed 's/^/[tjs2]   /'
    if echo "$OUTPUT_M1" | grep -q "M1_RESULT=OK"; then
      echo "[tjs2] M1 自测通过（官方 TJS2 经宿主读到 XP3 内文件）"
    else
      echo "[tjs2] M1 自测未通过"; exit 1
    fi
  else
    echo "[tjs2] M1 自测链接失败:"; cat "$OUT/link_m1.log"; exit 1
  fi
fi

# ---- Milestone 1c: app 侧纯 C 边界在 macOS host 上真跑一遍 ---------------
# CI 不能运行 iOS app，所以 HostBridge.cpp 的逻辑必须先在 host 上证明正确；
# iOS 侧只负责「用同一套 flags 编译 + 链接进 app」。
if [ -f "$HOST/HostBridge.cpp" ] && [ -f "$HOST/test_m1c.cpp" ]; then
  echo "[tjs2] M1c: 编译 app 侧适配层 HostBridge.cpp"
  if ! clang++ -std=c++14 -O1 -fexceptions -DTJS_NO_REGEXP -DTJS2_POSIX \
        -I"$STUB" -I"$SRC" -I"$HOST" -I"$ROOT/native/Engine" -include "$COMPAT" \
        -c "$HOST/HostBridge.cpp" -o "$OUT/HostBridge.o" 2>"$OUT/host_m1c.log"; then
    echo "[tjs2] M1c HostBridge 编译失败:"; cat "$OUT/host_m1c.log"; exit 1
  fi
  echo "[tjs2] HostBridge.o OK"

  echo "[tjs2] M1c: 编译并运行冒烟自测（eval / Array.load / Array.save / 卸载）"
  if clang++ -std=c++14 -O1 -fexceptions -DTJS_NO_REGEXP -DTJS2_POSIX \
        -I"$STUB" -I"$SRC" -I"$HOST" -I"$ROOT/native/Engine" -include "$COMPAT" \
        "$HOST/test_m1c.cpp" "$OUT/HostBridge.o" "$OUT/HostStreams.o" \
        "$OUT/XP3Archive.o" "$OUT/libtjs2.a" \
        -lz -liconv -lpthread -o "$OUT/tjs2test_m1c" 2>"$OUT/link_m1c.log"; then
    OUTPUT_M1C=$("$OUT/tjs2test_m1c" "$ROOT/dist/fixtures/demo-game.xp3" "$OUT" 2>&1)
    echo "$OUTPUT_M1C" | sed 's/^/[tjs2]   /'
    if echo "$OUTPUT_M1C" | grep -q "M1C_RESULT=OK"; then
      echo "[tjs2] M1c 自测通过（app 侧 C 边界真跑 TJS2 + 宿主读写）"
    else
      echo "[tjs2] M1c 自测未通过"; exit 1
    fi
  else
    echo "[tjs2] M1c 自测链接失败:"; cat "$OUT/link_m1c.log"; exit 1
  fi
fi

# ---- Milestone 2a: 官方 KAG3 系统脚本经 Scripts.execStorage 真跑 ----------
# 用「目录模式归档」造一个最小游戏：startup.tjs 调 Scripts.execStorage，
# 被加载的 system/Utils.tjs 是官方 KAG3 原文（日文注释 / property / <->）。
KAG="$ROOT/native/KAG"
if [ -f "$HOST/HostBridge.cpp" ] && [ -f "$HOST/test_m2a.cpp" ] \
   && [ -f "$KAG/src/system/Utils.tjs" ] && [ -f "$KAG/src/m2a_probe_startup.tjs" ]; then
  M2A_GAME="$OUT/m2a_game"
  mkdir -p "$M2A_GAME/system"
  cp "$KAG/src/m2a_probe_startup.tjs" "$M2A_GAME/startup.tjs"
  cp "$KAG/src/system/Utils.tjs" "$M2A_GAME/system/Utils.tjs"

  echo "[tjs2] M2a: 编译并运行官方 KAG3 脚本自测（Scripts.execStorage）"
  if clang++ -std=c++14 -O1 -fexceptions -DTJS_NO_REGEXP -DTJS2_POSIX \
        -I"$STUB" -I"$SRC" -I"$HOST" -I"$ROOT/native/Engine" -include "$COMPAT" \
        "$HOST/test_m2a.cpp" "$OUT/HostBridge.o" "$OUT/HostStreams.o" \
        "$OUT/XP3Archive.o" "$OUT/libtjs2.a" \
        -lz -liconv -lpthread -o "$OUT/tjs2test_m2a" 2>"$OUT/link_m2a.log"; then
    OUTPUT_M2A=$("$OUT/tjs2test_m2a" "$M2A_GAME" "$OUT" 2>&1)
    echo "$OUTPUT_M2A" | sed 's/^/[tjs2]   /'
    if echo "$OUTPUT_M2A" | grep -q "M2A_RESULT=OK"; then
      echo "[tjs2] M2a 自测通过（官方 KAG3 Utils.tjs 经 TJS2 真跑，intrandom(5,5)==5）"
    else
      echo "[tjs2] M2a 自测未通过"; exit 1
    fi
  else
    echo "[tjs2] M2a 自测链接失败:"; cat "$OUT/link_m2a.log"; exit 1
  fi
fi
exit 0
