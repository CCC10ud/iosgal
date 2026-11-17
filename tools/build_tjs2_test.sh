#!/usr/bin/env bash
# build_tjs2_test.sh — 编译官方 TJS2 核心为静态库并运行 eval 自测（Milestone 0）
#                      + 编译 GalBox 宿主层并真跑 XP3 文件读取自测（Milestone 1a）
#                      + 真跑 app 侧纯 C 边界 HostBridge（Milestone 1c）
#                      + 真跑官方 KAG3 系统脚本经 Scripts.execStorage（Milestone 2a）
#                      + 真跑 Storages 自动路径 + 官方原始 Shift-JIS 字节（Milestone 2b）
#                      + 真跑官方 Initialize.tjs 原文 + System/Debug 原生类（Milestone 2c）
#                      + 真跑正则（RegExp 原生类，std::regex 背书）（Milestone 2d）
#                      + 全量 vendoring 官方 system/*.tjs 并推进启动链（Milestone 2e-1）
#                      + GalBox 的 Layer 骨架能被官方脚本类继承（Milestone 2e-2）
#                      + GalBox 的 Window 骨架能被官方脚本类继承（Milestone 2e-3）
#                      + 夹具换成官方 Config.tjs（Milestone 2e-5）
#
# 由 CI 在 xcodebuild 之前调用。设计原则：
#   - 源码尚未 vendoring 时直接跳过（exit 0），绝不破坏现有出包链路。
#   - 源码就位后：逐个 clang++ 编译 native/TJS2/src/*.cpp -> libtjs2.a，
#     再编译 test_m0.cpp 链接，运行 eval("1+1") 自测。任何错误都让 CI 失败，
#     把真实移植问题暴露出来。
#   - 正则：M2d 起**不再**定义官方 TJS_NO_REGEXP。tjsRegExp.cpp 已由 GalBox 用
#     std::regex 重写（见 native/TJS2/PATCHES.md P7），无需再排除该文件。
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
# iconv 链接参数按平台探测：macOS / iOS SDK 有独立的 libiconv；**glibc 把 iconv
# 内建在 libc 里，没有 libiconv.so** —— Linux 上写死 -liconv 会链接失败。
# （本脚本现在也会在 ubuntu runner 上跑，见 .github/workflows 里的自测 job。）
if echo 'int main(){return 0;}' | clang++ -x c++ - -liconv -o /dev/null 2>/dev/null; then
  ICONV_LIB="-liconv"
else
  ICONV_LIB=""
  echo "[tjs2] 该平台没有独立 libiconv（glibc 内建），链接时省略 -liconv"
fi

# 通用崩溃诊断（run #52）：每个自测运行后调用，打印退出码；段错误(>=128)时
# 用 gdb 打 backtrace。诊断写 stdout 但不进 OUTPUT_* 变量（调用点在命令替换
# 之后才调用本函数），因此不干扰 M*_RESULT 判据。macOS runner 无 gdb 时只打印退出码。
diag_crash() {
  local label="$1" rc="$2" bin="$3"; shift 3
  echo "[tjs2] $label exit code: $rc"
  if [ "$rc" -ge 128 ] && command -v gdb >/dev/null 2>&1; then
    echo "[tjs2] $label crash backtrace (gdb):"
    gdb -q -batch -ex run -ex bt --args "$bin" "$@" 2>&1 \
      | grep -vE '^\[New Thread|^\[Thread' \
      | sed 's/^/[tjs2][gdb] /' | head -40
  fi
}

echo "[tjs2] M0: 编译官方 TJS2 核心（含 GalBox 正则层）-> $OUT/libtjs2.a"
ERR=0
OBJS=()
for f in "$SRC"/*.cpp; do
  [ -e "$f" ] || continue
  b="$(basename "$f")"
  o="$OUT/${b%.cpp}.o"
  if ! clang++ -std=c++14 -O1 -g -fexceptions -DTJS2_POSIX \
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
  if clang++ -std=c++14 -O1 -g -fexceptions -DTJS2_POSIX \
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
  if ! clang++ -std=c++14 -O1 -g -fexceptions -DTJS2_POSIX \
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
  if clang++ -std=c++14 -O1 -g -fexceptions -DTJS2_POSIX \
        -I"$STUB" -I"$SRC" -I"$HOST" -I"$ROOT/native/Engine" -include "$COMPAT" \
        "$ROOT/native/TJS2/test_m1.cpp" "$OUT/HostStreams.o" "$OUT/XP3Archive.o" \
        "$OUT/libtjs2.a" -lz $ICONV_LIB -lpthread -o "$OUT/tjs2test_m1" 2>"$OUT/link_m1.log"; then
    OUTPUT_M1=$("$OUT/tjs2test_m1" "$ROOT/dist/fixtures/demo-game.xp3" "$OUT" 2>&1)
    diag_crash "M1" "$?" "$OUT/tjs2test_m1" "$ROOT/dist/fixtures/demo-game.xp3" "$OUT"
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
  if ! clang++ -std=c++14 -O1 -g -fexceptions -DTJS2_POSIX \
        -I"$STUB" -I"$SRC" -I"$HOST" -I"$ROOT/native/Engine" -include "$COMPAT" \
        -c "$HOST/HostBridge.cpp" -o "$OUT/HostBridge.o" 2>"$OUT/host_m1c.log"; then
    echo "[tjs2] M1c HostBridge 编译失败:"; cat "$OUT/host_m1c.log"; exit 1
  fi
  echo "[tjs2] HostBridge.o OK"

  echo "[tjs2] M1c: 编译并运行冒烟自测（eval / Array.load / Array.save / 卸载）"
  if clang++ -std=c++14 -O1 -g -fexceptions -DTJS2_POSIX \
        -I"$STUB" -I"$SRC" -I"$HOST" -I"$ROOT/native/Engine" -include "$COMPAT" \
        "$HOST/test_m1c.cpp" "$OUT/HostBridge.o" "$OUT/HostStreams.o" \
        "$OUT/XP3Archive.o" "$OUT/libtjs2.a" \
        -lz $ICONV_LIB -lpthread -o "$OUT/tjs2test_m1c" 2>"$OUT/link_m1c.log"; then
    OUTPUT_M1C=$("$OUT/tjs2test_m1c" "$ROOT/dist/fixtures/demo-game.xp3" "$OUT" 2>&1)
    diag_crash "M1c" "$?" "$OUT/tjs2test_m1c" "$ROOT/dist/fixtures/demo-game.xp3" "$OUT"
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
  if clang++ -std=c++14 -O1 -g -fexceptions -DTJS2_POSIX \
        -I"$STUB" -I"$SRC" -I"$HOST" -I"$ROOT/native/Engine" -include "$COMPAT" \
        "$HOST/test_m2a.cpp" "$OUT/HostBridge.o" "$OUT/HostStreams.o" \
        "$OUT/XP3Archive.o" "$OUT/libtjs2.a" \
        -lz $ICONV_LIB -lpthread -o "$OUT/tjs2test_m2a" 2>"$OUT/link_m2a.log"; then
    OUTPUT_M2A=$("$OUT/tjs2test_m2a" "$M2A_GAME" "$OUT" 2>&1)
    diag_crash "M2a" "$?" "$OUT/tjs2test_m2a" "$M2A_GAME" "$OUT"
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

# ---- Milestone 2b: Storages autoPath + 官方原始 Shift-JIS 字节 ------------
# M2a 用的是 GitHub contents API 转码后的 UTF-8 副本，SJIS 解码链从未真跑。
# 本轮把按 git blob sha 取的官方原始字节塞进目录归档，经 autoPath 短名加载。
if [ -f "$HOST/HostBridge.cpp" ] && [ -f "$HOST/test_m2b.cpp" ] \
   && [ -f "$KAG/src/original/system/Utils.tjs" ] \
   && [ -f "$KAG/src/m2b_probe_startup.tjs" ] \
   && [ -f "$KAG/src/m2b_bad_autopath.tjs" ]; then
  M2B_GAME="$OUT/m2b_game"
  mkdir -p "$M2B_GAME/system"
  cp "$KAG/src/m2b_probe_startup.tjs" "$M2B_GAME/startup.tjs"
  cp "$KAG/src/m2b_bad_autopath.tjs" "$M2B_GAME/bad_startup.tjs"
  cp "$KAG/src/original/system/Utils.tjs" "$M2B_GAME/system/Utils.tjs"

  # 硬校验：必须真的是官方原始 3658 B Shift-JIS blob，而不是 M2a 的
  # 3940 B UTF-8 转码副本。否则本轮等于什么都没验证。
  M2B_BYTES=$(wc -c < "$M2B_GAME/system/Utils.tjs" | tr -d ' ')
  if [ "$M2B_BYTES" != "3658" ]; then
    echo "[tjs2] M2b fixture 不是官方原始字节: ${M2B_BYTES}B (期望 3658B)"
    exit 1
  fi
  echo "[tjs2] M2b fixture: ${M2B_BYTES}B 官方原始 Shift-JIS blob"

  echo "[tjs2] M2b: 编译并运行 Storages autoPath / Shift-JIS 自测"
  if clang++ -std=c++14 -O1 -g -fexceptions -DTJS2_POSIX \
        -I"$STUB" -I"$SRC" -I"$HOST" -I"$ROOT/native/Engine" -include "$COMPAT" \
        "$HOST/test_m2b.cpp" "$OUT/HostBridge.o" "$OUT/HostStreams.o" \
        "$OUT/XP3Archive.o" "$OUT/libtjs2.a" \
        -lz $ICONV_LIB -lpthread -o "$OUT/tjs2test_m2b" 2>"$OUT/link_m2b.log"; then
    OUTPUT_M2B=$("$OUT/tjs2test_m2b" "$M2B_GAME" "$OUT" 2>&1)
    M2B_RC=$?
    echo "[tjs2] M2b exit code: $M2B_RC"
    # 诊断（run #49）：Linux 上第二次 ExecScript 段错误(139)，macOS 同码通过。
    # 崩溃时若有 gdb，打一份 backtrace 供定位进程级状态泄漏根因。
    if [ "$M2B_RC" -ge 128 ] && command -v gdb >/dev/null 2>&1; then
      echo "[tjs2] M2b crash backtrace (gdb):"
      gdb -q -batch -ex run -ex bt --args \
          "$OUT/tjs2test_m2b" "$M2B_GAME" "$OUT" 2>&1 \
          | grep -vE '^\[New Thread|^\[Thread' \
          | sed 's/^/[tjs2][gdb] /' | head -60
    fi
    echo "$OUTPUT_M2B" | sed 's/^/[tjs2]   /'
    if echo "$OUTPUT_M2B" | grep -q "M2B_RESULT=OK"; then
      echo "[tjs2] M2b 自测通过（Storages autoPath + 官方 SJIS Utils.tjs 真跑）"
    else
      echo "[tjs2] M2b 自测未通过"; exit 1
    fi
  else
    echo "[tjs2] M2b 自测链接失败:"; cat "$OUT/link_m2b.log"; exit 1
  fi
fi

# ---- Milestone 2c: 官方 Initialize.tjs 原文真跑（System / Debug） --------
# 前两轮跑的都是自写探针。本轮把官方 kag3/template/startup.tjs 与
# system/Initialize.tjs 原文（Shift-JIS 字节，一字不改）放进目录归档，
# 看它到底能执行到哪一句。两个场景：「没有 Config.tjs」与「有探针 Config」。
if [ -f "$HOST/HostBridge.cpp" ] && [ -f "$HOST/test_m2c.cpp" ] \
   && [ -f "$KAG/src/original/startup.tjs" ] \
   && [ -f "$KAG/src/original/system/Initialize.tjs" ] \
   && [ -f "$KAG/src/original/system/Utils.tjs" ] \
   && [ -f "$KAG/src/m2c_probe_config.tjs" ]; then
  M2C_A="$OUT/m2c_game"
  M2C_B="$OUT/m2c_game_cfg"
  rm -rf "$M2C_A" "$M2C_B"
  mkdir -p "$M2C_A/system"
  cp "$KAG/src/original/startup.tjs"           "$M2C_A/startup.tjs"
  cp "$KAG/src/original/system/Initialize.tjs" "$M2C_A/system/Initialize.tjs"
  cp "$KAG/src/original/system/Utils.tjs"      "$M2C_A/system/Utils.tjs"
  cp -R "$M2C_A" "$M2C_B"
  cp "$KAG/src/m2c_probe_config.tjs"           "$M2C_B/system/Config.tjs"

  # 硬校验：必须是官方原始 Shift-JIS 字节（7246 B），不能是转码后的 UTF-8 副本。
  INIT_BYTES=$(wc -c < "$M2C_A/system/Initialize.tjs" | tr -d ' ')
  if [ "$INIT_BYTES" != "7246" ]; then
    echo "[tjs2] M2c fixture 不是官方 Initialize.tjs 原始字节: ${INIT_BYTES}B (期望 7246B)"
    exit 1
  fi
  echo "[tjs2] M2c fixture: 官方 startup.tjs + Initialize.tjs (${INIT_BYTES}B SJIS) + Utils.tjs"

  echo "[tjs2] M2c: 编译并运行官方 Initialize.tjs 原文自测（System / Debug）"
  if clang++ -std=c++14 -O1 -g -fexceptions -DTJS2_POSIX \
        -I"$STUB" -I"$SRC" -I"$HOST" -I"$ROOT/native/Engine" -include "$COMPAT" \
        "$HOST/test_m2c.cpp" "$OUT/HostBridge.o" "$OUT/HostStreams.o" \
        "$OUT/XP3Archive.o" "$OUT/libtjs2.a" \
        -lz $ICONV_LIB -lpthread -o "$OUT/tjs2test_m2c" 2>"$OUT/link_m2c.log"; then
    OUTPUT_M2C=$("$OUT/tjs2test_m2c" "$M2C_A" "$M2C_B" "$OUT" 2>&1)
    diag_crash "M2c" "$?" "$OUT/tjs2test_m2c" "$M2C_A" "$M2C_B" "$OUT"
    echo "$OUTPUT_M2C" | sed 's/^/[tjs2]   /'
    if echo "$OUTPUT_M2C" | grep -q "M2C_RESULT=OK"; then
      echo "[tjs2] M2c 自测通过（官方 Initialize.tjs 真跑，推过 Config 检查与 createAppLock）"
    else
      echo "[tjs2] M2c 自测未通过"; exit 1
    fi
  else
    echo "[tjs2] M2c 自测链接失败:"; cat "$OUT/link_m2c.log"; exit 1
  fi
fi

# ---- Milestone 2d: 正则（RegExp 原生类，std::regex 重写） ----------------
# M2c 停在 `Member "RegExp" does not exist` @ Initialize.tjs:164。本轮不再定义
# TJS_NO_REGEXP，正则层由 GalBox 重写（native/TJS2/src/tjsRegExp.cpp）。
# 两个夹具：A 正则语义探针（自写）；B 官方 startup/Initialize/Utils 原文 +
# 探针 Config.tjs（必须推过 createAppLock 那一行正则）。
if [ -f "$HOST/HostBridge.cpp" ] && [ -f "$HOST/test_m2d.cpp" ] \
   && [ -f "$KAG/src/m2d_regex_probe.tjs" ] \
   && [ -f "$KAG/src/original/startup.tjs" ] \
   && [ -f "$KAG/src/original/system/Initialize.tjs" ] \
   && [ -f "$KAG/src/original/system/Utils.tjs" ] \
   && [ -f "$KAG/src/m2c_probe_config.tjs" ]; then
  M2D_A="$OUT/m2d_regex"
  M2D_B="$OUT/m2d_init"
  rm -rf "$M2D_A" "$M2D_B"
  mkdir -p "$M2D_A" "$M2D_B/system"
  cp "$KAG/src/m2d_regex_probe.tjs"            "$M2D_A/regex_probe.tjs"
  cp "$KAG/src/original/startup.tjs"           "$M2D_B/startup.tjs"
  cp "$KAG/src/original/system/Initialize.tjs" "$M2D_B/system/Initialize.tjs"
  cp "$KAG/src/original/system/Utils.tjs"      "$M2D_B/system/Utils.tjs"
  cp "$KAG/src/m2c_probe_config.tjs"           "$M2D_B/system/Config.tjs"

  # 探针里有日文字面量（/あ+/），文件必须是 UTF-8 多字节 —— 顺带验证 UTF-8 解码链。
  if ! LC_ALL=C grep -q $'\xe3\x81\x82' "$M2D_A/regex_probe.tjs"; then
    echo "[tjs2] M2d fixture 里没有 UTF-8 日文字面量（非 ASCII 用例等于没测）"
    exit 1
  fi
  # Initialize.tjs 必须仍是官方 7246B SJIS 原文，不能被顺手转码。
  INIT_BYTES_M2D=$(wc -c < "$M2D_B/system/Initialize.tjs" | tr -d ' ')
  if [ "$INIT_BYTES_M2D" != "7246" ]; then
    echo "[tjs2] M2d fixture 的 Initialize.tjs 不是官方原始字节: ${INIT_BYTES_M2D}B"
    exit 1
  fi
  echo "[tjs2] M2d fixture: 正则探针(UTF-8) + 官方 startup/Initialize(${INIT_BYTES_M2D}B SJIS)/Utils + 探针 Config"

  echo "[tjs2] M2d: 编译并运行正则自测（RegExp 语义 + 官方 createAppLock 正则）"
  if clang++ -std=c++14 -O1 -g -fexceptions -DTJS2_POSIX \
        -I"$STUB" -I"$SRC" -I"$HOST" -I"$ROOT/native/Engine" -include "$COMPAT" \
        "$HOST/test_m2d.cpp" "$OUT/HostBridge.o" "$OUT/HostStreams.o" \
        "$OUT/XP3Archive.o" "$OUT/libtjs2.a" \
        -lz $ICONV_LIB -lpthread -o "$OUT/tjs2test_m2d" 2>"$OUT/link_m2d.log"; then
    OUTPUT_M2D=$("$OUT/tjs2test_m2d" "$M2D_A" "$M2D_B" "$OUT" 2>&1)
    diag_crash "M2d" "$?" "$OUT/tjs2test_m2d" "$M2D_A" "$M2D_B" "$OUT"
    echo "$OUTPUT_M2D" | sed 's/^/[tjs2]   /'
    if echo "$OUTPUT_M2D" | grep -q "M2D_RESULT=OK"; then
      echo "[tjs2] M2d 自测通过（RegExp 语义 + 官方脚本推过正则阻塞点）"
    else
      echo "[tjs2] M2d 自测未通过"; exit 1
    fi
  else
    echo "[tjs2] M2d 自测链接失败:"; cat "$OUT/link_m2d.log"; exit 1
  fi
fi

# ---- Milestone 2e-1：官方 KAG 系统脚本全量 vendoring -----------------------
# run #32 停在 "Storages: script not found via autoPath: KAGLayer.tjs"（缺文件）。
# 本轮把 kag3/template/system/ 的 28 个上游 blob 原样装进夹具，把阻塞点逼成
# "class KAGLayer extends Layer" 里 Layer 原生类未实现（缺原生类）。
# 判据分三层：① 清单 sha256/bytes 逐文件校验；② TJS2 侧逐个 isExistentStorage
# 20/20 命中且不存在的哨兵不命中；③ scriptTrace 的正反对照（有文件 / 无文件）。
if [ -f "$HOST/HostBridge.cpp" ] && [ -f "$HOST/test_m2e.cpp" ] \
   && [ -f "$KAG/src/m2e_manifest_probe.tjs" ] \
   && [ -f "$KAG/src/original/system/KAGLayer.tjs" ]; then

  # ① 28 个文件必须与上游 git blob 逐字节一致（用仓库里的清单做唯一数据源）。
  #    contents API 会静默转 UTF-8，字节数与 sha256 都会变 —— 这一步就是抓它。
  if ! python3 - "$KAG/VENDOR_MANIFEST.json" "$KAG/src/original" <<'PYEOF'
import json, hashlib, os, sys
man = json.load(open(sys.argv[1], encoding='utf-8'))
root = sys.argv[2]
items = [e for e in man['upstream_raw']
         if e['path'].startswith('src/original/system/')]
assert len(items) == 28, 'expected 28 system blobs, got %d' % len(items)
total = 0
for e in items:
    p = os.path.join(root, e['path'][len('src/original/'):])
    raw = open(p, 'rb').read()
    assert len(raw) == e['bytes'], (e['path'], len(raw), e['bytes'])
    assert hashlib.sha256(raw).hexdigest() == e['sha256'], e['path']
    total += len(raw)
assert total == 448729, total
print('  [m2e] 28 official blobs verified:', total, 'B')
PYEOF
  then
    echo "[tjs2] M2e vendoring 校验失败：官方 system 原始字节与清单不符"
    exit 1
  fi

  M2E_GAME="$OUT/m2e_game"
  M2E_MIN="$OUT/m2e_min"
  rm -rf "$M2E_GAME" "$M2E_MIN"
  mkdir -p "$M2E_GAME/system" "$M2E_MIN/system"
  cp "$KAG/src/original/startup.tjs"            "$M2E_GAME/startup.tjs"
  cp "$KAG/src/m2e_manifest_probe.tjs"          "$M2E_GAME/m2e_manifest_probe.tjs"
  # 官方常量表（摘自官方 ScriptMgnIntf.cpp 的 TVPInitTJSScript）。
  # 官方脚本直接无前缀引用 clNone/stt*/cr*/VK_* 等，必须是全局常量。
  cp "$KAG/src/system/GalBoxConsts.tjs"          "$M2E_GAME/system/GalBoxConsts.tjs"
  cp "$KAG/src/original/system/"*               "$M2E_GAME/system/"
  # 官方 Config.tjs 是游戏自带的（上游只给 Config.~new 模板）。用 M2c 的
  # 版本对齐探针，让 Initialize.tjs 跳过 UpdateConfig.tjs，直奔系统脚本链。
  cp "$KAG/src/m2c_probe_config.tjs"            "$M2E_GAME/system/Config.tjs"

  # 负向对照：只有 M2d 的 4 个文件，KAGLayer.tjs 故意缺席。
  cp "$KAG/src/original/startup.tjs"            "$M2E_MIN/startup.tjs"
  cp "$KAG/src/original/system/Initialize.tjs"  "$M2E_MIN/system/Initialize.tjs"
  cp "$KAG/src/original/system/Utils.tjs"       "$M2E_MIN/system/Utils.tjs"
  cp "$KAG/src/m2c_probe_config.tjs"            "$M2E_MIN/system/Config.tjs"

  M2E_COUNT=$(find "$M2E_GAME/system" -maxdepth 1 -type f | wc -l | tr -d ' ')
  if [ "$M2E_COUNT" != "30" ]; then
    echo "[tjs2] M2e 夹具 system/ 应有 28 个官方文件 + Config.tjs + 官方常量表，实得 $M2E_COUNT"
    exit 1
  fi
  echo "[tjs2] M2e fixture: 官方 system/ 28 文件 + Config 探针；负向对照 4 文件"

  echo "[tjs2] M2e: 编译并运行全量系统脚本自测（清单 / 启动链 / 负向对照）"
  if clang++ -std=c++14 -O1 -g -fexceptions -DTJS2_POSIX \
        -I"$STUB" -I"$SRC" -I"$HOST" -I"$ROOT/native/Engine" -include "$COMPAT" \
        "$HOST/test_m2e.cpp" "$OUT/HostBridge.o" "$OUT/HostStreams.o" \
        "$OUT/XP3Archive.o" "$OUT/libtjs2.a" \
        -lz $ICONV_LIB -lpthread -o "$OUT/tjs2test_m2e" 2>"$OUT/link_m2e.log"; then
    OUTPUT_M2E=$("$OUT/tjs2test_m2e" "$M2E_GAME" "$M2E_MIN" "$OUT" 2>&1)
    diag_crash "M2e" "$?" "$OUT/tjs2test_m2e" "$M2E_GAME" "$M2E_MIN" "$OUT"
    echo "$OUTPUT_M2E" | sed 's/^/[tjs2]   /'
    if echo "$OUTPUT_M2E" | grep -q "M2E_RESULT=OK"; then
      echo "[tjs2] M2e 自测通过（缺文件 → 缺 Layer 原生类，三场景含负向对照）"
    else
      echo "[tjs2] M2e 自测未通过"; exit 1
    fi
  else
    echo "[tjs2] M2e 自测链接失败:"; cat "$OUT/link_m2e.log"; exit 1
  fi
fi


# ---- Milestone 2e-2：GalBox 的 Layer 骨架（纯 TJS2 shim） --------------------
# run #34 停在 KAGLayer.tjs:87 的 'Member "Layer" does not exist'。
# 本轮用一份自写的纯 TJS2 Layer 骨架顶上，让官方脚本类能真正实例化，
# 把下一批缺口逼出来。三个场景：继承成功 / 完整启动链 / 无骨架负向对照。
if [ -f "$HOST/HostBridge.cpp" ] && [ -f "$HOST/test_m2f.cpp" ] \
   && [ -f "$KAG/src/m2f_layer_probe.tjs" ] \
   && [ -f "$KAG/src/system/GalBoxLayer.tjs" ] \
   && [ -d "$M2E_GAME" ]; then

  M2F_GAME="$OUT/m2f_game"
  M2F_NOSHIM="$OUT/m2f_noshim"
  rm -rf "$M2F_GAME" "$M2F_NOSHIM"
  cp -R "$M2E_GAME" "$M2F_GAME"
  cp "$KAG/src/system/GalBoxLayer.tjs" "$M2F_GAME/system/GalBoxLayer.tjs"
  # M2e-10：Layer 构造会 new Font()，Font 骨架必须和 Layer 一起进夹具，
  # 否则早期里程碑（M2F/M2G/M2H 逐层 cp -R 继承）会因缺 Font 而回归失败。
  cp "$KAG/src/system/GalBoxFont.tjs"  "$M2F_GAME/system/GalBoxFont.tjs"
  cp "$KAG/src/m2f_layer_probe.tjs"    "$M2F_GAME/m2f_layer_probe.tjs"
  # 负向对照：同一套夹具，唯一区别就是**不放** Layer 骨架。
  cp -R "$M2F_GAME" "$M2F_NOSHIM"
  rm -f "$M2F_NOSHIM/system/GalBoxLayer.tjs"

  if [ ! -f "$M2F_NOSHIM/system/KAGLayer.tjs" ]; then
    echo "[tjs2] M2f 负向对照夹具缺少官方 KAGLayer.tjs"
    exit 1
  fi
  if [ -f "$M2F_NOSHIM/system/GalBoxLayer.tjs" ]; then
    echo "[tjs2] M2f 负向对照夹具里不该有 Layer 骨架"
    exit 1
  fi
  echo "[tjs2] M2f fixture: 官方 28 文件 + GalBox Layer 骨架；负向对照去掉骨架"

  echo "[tjs2] M2f: 编译并运行 Layer 骨架自测（继承 / 启动链 / 负向对照）"
  if clang++ -std=c++14 -O1 -g -fexceptions -DTJS2_POSIX \
        -I"$STUB" -I"$SRC" -I"$HOST" -I"$ROOT/native/Engine" -include "$COMPAT" \
        "$HOST/test_m2f.cpp" "$OUT/HostBridge.o" "$OUT/HostStreams.o" \
        "$OUT/XP3Archive.o" "$OUT/libtjs2.a" \
        -lz $ICONV_LIB -lpthread -o "$OUT/tjs2test_m2f" 2>"$OUT/link_m2f.log"; then
    OUTPUT_M2F=$("$OUT/tjs2test_m2f" "$M2F_GAME" "$M2F_NOSHIM" "$OUT" 2>&1)
    diag_crash "M2f" "$?" "$OUT/tjs2test_m2f" "$M2F_GAME" "$M2F_NOSHIM" "$OUT"
    echo "$OUTPUT_M2F" | sed 's/^/[tjs2]   /'
    if echo "$OUTPUT_M2F" | grep -q "M2F_RESULT=OK"; then
      echo "[tjs2] M2f 自测通过（官方脚本类成功继承 Layer 骨架 + 负向对照）"
    else
      echo "[tjs2] M2f 自测未通过"; exit 1
    fi
  else
    echo "[tjs2] M2f 自测链接失败:"; cat "$OUT/link_m2f.log"; exit 1
  fi
fi


# ---- Milestone 2e-3：GalBox 的 Window 骨架（纯 TJS2 shim） -------------------
# run #36 停在 MainWindow.tjs:7 的 'Member "Window" does not exist'。
# 本轮补 Window 骨架 + System.dataPath + Debug.logLocation，
# 三个场景：继承成立 / 完整启动链 / 无 Window 骨架负向对照。
if [ -f "$HOST/HostBridge.cpp" ] && [ -f "$HOST/test_m2g.cpp" ] \
   && [ -f "$KAG/src/m2g_window_probe.tjs" ] \
   && [ -f "$KAG/src/system/GalBoxWindow.tjs" ] \
   && [ -d "$M2F_GAME" ]; then

  M2G_GAME="$OUT/m2g_game"
  M2G_NOWIN="$OUT/m2g_nowindow"
  rm -rf "$M2G_GAME" "$M2G_NOWIN"
  cp -R "$M2F_GAME" "$M2G_GAME"
  cp "$KAG/src/system/GalBoxWindow.tjs" "$M2G_GAME/system/GalBoxWindow.tjs"
  # M2e-7：Window 骨架构造里会 new Menu()，夹具必须带上 Menu 骨架。
  cp "$KAG/src/system/GalBoxMenu.tjs"   "$M2G_GAME/system/GalBoxMenu.tjs"
  cp "$KAG/src/m2g_window_probe.tjs"    "$M2G_GAME/m2g_window_probe.tjs"
  # 负向对照：Layer 骨架**在**，Window 骨架**不在**。
  cp -R "$M2F_GAME" "$M2G_NOWIN"
  cp "$KAG/src/m2g_window_probe.tjs"    "$M2G_NOWIN/m2g_window_probe.tjs"

  if [ ! -f "$M2G_NOWIN/system/GalBoxLayer.tjs" ] \
     || [ -f "$M2G_NOWIN/system/GalBoxWindow.tjs" ]; then
    echo "[tjs2] M2g 负向对照夹具状态不对（Layer 应在、Window 应不在）"
    exit 1
  fi
  echo "[tjs2] M2g fixture: Layer+Window 骨架；负向对照只有 Layer 骨架"

  echo "[tjs2] M2g: 编译并运行 Window 骨架自测（继承 / 启动链 / 负向对照）"
  if clang++ -std=c++14 -O1 -g -fexceptions -DTJS2_POSIX \
        -I"$STUB" -I"$SRC" -I"$HOST" -I"$ROOT/native/Engine" -include "$COMPAT" \
        "$HOST/test_m2g.cpp" "$OUT/HostBridge.o" "$OUT/HostStreams.o" \
        "$OUT/XP3Archive.o" "$OUT/libtjs2.a" \
        -lz $ICONV_LIB -lpthread -o "$OUT/tjs2test_m2g" 2>"$OUT/link_m2g.log"; then
    OUTPUT_M2G=$("$OUT/tjs2test_m2g" "$M2G_GAME" "$M2G_NOWIN" "$OUT" 2>&1)
    diag_crash "M2g" "$?" "$OUT/tjs2test_m2g" "$M2G_GAME" "$M2G_NOWIN" "$OUT"
    echo "$OUTPUT_M2G" | sed 's/^/[tjs2]   /'
    if echo "$OUTPUT_M2G" | grep -q "M2G_RESULT=OK"; then
      echo "[tjs2] M2g 自测通过（官方 KAGWindow 不再缺 Window 父类 + 负向对照）"
    else
      echo "[tjs2] M2g 自测未通过"; exit 1
    fi
  else
    echo "[tjs2] M2g 自测链接失败:"; cat "$OUT/link_m2g.log"; exit 1
  fi
fi


# ---- Milestone 2e-5：夹具换成官方 Config.tjs + 查同进程状态泄漏 -------------
# run #45 的缺口 'Member "KAGWindow_config" does not exist' @ MainWindow.tjs:273
# 来自**游戏自带的 Config.tjs**（KAGWindow_config 定义在官方 Config.~new:51），
# 而我们的夹具用的是 M2c 那份只有一行版本号的探针版 Config。
# 本轮用官方 Config.~new 的原始 SJIS 字节当 Config.tjs，让夹具第一次接近真实游戏。
if [ -f "$HOST/test_m2h.cpp" ] && [ -d "$M2G_GAME" ] \
   && [ -f "$KAG/src/original/system/Config.~new" ]; then

  M2H_GAME="$OUT/m2h_game"
  M2H_CTRL="$OUT/m2h_ctrl"
  rm -rf "$M2H_GAME" "$M2H_CTRL"
  cp -R "$M2G_GAME" "$M2H_GAME"
  # M2e-6：把 Pad 骨架放进夹具，让 UpdateConfig.tjs:79 的 new Pad() 能实例化。
  cp "$KAG/src/system/GalBoxPad.tjs" "$M2H_GAME/system/GalBoxPad.tjs"
  # M2e-9：把 Timer 骨架放进夹具，让 MainWindow.tjs:337 的 new Timer() 能实例化。
  cp "$KAG/src/system/GalBoxTimer.tjs" "$M2H_GAME/system/GalBoxTimer.tjs"
  # 官方 Config.tjs = 原始 Shift-JIS 字节 + 前置一行 ASCII 版本号。
  # 版本号用来让 Initialize.tjs 跳过 UpdateConfig.tjs 分支；
  # ASCII 在 CP932 里恒等，和 SJIS 正文混排安全。
  { printf 'global.config_version = "3.29-dev.20071022";\n'; \
    cat "$KAG/src/original/system/Config.~new"; } > "$M2H_GAME/system/Config.tjs"
  # 负向对照：同一套夹具，Config.tjs 换回 M2c 的探针版（缺 KAGWindow_config）
  cp -R "$M2G_GAME" "$M2H_CTRL"
  cp "$KAG/src/m2c_probe_config.tjs" "$M2H_CTRL/system/Config.tjs"
  # M2e-7 负向对照：唯一区别是**不放** Menu 骨架，其余（官方 Config + Pad）
  # 完全一样。它必须复现 `Member "MenuItem" does not exist`，否则 A 场景
  # 的「跨过 MenuItem」就是恒真式自证。
  M2H_NOMENU="$OUT/m2h_nomenu"
  rm -rf "$M2H_NOMENU"
  cp -R "$M2H_GAME" "$M2H_NOMENU"
  rm -f "$M2H_NOMENU/system/GalBoxMenu.tjs"
  if [ -f "$M2H_NOMENU/system/GalBoxMenu.tjs" ]; then
    echo "[tjs2] M2e-7 负向对照夹具里不该有 GalBoxMenu.tjs"
    exit 1
  fi
  # M2e-8 负向对照：只包含官方 Config + Menu 骨架，但把 Window 骨架里
  # M2E8_STYLE_BEGIN/END 之间的三个样式成员整段删掉。它必须复现
  # `Member "borderStyle" does not exist` @ MainWindow.tjs。
  M2H_NOSTYLE="$OUT/m2h_nostyle"
  rm -rf "$M2H_NOSTYLE"
  cp -R "$M2H_GAME" "$M2H_NOSTYLE"
  python3 - "$M2H_NOSTYLE/system/GalBoxWindow.tjs" <<'PYEOF' || exit 1
import sys
p = sys.argv[1]
with open(p, encoding='utf-8') as f:
    src = f.read()
if 'M2E8_STYLE_BEGIN' not in src or 'M2E8_STYLE_END' not in src:
    raise SystemExit('M2e-8 style markers missing')
out, skip = [], False
for line in src.splitlines(True):
    if 'M2E8_STYLE_BEGIN' in line:
        skip = True
        continue
    if 'M2E8_STYLE_END' in line:
        skip = False
        continue
    if not skip:
        out.append(line)
with open(p, 'w', encoding='utf-8', newline='') as f:
    f.write(''.join(out))
PYEOF
  if grep -q "M2E8_STYLE_BEGIN" "$M2H_NOSTYLE/system/GalBoxWindow.tjs"; then
    echo "[tjs2] M2e-8 负向对照没能删掉样式成员"
    exit 1
  fi
  # M2e-9 负向对照：唯一区别是**不放** Timer 骨架，其余全一样。
  # 必须复现 `Member "Timer" does not exist` @ MainWindow.tjs:337。
  M2H_NOTIMER="$OUT/m2h_notimer"
  rm -rf "$M2H_NOTIMER"
  cp -R "$M2H_GAME" "$M2H_NOTIMER"
  rm -f "$M2H_NOTIMER/system/GalBoxTimer.tjs"
  if [ -f "$M2H_NOTIMER/system/GalBoxTimer.tjs" ]; then
    echo "[tjs2] M2e-9 负向对照夹具里不该有 GalBoxTimer.tjs"
    exit 1
  fi
  # M2e-10 负向对照：Font 骨架文件**保留**，只把 Layer 里的 font 属性段
  # （M2E10_FONT_BEGIN/END 之间）整段删掉 —— 这样 Layer 构造不再 new Font，
  # 官方 AnimationLayer.tjs:343 读 font.face 时必须复现
  # `Member "font" does not exist`。这是精准对照：唯一差异是 Layer.font。
  M2H_NOFONT="$OUT/m2h_nofont"
  rm -rf "$M2H_NOFONT"
  cp -R "$M2H_GAME" "$M2H_NOFONT"
  python3 - "$M2H_NOFONT/system/GalBoxLayer.tjs" <<'PYEOF' || exit 1
import sys
p = sys.argv[1]
with open(p, encoding='utf-8') as f:
    src = f.read()
if 'M2E10_FONT_BEGIN' not in src or 'M2E10_FONT_END' not in src:
    raise SystemExit('M2e-10 font markers missing')
out, skip = [], False
for line in src.splitlines(True):
    if 'M2E10_FONT_BEGIN' in line:
        skip = True
        continue
    if 'M2E10_FONT_END' in line:
        skip = False
        continue
    # 构造函数里的调用行也要删, 否则 initFont 未定义会先报别的错,
    # 盖住我们要复现的 `font` 缺口。
    if 'initFont();' in line:
        continue
    if not skip:
        out.append(line)
with open(p, 'w', encoding='utf-8', newline='') as f:
    f.write(''.join(out))
PYEOF
  if grep -q "M2E10_FONT_BEGIN" "$M2H_NOFONT/system/GalBoxLayer.tjs"; then
    echo "[tjs2] M2e-10 负向对照没能删掉 Layer.font 段"
    exit 1
  fi

  if ! grep -q "function KAGWindow_config" "$M2H_GAME/system/Config.tjs"; then
    echo "[tjs2] M2e-5 夹具的 Config.tjs 里没有 KAGWindow_config（官方原文没拼进去？）"
    exit 1
  fi
  if grep -q "function KAGWindow_config" "$M2H_CTRL/system/Config.tjs"; then
    echo "[tjs2] M2e-5 负向对照夹具不该有 KAGWindow_config"
    exit 1
  fi
  echo "[tjs2] M2e-5 fixture: 官方 Config.tjs（含 KAGWindow_config）；负向对照用探针版"

  echo "[tjs2] M2e-5: 编译并运行自测（官方 Config.tjs / 二次执行诊断 / 负向对照）"
  if clang++ -std=c++14 -O1 -g -fexceptions -DTJS2_POSIX \
        -I"$STUB" -I"$SRC" -I"$HOST" -I"$ROOT/native/Engine" -include "$COMPAT" \
        "$HOST/test_m2h.cpp" "$OUT/HostBridge.o" "$OUT/HostStreams.o" \
        "$OUT/XP3Archive.o" "$OUT/libtjs2.a" \
        -lz $ICONV_LIB -lpthread -o "$OUT/tjs2test_m2h" 2>"$OUT/link_m2h.log"; then
    # 每个场景**独立进程**跑：run #40 出现过「同进程内第二次执行会破坏官方
    # 启动链」, 若三个场景挤在一个进程里, 后面的场景会被前面的污染,
    # 负向对照就会假红。独立进程 => 每个场景都是干净首跑。
    for M2H_CASE in A B NOMENU NOSTYLE NOTIMER NOFONT LEAK LEAK2; do
      OUTPUT_M2H=$("$OUT/tjs2test_m2h" "$M2H_CASE" "$M2H_GAME" "$M2H_CTRL" "$OUT" "$M2H_NOMENU" "$M2H_NOSTYLE" "$M2H_NOTIMER" "$M2H_NOFONT" 2>&1)
      diag_crash "M2h-$M2H_CASE" "$?" "$OUT/tjs2test_m2h" "$M2H_CASE" "$M2H_GAME" "$M2H_CTRL" "$OUT" "$M2H_NOMENU" "$M2H_NOSTYLE" "$M2H_NOTIMER" "$M2H_NOFONT"
      echo "$OUTPUT_M2H" | sed 's/^/[tjs2]   /'
      case "$M2H_CASE" in
        A|B|NOMENU|NOSTYLE|NOTIMER|NOFONT)
          if ! echo "$OUTPUT_M2H" | grep -q "M2H_RESULT=OK"; then
            echo "[tjs2] 场景 $M2H_CASE 未通过"; exit 1
          fi
          ;;
        *)
          # LEAK / LEAK2 只是诊断, 不参与判定(它们的结论写在 verdict 里)
          ;;
      esac
    done
    echo "[tjs2] M2e-5 自测通过（官方 Config.tjs 消掉 KAGWindow_config 缺口 + 负向对照）"
  else
    echo "[tjs2] M2e-5 自测链接失败:"; cat "$OUT/link_m2h.log"; exit 1
  fi
fi

exit 0
