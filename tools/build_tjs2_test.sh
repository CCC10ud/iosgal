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
#                      + KAGParser 骨架补 debugLevel 槽位（Milestone 2e-13）
#                      + Layer 骨架补命中测试面 hitType/hitThreshold（Milestone 2e-14）
#                      + Layer 骨架补图像面 setImageSize/setSizeToImageSize（Milestone 2e-15）
#                      + Layer 骨架补绘制面 face/fillRect（Milestone 2e-16）
#                      + Layer 骨架补光标面 cursor（Milestone 2e-17）
#                      + Layer 骨架补图层名 name（Milestone 2e-18）
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

# 按 `XXXX_BEGIN` / `XXXX_END` 标记整段删除，用于生成负向对照夹具。
# 统一走这里，避免每个里程碑复制一份 20 行的 python 块（M2e-14 起有三个对照）。
drop_marked() {
  python3 - "$1" "$2" "$3" <<'PYEOF' || exit 1
import sys
p, b, e = sys.argv[1], sys.argv[2], sys.argv[3]
with open(p, encoding='utf-8') as f:
    src = f.read()
if b not in src or e not in src:
    raise SystemExit('markers missing: %s / %s' % (b, e))
out, skip = [], False
for line in src.splitlines(True):
    if b in line:
        skip = True
        continue
    if e in line:
        skip = False
        continue
    if not skip:
        out.append(line)
with open(p, 'w', encoding='utf-8', newline='') as f:
    f.write(''.join(out))
PYEOF
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
  # M2e-11：KAGParser 在 Conductor.tjs 的 extends 阶段就需要，从 M2F 起进夹具。
  cp "$KAG/src/system/GalBoxKAGParser.tjs" "$M2F_GAME/system/GalBoxKAGParser.tjs"
  # M2e-12：AsyncTrigger 在 Conductor.tjs 构造里 new，同样从 M2F 起进夹具。
  cp "$KAG/src/system/GalBoxAsyncTrigger.tjs" "$M2F_GAME/system/GalBoxAsyncTrigger.tjs"
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
  # M2e-25：把 MIDISoundBuffer 骨架放进夹具，让 BGM.tjs:114 的
  # `class KAGMIDISoundBuffer extends MIDISoundBuffer, ...` 能解析。
  cp "$KAG/src/system/GalBoxSound.tjs" "$M2H_GAME/system/GalBoxSound.tjs"
  if ! grep -q "class MIDISoundBuffer" "$M2H_GAME/system/GalBoxSound.tjs"; then
    echo "[tjs2] M2e-25 夹具的 GalBoxSound.tjs 里没有 class MIDISoundBuffer"
    exit 1
  fi
  # M2e-27：把 VideoOverlay 骨架放进夹具，让 Movie.tjs:4 的
  # `class Movie extends VideoOverlay` 能解析。
  cp "$KAG/src/system/GalBoxMovie.tjs" "$M2H_GAME/system/GalBoxMovie.tjs"
  if ! grep -q "class VideoOverlay" "$M2H_GAME/system/GalBoxMovie.tjs"; then
    echo "[tjs2] M2e-27 夹具的 GalBoxMovie.tjs 里没有 class VideoOverlay"
    exit 1
  fi
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
  # M2e-11 负向对照：唯一区别是**不放** KAGParser 骨架，其余全一样。
  # 必须复现 `Member "KAGParser" does not exist` @ Conductor.tjs:13。
  M2H_NOKAGPARSER="$OUT/m2h_nokagparser"
  rm -rf "$M2H_NOKAGPARSER"
  cp -R "$M2H_GAME" "$M2H_NOKAGPARSER"
  rm -f "$M2H_NOKAGPARSER/system/GalBoxKAGParser.tjs"
  if [ -f "$M2H_NOKAGPARSER/system/GalBoxKAGParser.tjs" ]; then
    echo "[tjs2] M2e-11 负向对照夹具里不该有 GalBoxKAGParser.tjs"
    exit 1
  fi
  # M2e-12 负向对照：唯一区别是**不放** AsyncTrigger 骨架，其余全一样。
  # 必须复现 `Member "AsyncTrigger" does not exist` @ Conductor.tjs:33。
  M2H_NOASYNCTRIGGER="$OUT/m2h_noasynctrigger"
  rm -rf "$M2H_NOASYNCTRIGGER"
  cp -R "$M2H_GAME" "$M2H_NOASYNCTRIGGER"
  rm -f "$M2H_NOASYNCTRIGGER/system/GalBoxAsyncTrigger.tjs"
  if [ -f "$M2H_NOASYNCTRIGGER/system/GalBoxAsyncTrigger.tjs" ]; then
    echo "[tjs2] M2e-12 负向对照夹具里不该有 GalBoxAsyncTrigger.tjs"
    exit 1
  fi

  # M2e-13 负向对照：KAGParser 骨架文件**保留**，只把 M2E13_BEGIN/END 之间
  # 的 `var debugLevel;` 整段删掉。官方 AnimationLayer.tjs:266 / Conductor.tjs:350
  # 给该成员赋值，缺了就必须复现 `Member "debugLevel" does not exist`。
  M2H_NODEBUGLEVEL="$OUT/m2h_nodebuglevel"
  rm -rf "$M2H_NODEBUGLEVEL"
  cp -R "$M2H_GAME" "$M2H_NODEBUGLEVEL"
  python3 - "$M2H_NODEBUGLEVEL/system/GalBoxKAGParser.tjs" <<'PYEOF' || exit 1
import sys
p = sys.argv[1]
with open(p, encoding='utf-8') as f:
    src = f.read()
if 'M2E13_BEGIN' not in src or 'M2E13_END' not in src:
    raise SystemExit('M2e-13 debugLevel markers missing')
out, skip = [], False
for line in src.splitlines(True):
    if 'M2E13_BEGIN' in line:
        skip = True
        continue
    if 'M2E13_END' in line:
        skip = False
        continue
    if not skip:
        out.append(line)
with open(p, 'w', encoding='utf-8', newline='') as f:
    f.write(''.join(out))
PYEOF
  if grep -q "M2E13_BEGIN" "$M2H_NODEBUGLEVEL/system/GalBoxKAGParser.tjs"; then
    echo "[tjs2] M2e-13 负向对照没能删掉 debugLevel 成员"
    exit 1
  fi
  if grep -q "var debugLevel" "$M2H_NODEBUGLEVEL/system/GalBoxKAGParser.tjs"; then
    echo "[tjs2] M2e-13 负向对照里 debugLevel 还在"
    exit 1
  fi

  # M2e-14 负向对照（一）：Layer 骨架文件保留，把 M2E14 整段（hitType + hitThreshold）
  # 删掉。必须复现 `Member "hitType" does not exist` @ GraphicLayer.tjs:337。
  M2H_NOHITTYPE="$OUT/m2h_nohittype"
  rm -rf "$M2H_NOHITTYPE"
  cp -R "$M2H_GAME" "$M2H_NOHITTYPE"
  drop_marked "$M2H_NOHITTYPE/system/GalBoxLayer.tjs" M2E14_BEGIN M2E14_END
  if grep -q "M2E14_BEGIN\|var hitType\|var hitThreshold" "$M2H_NOHITTYPE/system/GalBoxLayer.tjs"; then
    echo "[tjs2] M2e-14 NOHITTYPE 夹具里命中面成员还在"
    exit 1
  fi
  if ! grep -q "var font" "$M2H_NOHITTYPE/system/GalBoxLayer.tjs"; then
    echo "[tjs2] M2e-14 NOHITTYPE 夹具把无关成员也删掉了"
    exit 1
  fi

  # M2e-14 负向对照（二）：只删 hitThreshold 那一行，hitType 保留。
  # 必须复现 `Member "hitThreshold" does not exist` @ GraphicLayer.tjs:338 ——
  # 否则说明这一轮补 hitThreshold 是"顺手"，不是必要。
  M2H_NOHITTHRESHOLD="$OUT/m2h_nohitthreshold"
  rm -rf "$M2H_NOHITTHRESHOLD"
  cp -R "$M2H_GAME" "$M2H_NOHITTHRESHOLD"
  drop_marked "$M2H_NOHITTHRESHOLD/system/GalBoxLayer.tjs" M2E14_THRESHOLD_BEGIN M2E14_THRESHOLD_END
  if grep -q "M2E14_THRESHOLD_BEGIN\|var hitThreshold" "$M2H_NOHITTHRESHOLD/system/GalBoxLayer.tjs"; then
    echo "[tjs2] M2e-14 NOHITTHRESHOLD 夹具里 hitThreshold 还在"
    exit 1
  fi
  if ! grep -q "var hitType" "$M2H_NOHITTHRESHOLD/system/GalBoxLayer.tjs"; then
    echo "[tjs2] M2e-14 NOHITTHRESHOLD 夹具把 hitType 也删掉了"
    exit 1
  fi

  # M2e-15 负向对照（一）：整块图像面（imageWidth/imageHeight + 两个方法）删掉。
  # 必须复现 `Member "setImageSize" does not exist` @ MainWindow.tjs:348。
  M2H_NOIMAGESIZE="$OUT/m2h_noimagesize"
  rm -rf "$M2H_NOIMAGESIZE"
  cp -R "$M2H_GAME" "$M2H_NOIMAGESIZE"
  drop_marked "$M2H_NOIMAGESIZE/system/GalBoxLayer.tjs" M2E15_BEGIN M2E15_END
  if grep -q "M2E15_BEGIN\|var imageWidth\|var imageHeight\|function setImageSize\|function setSizeToImageSize" "$M2H_NOIMAGESIZE/system/GalBoxLayer.tjs"; then
    echo "[tjs2] M2e-15 NOIMAGESIZE 夹具里图像面成员还在"
    exit 1
  fi
  if ! grep -q "var hitType" "$M2H_NOIMAGESIZE/system/GalBoxLayer.tjs"; then
    echo "[tjs2] M2e-15 NOIMAGESIZE 夹具把命中面也删掉了"
    exit 1
  fi

  # M2e-15 负向对照（二）：只删 setSizeToImageSize，setImageSize 保留。
  # 必须复现 `Member "setSizeToImageSize" does not exist` @ MainWindow.tjs:349 ——
  # 否则说明它是"顺手"补的，不是独立缺口。
  M2H_NOSIZETOIMAGESIZE="$OUT/m2h_nosizetoimagesize"
  rm -rf "$M2H_NOSIZETOIMAGESIZE"
  cp -R "$M2H_GAME" "$M2H_NOSIZETOIMAGESIZE"
  drop_marked "$M2H_NOSIZETOIMAGESIZE/system/GalBoxLayer.tjs" M2E15_SIZETO_BEGIN M2E15_SIZETO_END
  if grep -q "M2E15_SIZETO_BEGIN\|function setSizeToImageSize" "$M2H_NOSIZETOIMAGESIZE/system/GalBoxLayer.tjs"; then
    echo "[tjs2] M2e-15 NOSIZETOIMAGESIZE 夹具里 setSizeToImageSize 还在"
    exit 1
  fi
  if ! grep -q "function setImageSize" "$M2H_NOSIZETOIMAGESIZE/system/GalBoxLayer.tjs"; then
    echo "[tjs2] M2e-15 NOSIZETOIMAGESIZE 夹具把 setImageSize 也删掉了"
    exit 1
  fi

  # M2e-16 负向对照（一）：只删绘制面成员 face，fillRect 保留。
  # 必须复现 `Member "face" does not exist` @ GraphicLayer.tjs:422。
  M2H_NODRAWFACE="$OUT/m2h_nodrawface"
  rm -rf "$M2H_NODRAWFACE"
  cp -R "$M2H_GAME" "$M2H_NODRAWFACE"
  drop_marked "$M2H_NODRAWFACE/system/GalBoxLayer.tjs" M2E16_FACE_BEGIN M2E16_FACE_END
  if grep -q "M2E16_FACE_BEGIN\|var face" "$M2H_NODRAWFACE/system/GalBoxLayer.tjs"; then
    echo "[tjs2] M2e-16 NODRAWFACE 夹具里 face 还在"
    exit 1
  fi
  if ! grep -q "function fillRect" "$M2H_NODRAWFACE/system/GalBoxLayer.tjs"; then
    echo "[tjs2] M2e-16 NODRAWFACE 夹具把 fillRect 也删掉了"
    exit 1
  fi

  # M2e-16 负向对照（二）：只删 fillRect，face 保留。
  # 必须复现 `Member "fillRect" does not exist` @ GraphicLayer.tjs:423 ——
  # 否则说明它是"顺手"补的。
  M2H_NOFILLRECT="$OUT/m2h_nofillrect"
  rm -rf "$M2H_NOFILLRECT"
  cp -R "$M2H_GAME" "$M2H_NOFILLRECT"
  drop_marked "$M2H_NOFILLRECT/system/GalBoxLayer.tjs" M2E16_FILLRECT_BEGIN M2E16_FILLRECT_END
  if grep -q "M2E16_FILLRECT_BEGIN\|function fillRect" "$M2H_NOFILLRECT/system/GalBoxLayer.tjs"; then
    echo "[tjs2] M2e-16 NOFILLRECT 夹具里 fillRect 还在"
    exit 1
  fi
  if ! grep -q "var face" "$M2H_NOFILLRECT/system/GalBoxLayer.tjs"; then
    echo "[tjs2] M2e-16 NOFILLRECT 夹具把 face 也删掉了"
    exit 1
  fi

  # M2e-17 负向对照：只删光标面成员 cursor。
  # 必须复现 `Member "cursor" does not exist` @ GraphicLayer.tjs:58
  # （官方 setDefaultCursor 里 cursor = defaultCursor = c）。
  M2H_NOCURSOR="$OUT/m2h_nocursor"
  rm -rf "$M2H_NOCURSOR"
  cp -R "$M2H_GAME" "$M2H_NOCURSOR"
  drop_marked "$M2H_NOCURSOR/system/GalBoxLayer.tjs" M2E17_BEGIN M2E17_END
  # 带初值匹配：裸 "var cursor" 会被 M2e-24 新增的 var cursorX 子串命中（第三次同类坑）。
  if grep -q "M2E17_BEGIN\|var cursor = crDefault" "$M2H_NOCURSOR/system/GalBoxLayer.tjs"; then
    echo "[tjs2] M2e-17 NOCURSOR 夹具里 cursor 还在"
    exit 1
  fi

  # M2e-18 负向对照：只删图层名成员 name。
  # 必须复现 `Member "name" does not exist` @ HistoryLayer.tjs:110
  M2H_NONAME="$OUT/m2h_noname"
  rm -rf "$M2H_NONAME"
  cp -R "$M2H_GAME" "$M2H_NONAME"
  drop_marked "$M2H_NONAME/system/GalBoxLayer.tjs" M2E18_BEGIN M2E18_END
  if grep -q "M2E18_BEGIN\|var name" "$M2H_NONAME/system/GalBoxLayer.tjs"; then
    echo "[tjs2] M2e-18 NONAME 夹具里 name 还在"
    exit 1
  fi
  for keep in "var cursor = crDefault" "var face" "function fillRect" "var imageWidth" "var hitType"; do
    if ! grep -q "$keep" "$M2H_NONAME/system/GalBoxLayer.tjs"; then
      echo "[tjs2] M2e-18 NONAME 夹具丢了无关成员：$keep"
      exit 1
    fi
  done
  # 别把同段的绘制面一起删了 —— 否则测的是"整段没补"，不是"只缺 cursor"。
  for keep in "var face" "function fillRect" "var imageWidth" "var hitType"; do
    if ! grep -q "$keep" "$M2H_NOCURSOR/system/GalBoxLayer.tjs"; then
      echo "[tjs2] M2e-17 NOCURSOR 夹具丢了无关成员：$keep"
      exit 1
    fi
  done

  # M2e-19 负向对照：只删焦点接收成员 focusable。
  # 必须复现 `Member "focusable" does not exist` @ HistoryLayer.tjs:130
  M2H_NOFOCUSABLE="$OUT/m2h_nofocusable"
  rm -rf "$M2H_NOFOCUSABLE"
  cp -R "$M2H_GAME" "$M2H_NOFOCUSABLE"
  drop_marked "$M2H_NOFOCUSABLE/system/GalBoxLayer.tjs" M2E19_BEGIN M2E19_END
  if grep -q "M2E19_BEGIN\|var focusable" "$M2H_NOFOCUSABLE/system/GalBoxLayer.tjs"; then
    echo "[tjs2] M2e-19 NOFOCUSABLE 夹具里 focusable 还在"
    exit 1
  fi
  for keep in "var name" "var cursor" "var face" "function fillRect" "var imageWidth" "var hitType"; do
    if ! grep -q "$keep" "$M2H_NOFOCUSABLE/system/GalBoxLayer.tjs"; then
      echo "[tjs2] M2e-19 NOFOCUSABLE 夹具丢了无关成员：$keep"
      exit 1
    fi
  done

  # M2e-20 负向对照：只删显示尺寸方法 setSize。
  # 必须复现 `Member "setSize" does not exist` @ AnimationLayer.tjs:367
  M2H_NOSIZE="$OUT/m2h_nosize"
  rm -rf "$M2H_NOSIZE"
  cp -R "$M2H_GAME" "$M2H_NOSIZE"
  drop_marked "$M2H_NOSIZE/system/GalBoxLayer.tjs" M2E20_BEGIN M2E20_END
  if grep -q "M2E20_BEGIN\|function setSize(" "$M2H_NOSIZE/system/GalBoxLayer.tjs"; then
    echo "[tjs2] M2e-20 NOSIZE 夹具里 setSize 还在"
    exit 1
  fi
  for keep in "function setSizeToImageSize" "function setPos" "var focusable" "var name" "var cursor" "var face"; do
    if ! grep -q "$keep" "$M2H_NOSIZE/system/GalBoxLayer.tjs"; then
      echo "[tjs2] M2e-20 NOSIZE 夹具丢了无关成员：$keep"
      exit 1
    fi
  done

  # M2e-21 负向对照：只删图像脏标记 imageModified。
  # 必须复现 `Member "imageModified" does not exist` @ MessageLayer.tjs:493
  M2H_NOIMAGEMODIFIED="$OUT/m2h_noimagemodified"
  rm -rf "$M2H_NOIMAGEMODIFIED"
  cp -R "$M2H_GAME" "$M2H_NOIMAGEMODIFIED"
  drop_marked "$M2H_NOIMAGEMODIFIED/system/GalBoxLayer.tjs" M2E21_BEGIN M2E21_END
  if grep -q "M2E21_BEGIN\|var imageModified" "$M2H_NOIMAGEMODIFIED/system/GalBoxLayer.tjs"; then
    echo "[tjs2] M2e-21 NOIMAGEMODIFIED 夹具里 imageModified 还在"
    exit 1
  fi
  for keep in "function setSize(" "var focusable" "var name" "var cursor" "var face" "function setSizeToImageSize"; do
    if ! grep -q "$keep" "$M2H_NOIMAGEMODIFIED/system/GalBoxLayer.tjs"; then
      echo "[tjs2] M2e-21 NOIMAGEMODIFIED 夹具丢了无关成员：$keep"
      exit 1
    fi
  done

  # M2e-22 负向对照：只删颜色矩形填充 colorRect。
  # 必须复现 `Member "colorRect" does not exist` @ MessageLayer.tjs:587
  M2H_NOCOLORRECT="$OUT/m2h_nocolorrect"
  rm -rf "$M2H_NOCOLORRECT"
  cp -R "$M2H_GAME" "$M2H_NOCOLORRECT"
  drop_marked "$M2H_NOCOLORRECT/system/GalBoxLayer.tjs" M2E22_BEGIN M2E22_END
  if grep -q "M2E22_BEGIN\|function colorRect" "$M2H_NOCOLORRECT/system/GalBoxLayer.tjs"; then
    echo "[tjs2] M2e-22 NOCOLORRECT 夹具里 colorRect 还在"
    exit 1
  fi
  for keep in "function fillRect" "var imageModified" "function setSize(" "var focusable" "var name" "var cursor" "var face" "function setSizeToImageSize"; do
    if ! grep -q "$keep" "$M2H_NOCOLORRECT/system/GalBoxLayer.tjs"; then
      echo "[tjs2] M2e-22 NOCOLORRECT 夹具丢了无关成员：$keep"
      exit 1
    fi
  done

  # M2e-23 负向对照：只删父层提示开关 showParentHint。
  # 必须复现 `Member "showParentHint" does not exist` @ MessageLayer.tjs:1987
  M2H_NOSHOWPARENTHINT="$OUT/m2h_noshowparenthint"
  rm -rf "$M2H_NOSHOWPARENTHINT"
  cp -R "$M2H_GAME" "$M2H_NOSHOWPARENTHINT"
  drop_marked "$M2H_NOSHOWPARENTHINT/system/GalBoxLayer.tjs" M2E23_BEGIN M2E23_END
  if grep -q "M2E23_BEGIN\|var showParentHint" "$M2H_NOSHOWPARENTHINT/system/GalBoxLayer.tjs"; then
    echo "[tjs2] M2e-23 NOSHOWPARENTHINT 夹具里 showParentHint 还在"
    exit 1
  fi
  for keep in "function colorRect" "function fillRect" "var imageModified" "function setSize(" "var focusable" "var name" "var cursor" "var face" "function setSizeToImageSize"; do
    if ! grep -q "$keep" "$M2H_NOSHOWPARENTHINT/system/GalBoxLayer.tjs"; then
      echo "[tjs2] M2e-23 NOSHOWPARENTHINT 夹具丢了无关成员：$keep"
      exit 1
    fi
  done

  # M2e-24 负向对照：只删层内鼠标坐标 cursorX/cursorY（成对声明一起删）。
  # 必须复现 `Member "cursorX" does not exist` @ MessageLayer.tjs:607
  M2H_NOCURSORX="$OUT/m2h_nocursorx"
  rm -rf "$M2H_NOCURSORX"
  cp -R "$M2H_GAME" "$M2H_NOCURSORX"
  drop_marked "$M2H_NOCURSORX/system/GalBoxLayer.tjs" M2E24_BEGIN M2E24_END
  if grep -q "M2E24_BEGIN\|var cursorX\|var cursorY" "$M2H_NOCURSORX/system/GalBoxLayer.tjs"; then
    echo "[tjs2] M2e-24 NOCURSORX 夹具里 cursorX/cursorY 还在"
    exit 1
  fi
  # M2e-17 的 var cursor 是不同成员：keep 用带初值的完整模式防子串误配。
  for keep in "var cursor = crDefault" "var showParentHint" "function colorRect" "function fillRect" "var imageModified" "function setSize(" "var focusable" "var name" "var face" "function setSizeToImageSize"; do
    if ! grep -q "$keep" "$M2H_NOCURSORX/system/GalBoxLayer.tjs"; then
      echo "[tjs2] M2e-24 NOCURSORX 夹具丢了无关成员：$keep"
      exit 1
    fi
  done

  # M2e-25 负向对照：文件级删除（同 M2e-7/11/12 模式）——不放 GalBoxSound.tjs。
  # 必须复现 `Member "MIDISoundBuffer" does not exist` @ BGM.tjs:114
  M2H_NOMIDI="$OUT/m2h_nomidi"
  rm -rf "$M2H_NOMIDI"
  cp -R "$M2H_GAME" "$M2H_NOMIDI"
  rm -f "$M2H_NOMIDI/system/GalBoxSound.tjs"
  if [ -f "$M2H_NOMIDI/system/GalBoxSound.tjs" ]; then
    echo "[tjs2] M2e-25 NOMIDI 夹具里不该有 GalBoxSound.tjs"
    exit 1
  fi

  # M2e-26 负向对照：段级删除（文件保留）——只删 M2E26 段。
  # 必须复现 `Member "WaveSoundBuffer" does not exist` @ SE.tjs:4
  M2H_NOWAVE="$OUT/m2h_nowave"
  rm -rf "$M2H_NOWAVE"
  cp -R "$M2H_GAME" "$M2H_NOWAVE"
  drop_marked "$M2H_NOWAVE/system/GalBoxSound.tjs" M2E26_BEGIN M2E26_END
  if grep -q "M2E26_BEGIN\|class WaveSoundBuffer" "$M2H_NOWAVE/system/GalBoxSound.tjs"; then
    echo "[tjs2] M2e-26 NOWAVE 夹具里 WaveSoundBuffer 还在"
    exit 1
  fi
  if ! grep -q "class MIDISoundBuffer" "$M2H_NOWAVE/system/GalBoxSound.tjs"; then
    echo "[tjs2] M2e-26 NOWAVE 夹具把 MIDISoundBuffer 也删了"
    exit 1
  fi

  # M2e-27 负向对照：文件级删除 —— 不放 GalBoxMovie.tjs。
  # 必须复现 `Member "VideoOverlay" does not exist` @ Movie.tjs:4
  M2H_NOVIDEO="$OUT/m2h_novideo"
  rm -rf "$M2H_NOVIDEO"
  cp -R "$M2H_GAME" "$M2H_NOVIDEO"
  rm -f "$M2H_NOVIDEO/system/GalBoxMovie.tjs"
  if [ -f "$M2H_NOVIDEO/system/GalBoxMovie.tjs" ]; then
    echo "[tjs2] M2e-27 NOVIDEO 夹具里不该有 GalBoxMovie.tjs"
    exit 1
  fi

  # M2e-28 负向对照：只删 Window 骨架的 left/top（段级，文件保留）。
  # 必须复现 `Member "left" does not exist` @ MainWindow.tjs:438
  M2H_NOWINLEFT="$OUT/m2h_nowinleft"
  rm -rf "$M2H_NOWINLEFT"
  cp -R "$M2H_GAME" "$M2H_NOWINLEFT"
  drop_marked "$M2H_NOWINLEFT/system/GalBoxWindow.tjs" M2E28_BEGIN M2E28_END
  if grep -q "M2E28_BEGIN\|var left = 0;\|var top = 0;" "$M2H_NOWINLEFT/system/GalBoxWindow.tjs"; then
    echo "[tjs2] M2e-28 NOWINLEFT 夹具里 Window.left/top 还在"
    exit 1
  fi
  if ! grep -q "M2E8_STYLE_BEGIN" "$M2H_NOWINLEFT/system/GalBoxWindow.tjs"; then
    echo "[tjs2] M2e-28 NOWINLEFT 夹具把 M2e-8 的 style 段也删了"
    exit 1
  fi

  # M2e-29 负向对照：只删两个音频类的 paused（段级 ×2，文件保留）。
  # 必须复现 `Member "paused" does not exist` @ BGM.tjs:585
  M2H_NOPAUSED="$OUT/m2h_nopaused"
  rm -rf "$M2H_NOPAUSED"
  cp -R "$M2H_GAME" "$M2H_NOPAUSED"
  drop_marked "$M2H_NOPAUSED/system/GalBoxSound.tjs" M2E29A_BEGIN M2E29A_END
  drop_marked "$M2H_NOPAUSED/system/GalBoxSound.tjs" M2E29B_BEGIN M2E29B_END
  if grep -q "M2E29A_BEGIN\|M2E29B_BEGIN\|var paused" "$M2H_NOPAUSED/system/GalBoxSound.tjs"; then
    echo "[tjs2] M2e-29 NOPAUSED 夹具里 paused 还在"
    exit 1
  fi
  for keep in "class MIDISoundBuffer" "class WaveSoundBuffer" "var owner"; do
    if ! grep -q "$keep" "$M2H_NOPAUSED/system/GalBoxSound.tjs"; then
      echo "[tjs2] M2e-29 NOPAUSED 夹具丢了无关成员：$keep"
      exit 1
    fi
  done

  # M2e-30 负向对照：只删 WaveSoundBuffer 的 pan（段级，文件保留）。
  # 不写死期望行号（见 M2e-28/29 教训：缺成员读返回 void，分支静默跳过）。
  M2H_NOPAN="$OUT/m2h_nopan"
  rm -rf "$M2H_NOPAN"
  cp -R "$M2H_GAME" "$M2H_NOPAN"
  drop_marked "$M2H_NOPAN/system/GalBoxSound.tjs" M2E30_BEGIN M2E30_END
  if grep -q "M2E30_BEGIN\|property pan" "$M2H_NOPAN/system/GalBoxSound.tjs"; then
    echo "[tjs2] M2e-30 NOPAN 夹具里 pan 还在"
    exit 1
  fi
  for keep in "class WaveSoundBuffer" "var paused = false;"; do
    if ! grep -q "$keep" "$M2H_NOPAN/system/GalBoxSound.tjs"; then
      echo "[tjs2] M2e-30 NOPAN 夹具丢了无关成员：$keep"
      exit 1
    fi
  done

  # M2e-31 负向对照：只删 VideoOverlay 的 mode（段级，文件/类本体保留）。
  M2H_NOMODE="$OUT/m2h_nomode"
  rm -rf "$M2H_NOMODE"
  cp -R "$M2H_GAME" "$M2H_NOMODE"
  drop_marked "$M2H_NOMODE/system/GalBoxMovie.tjs" M2E31_BEGIN M2E31_END
  if grep -q "M2E31_BEGIN\|var mode = vomLayer" "$M2H_NOMODE/system/GalBoxMovie.tjs"; then
    echo "[tjs2] M2e-31 NOMODE 夹具里 mode 还在"
    exit 1
  fi
  if ! grep -q "class VideoOverlay" "$M2H_NOMODE/system/GalBoxMovie.tjs"; then
    echo "[tjs2] M2e-31 NOMODE 夹具把 VideoOverlay 类也删了"
    exit 1
  fi

  # M2e-33 负向对照：只删 VideoOverlay 的 visible（段级，mode 必须保留）。
  M2H_NOVISIBLE="$OUT/m2h_novisible"
  rm -rf "$M2H_NOVISIBLE"
  cp -R "$M2H_GAME" "$M2H_NOVISIBLE"
  drop_marked "$M2H_NOVISIBLE/system/GalBoxMovie.tjs" M2E33_BEGIN M2E33_END
  if grep -q "M2E33_BEGIN\|var visible = true;" "$M2H_NOVISIBLE/system/GalBoxMovie.tjs"; then
    echo "[tjs2] M2e-33 NOVISIBLE 夹具里 visible 还在"
    exit 1
  fi
  if ! grep -q "var mode = vomLayer;" "$M2H_NOVISIBLE/system/GalBoxMovie.tjs"; then
    echo "[tjs2] M2e-33 NOVISIBLE 夹具把 mode 也删了（变量不唯一）"
    exit 1
  fi

  # M2e-34 负向对照：只删 VideoOverlay 的 loop（段级，visible 必须保留）。
  M2H_NOLOOP="$OUT/m2h_noloop"
  rm -rf "$M2H_NOLOOP"
  cp -R "$M2H_GAME" "$M2H_NOLOOP"
  drop_marked "$M2H_NOLOOP/system/GalBoxMovie.tjs" M2E34_BEGIN M2E34_END
  if grep -q "M2E34_BEGIN\|var loop = false;" "$M2H_NOLOOP/system/GalBoxMovie.tjs"; then
    echo "[tjs2] M2e-34 NOLOOP 夹具里 loop 还在"
    exit 1
  fi
  if ! grep -q "var visible = true;" "$M2H_NOLOOP/system/GalBoxMovie.tjs"; then
    echo "[tjs2] M2e-34 NOLOOP 夹具把 visible 也删了（变量不唯一）"
    exit 1
  fi

  # M2e-35 负向对照：只删 VideoOverlay 的 frame（段级，loop 必须保留）。
  M2H_NOFRAME="$OUT/m2h_noframe"
  rm -rf "$M2H_NOFRAME"
  cp -R "$M2H_GAME" "$M2H_NOFRAME"
  drop_marked "$M2H_NOFRAME/system/GalBoxMovie.tjs" M2E35_BEGIN M2E35_END
  if grep -q "M2E35_BEGIN\|var frame = 0;" "$M2H_NOFRAME/system/GalBoxMovie.tjs"; then
    echo "[tjs2] M2e-35 NOFRAME 夹具里 frame 还在"
    exit 1
  fi
  if ! grep -q "var loop = false;" "$M2H_NOFRAME/system/GalBoxMovie.tjs"; then
    echo "[tjs2] M2e-35 NOFRAME 夹具把 loop 也删了（变量不唯一）"
    exit 1
  fi

  # M2e-36 负向对照：只删 VideoOverlay 的 numberOfAudioStream（段级，frame 必须保留）。
  M2H_NOAUDIO="$OUT/m2h_noaudio"
  rm -rf "$M2H_NOAUDIO"
  cp -R "$M2H_GAME" "$M2H_NOAUDIO"
  drop_marked "$M2H_NOAUDIO/system/GalBoxMovie.tjs" M2E36_BEGIN M2E36_END
  if grep -q "M2E36_BEGIN\|var numberOfAudioStream = 0;" "$M2H_NOAUDIO/system/GalBoxMovie.tjs"; then
    echo "[tjs2] M2e-36 NOAUDIOSTREAM 夹具里 numberOfAudioStream 还在"
    exit 1
  fi
  if ! grep -q "var frame = 0;" "$M2H_NOAUDIO/system/GalBoxMovie.tjs"; then
    echo "[tjs2] M2e-36 NOAUDIOSTREAM 夹具把 frame 也删了（变量不唯一）"
    exit 1
  fi

  # M2e-37 负向对照：只删 VideoOverlay 的 segmentLoopStartFrame（段级，nAudio 必须保留）。
  M2H_NOSEG="$OUT/m2h_noseg"
  rm -rf "$M2H_NOSEG"
  cp -R "$M2H_GAME" "$M2H_NOSEG"
  drop_marked "$M2H_NOSEG/system/GalBoxMovie.tjs" M2E37_BEGIN M2E37_END
  if grep -q "M2E37_BEGIN\|var segmentLoopStartFrame = 0;" "$M2H_NOSEG/system/GalBoxMovie.tjs"; then
    echo "[tjs2] M2e-37 NOSEGSTART 夹具里 segmentLoopStartFrame 还在"
    exit 1
  fi
  if ! grep -q "var numberOfAudioStream = 0;" "$M2H_NOSEG/system/GalBoxMovie.tjs"; then
    echo "[tjs2] M2e-37 NOSEGSTART 夹具把 numberOfAudioStream 也删了（变量不唯一）"
    exit 1
  fi

  # M2e-38 负向对照：只删 VideoOverlay 的 segmentLoopEndFrame（段级，start 必须保留）。
  M2H_NOSEGEND="$OUT/m2h_nosegend"
  rm -rf "$M2H_NOSEGEND"
  cp -R "$M2H_GAME" "$M2H_NOSEGEND"
  drop_marked "$M2H_NOSEGEND/system/GalBoxMovie.tjs" M2E38_BEGIN M2E38_END
  if grep -q "M2E38_BEGIN\|var segmentLoopEndFrame = 0;" "$M2H_NOSEGEND/system/GalBoxMovie.tjs"; then
    echo "[tjs2] M2e-38 NOSEGEND 夹具里 segmentLoopEndFrame 还在"
    exit 1
  fi
  if ! grep -q "var segmentLoopStartFrame = 0;" "$M2H_NOSEGEND/system/GalBoxMovie.tjs"; then
    echo "[tjs2] M2e-38 NOSEGEND 夹具把 segmentLoopStartFrame 也删了（变量不唯一）"
    exit 1
  fi

  # M2e-39 负向对照：只删 VideoOverlay 的 periodEventFrame（段级，segEnd 必须保留）。
  M2H_NOPERIOD="$OUT/m2h_noperiod"
  rm -rf "$M2H_NOPERIOD"
  cp -R "$M2H_GAME" "$M2H_NOPERIOD"
  drop_marked "$M2H_NOPERIOD/system/GalBoxMovie.tjs" M2E39_BEGIN M2E39_END
  if grep -q "M2E39_BEGIN\|var periodEventFrame = 0;" "$M2H_NOPERIOD/system/GalBoxMovie.tjs"; then
    echo "[tjs2] M2e-39 NOPERIOD 夹具里 periodEventFrame 还在"
    exit 1
  fi
  if ! grep -q "var segmentLoopEndFrame = 0;" "$M2H_NOPERIOD/system/GalBoxMovie.tjs"; then
    echo "[tjs2] M2e-39 NOPERIOD 夹具把 segmentLoopEndFrame 也删了（变量不唯一）"
    exit 1
  fi

  # M2e-40 负向对照：只删 KAGParser 的 resetInterrupt()（段级，其他方法必须保留）。
  M2H_NORESINT="$OUT/m2h_noresint"
  rm -rf "$M2H_NORESINT"
  cp -R "$M2H_GAME" "$M2H_NORESINT"
  drop_marked "$M2H_NORESINT/system/GalBoxKAGParser.tjs" M2E40_BEGIN M2E40_END
  if grep -q "M2E40_BEGIN\|function resetInterrupt()" "$M2H_NORESINT/system/GalBoxKAGParser.tjs"; then
    echo "[tjs2] M2e-40 NORESINT 夹具里 resetInterrupt 还在"
    exit 1
  fi
  if ! grep -q "function goToLabel()" "$M2H_NORESINT/system/GalBoxKAGParser.tjs"; then
    echo "[tjs2] M2e-40 NORESINT 夹具把 goToLabel 也删了（变量不唯一）"
    exit 1
  fi

  # M2e-41 负向对照：只删 KAGParser 的 curStorage 成员（段级，其他成员必须保留）。
  # ⚠️ 不能像别的夹具那样查"文件里还有没有 curStorage 字样" —— M2E41 段里
  # 本身就有大段注释提到 curStorage，drop_marked 删整段后字样会一并消失，
  # 这里改查 M2E41 段标记 + var 声明两种形态都清干净。
  #
  # 🔴 **为什么还要额外删 M2E43 段（getNextTag）**：
  # curStorage 只在官方 Conductor.tjs:180 的 **catch 块**里被读。而 catch 块
  # 只有在 try 块抛异常时才进入 —— try 块里唯一的异常源就是 81 行的
  # `obj = getNextTag();`。M2e-43 补上 getNextTag() 后它返回 void，
  # 官方 91 行 `obj === void` 成立 ⇒ 走「シナリオ終了」分支 ⇒ **不进 catch 块**
  # ⇒ curStorage 永远不被读 ⇒ 只删 curStorage 反而链条畅通（这个对照失效）。
  # M2e-41/42 当年能复现缺口，正是因为当时基线骨架里也还没有 getNextTag。
  # ⇒ 这里额外删掉 M2E43 段，强制链条回到「进 catch 块」的历史路径，
  # 被测变量仍是 curStorage（getNextTag 只是制造入口的手段）。
  M2H_NOCURSTOR="$OUT/m2h_nocurstor"
  rm -rf "$M2H_NOCURSTOR"
  cp -R "$M2H_GAME" "$M2H_NOCURSTOR"
  drop_marked "$M2H_NOCURSTOR/system/GalBoxKAGParser.tjs" M2E41_BEGIN M2E41_END
  drop_marked "$M2H_NOCURSTOR/system/GalBoxKAGParser.tjs" M2E43_BEGIN M2E43_END
  if grep -q "M2E41_BEGIN\|var curStorage" "$M2H_NOCURSTOR/system/GalBoxKAGParser.tjs"; then
    echo "[tjs2] M2e-41 NOCURSTOR 夹具里 curStorage 还在"
    exit 1
  fi
  if grep -q "M2E43_BEGIN\|function getNextTag" "$M2H_NOCURSTOR/system/GalBoxKAGParser.tjs"; then
    echo "[tjs2] M2e-41 NOCURSTOR 夹具里 getNextTag 还在（链条不会进 catch 块，对照失效）"
    exit 1
  fi
  if ! grep -q "function goToLabel()" "$M2H_NOCURSTOR/system/GalBoxKAGParser.tjs"; then
    echo "[tjs2] M2e-41 NOCURSTOR 夹具把 goToLabel 也删了（变量不唯一）"
    exit 1
  fi
  if ! grep -q "function resetInterrupt()" "$M2H_NOCURSTOR/system/GalBoxKAGParser.tjs"; then
    echo "[tjs2] M2e-41 NOCURSTOR 夹具把 resetInterrupt 也删了（变量不唯一）"
    exit 1
  fi

  # M2e-42 负向对照：只删 KAGParser 的 curLine 成员（段级，其他成员必须保留）。
  # curLine 与 curStorage 在官方脚本里是同一行（Conductor.tjs:180）的两个读，
  # TJS2 拼接从左到右求值 ⇒ 本夹具必须保留 curStorage，否则先撞上的是它。
  # 🔴 同样要额外删 M2E43 段（getNextTag）—— 理由同 NOCURSTOR：不删它，
  # 链条走「シナリオ終了」分支就不进 catch 块，读不到 curLine，对照失效。
  M2H_NOCURLINE="$OUT/m2h_nocurline"
  rm -rf "$M2H_NOCURLINE"
  cp -R "$M2H_GAME" "$M2H_NOCURLINE"
  drop_marked "$M2H_NOCURLINE/system/GalBoxKAGParser.tjs" M2E42_BEGIN M2E42_END
  drop_marked "$M2H_NOCURLINE/system/GalBoxKAGParser.tjs" M2E43_BEGIN M2E43_END
  if grep -q "M2E42_BEGIN\|var curLine" "$M2H_NOCURLINE/system/GalBoxKAGParser.tjs"; then
    echo "[tjs2] M2e-42 NOCURLINE 夹具里 curLine 还在"
    exit 1
  fi
  if grep -q "M2E43_BEGIN\|function getNextTag" "$M2H_NOCURLINE/system/GalBoxKAGParser.tjs"; then
    echo "[tjs2] M2e-42 NOCURLINE 夹具里 getNextTag 还在（链条不会进 catch 块，对照失效）"
    exit 1
  fi
  if ! grep -q "var curStorage" "$M2H_NOCURLINE/system/GalBoxKAGParser.tjs"; then
    echo "[tjs2] M2e-42 NOCURLINE 夹具把 curStorage 也删了（变量不唯一 / 同行求值顺序会先撞它）"
    exit 1
  fi
  if ! grep -q "function goToLabel()" "$M2H_NOCURLINE/system/GalBoxKAGParser.tjs"; then
    echo "[tjs2] M2e-42 NOCURLINE 夹具把 goToLabel 也删了（变量不唯一）"
    exit 1
  fi
  if ! grep -q "function resetInterrupt()" "$M2H_NOCURLINE/system/GalBoxKAGParser.tjs"; then
    echo "[tjs2] M2e-42 NOCURLINE 夹具把 resetInterrupt 也删了（变量不唯一）"
    exit 1
  fi

  # M2e-43 负向对照：只删 KAGParser 的 getNextTag() 方法（段级，其他成员必须保留）。
  # ⚠️ 不能查裸字样 "getNextTag" —— M2E43 段里本身有大段注释提到它，
  # drop_marked 删整段后字样会一并消失，这里查段标记 + function 声明两种形态。
  M2H_NOGETNEXT="$OUT/m2h_nogetnext"
  rm -rf "$M2H_NOGETNEXT"
  cp -R "$M2H_GAME" "$M2H_NOGETNEXT"
  drop_marked "$M2H_NOGETNEXT/system/GalBoxKAGParser.tjs" M2E43_BEGIN M2E43_END
  if grep -q "M2E43_BEGIN\|function getNextTag" "$M2H_NOGETNEXT/system/GalBoxKAGParser.tjs"; then
    echo "[tjs2] M2e-43 NOGETNEXT 夹具里 getNextTag 还在"
    exit 1
  fi
  if ! grep -q "var curStorage" "$M2H_NOGETNEXT/system/GalBoxKAGParser.tjs"; then
    echo "[tjs2] M2e-43 NOGETNEXT 夹具把 curStorage 也删了（变量不唯一）"
    exit 1
  fi
  if ! grep -q "var curLine" "$M2H_NOGETNEXT/system/GalBoxKAGParser.tjs"; then
    echo "[tjs2] M2e-43 NOGETNEXT 夹具把 curLine 也删了（变量不唯一）"
    exit 1
  fi
  if ! grep -q "function goToLabel()" "$M2H_NOGETNEXT/system/GalBoxKAGParser.tjs"; then
    echo "[tjs2] M2e-43 NOGETNEXT 夹具把 goToLabel 也删了（变量不唯一）"
    exit 1
  fi
  if ! grep -q "function resetInterrupt()" "$M2H_NOGETNEXT/system/GalBoxKAGParser.tjs"; then
    echo "[tjs2] M2e-43 NOGETNEXT 夹具把 resetInterrupt 也删了（变量不唯一）"
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

  # ---- M2e-12b：把这一份夹具原样打成设备可读的内置包 -------------------
  # 设备端只能读 demo-game.xp3，自测跑的是目录虚拟归档。两边各维护一份
  # 文件清单必然漂移 —— M2e-2~M2e-12 的骨架正是只进了自测、没进包。这里用
  # **同一份** $M2H_GAME 现场打包，内置包与自测夹具从此同源，不再手工列举。
  GALBOX_KAG_XP3="$ROOT/dist/fixtures/galbox-kag.xp3"
  mkdir -p "$ROOT/dist/fixtures"
  # 夹具只有 system/ 与 startup.tjs；设备上还要有能播的剧本与图，否则 app
  # 打开后会落到「包内没有可运行的 .ks 剧本」的兜底分支。这两份资产直接
  # 从 demo-game.xp3 里取，不在这里复制一份正文（避免两份真相）。
  if [ -f "$ROOT/dist/fixtures/demo-game.xp3" ]; then
    python3 - "$ROOT/dist/fixtures/demo-game.xp3" "$M2H_GAME" "$ROOT/tools" <<'PYEOF' || exit 1
import os
import sys
sys.path.insert(0, sys.argv[3])
import xp3_fixture as X
p = X.parse_xp3(open(sys.argv[1], 'rb').read(), collect_data=True)
want = ('scenario/first.ks', 'bg/room.png', 'fg/sakura.png')
taken = []
for e in p['entries']:
    if e['name'] in want:
        dst = os.path.join(sys.argv[2], e['name'])
        d = os.path.dirname(dst)
        if d:
            os.makedirs(d, exist_ok=True)
        with open(dst, 'wb') as f:
            f.write(e['data'])
        taken.append(e['name'])
missing = [n for n in want if n not in taken]
if missing:
    print('[tjs2] demo-game.xp3 里缺资产:', missing)
    sys.exit(1)
print('[tjs2] 从 demo-game.xp3 取到设备端资产:', ', '.join(taken))
PYEOF
  else
    echo "[tjs2] 警告：没有 demo-game.xp3，内置包将不含剧本/图片（app 会落兜底分支）"
  fi
  python3 "$ROOT/tools/xp3_fixture.py" --pack "$M2H_GAME" "$GALBOX_KAG_XP3" || exit 1
  python3 - "$GALBOX_KAG_XP3" "$ROOT/tools" <<'PYEOF' || exit 1
import sys
sys.path.insert(0, sys.argv[2])
import xp3_fixture as X
p = X.parse_xp3(open(sys.argv[1], 'rb').read())
names = set(e['name'] for e in p['entries'])
req = ['startup.tjs', 'system/Initialize.tjs', 'system/Config.tjs']
req += ['system/GalBox%s.tjs' % s for s in (
    'Consts', 'Font', 'Layer', 'Window', 'Pad', 'Menu', 'Timer',
    'KAGParser', 'AsyncTrigger')]
missing = [n for n in req if n not in names]
if missing:
    print('[tjs2] 内置 KAG 包缺文件:', missing)
    sys.exit(1)
if p['count'] < 30:
    print('[tjs2] 内置 KAG 包文件数过少:', p['count'])
    sys.exit(1)
init = next(e for e in p['entries'] if e['name'] == 'system/Initialize.tjs')
if init['orgSize'] < 1000:
    print('[tjs2] system/Initialize.tjs 只有 %d B，像是探针版而不是官方原文'
          % init['orgSize'])
    sys.exit(1)
print('[tjs2] 内置 KAG 包: %d 个文件, Initialize.tjs %d B, 九个骨干全在'
      % (p['count'], init['orgSize']))
PYEOF

  echo "[tjs2] M2e-5: 编译并运行自测（官方 Config.tjs / 二次执行诊断 / 负向对照）"
  if clang++ -std=c++14 -O1 -g -fexceptions -DTJS2_POSIX \
        -I"$STUB" -I"$SRC" -I"$HOST" -I"$ROOT/native/Engine" -include "$COMPAT" \
        "$HOST/test_m2h.cpp" "$OUT/HostBridge.o" "$OUT/HostStreams.o" \
        "$OUT/XP3Archive.o" "$OUT/libtjs2.a" \
        -lz $ICONV_LIB -lpthread -o "$OUT/tjs2test_m2h" 2>"$OUT/link_m2h.log"; then
    # 每个场景**独立进程**跑：run #40 出现过「同进程内第二次执行会破坏官方
    # 启动链」, 若三个场景挤在一个进程里, 后面的场景会被前面的污染,
    # 负向对照就会假红。独立进程 => 每个场景都是干净首跑。
    # M2e-32：NOEVALSTORAGE 的夹具与 A 完全相同（evalStorage 是 C++ 原生方法，
    # 不在 TJS 骨架文件里），变量是宿主注册开关，由场景函数在进程内切换。
    for M2H_CASE in A B NOMENU NOSTYLE NOTIMER NOFONT NOKAGPARSER NOASYNCTRIGGER NODEBUGLEVEL NOHITTYPE NOHITTHRESHOLD NOIMAGESIZE NOSIZETOIMAGESIZE NODRAWFACE NOFILLRECT NOCURSOR NONAME NOFOCUSABLE NOSIZE NOIMAGEMODIFIED NOCOLORRECT NOSHOWPARENTHINT NOCURSORX NOMIDI NOWAVE NOVIDEO NOWINLEFT NOPAUSED NOPAN NOMODE NOEVALSTORAGE NOVISIBLE NOLOOP NOFRAME NOAUDIOSTREAM NOSEGSTART NOSEGEND NOPERIOD NORESINT NOCURSTOR NOCURLINE NOGETNEXT XP3 LEAK LEAK2; do
      OUTPUT_M2H=$("$OUT/tjs2test_m2h" "$M2H_CASE" "$M2H_GAME" "$M2H_CTRL" "$OUT" "$M2H_NOMENU" "$M2H_NOSTYLE" "$M2H_NOTIMER" "$M2H_NOFONT" "$M2H_NOKAGPARSER" "$M2H_NOASYNCTRIGGER" "$GALBOX_KAG_XP3" "$M2H_NODEBUGLEVEL" "$M2H_NOHITTYPE" "$M2H_NOHITTHRESHOLD" "$M2H_NOIMAGESIZE" "$M2H_NOSIZETOIMAGESIZE" "$M2H_NODRAWFACE" "$M2H_NOFILLRECT" "$M2H_NOCURSOR" "$M2H_NONAME" "$M2H_NOFOCUSABLE" "$M2H_NOSIZE" "$M2H_NOIMAGEMODIFIED" "$M2H_NOCOLORRECT" "$M2H_NOSHOWPARENTHINT" "$M2H_NOCURSORX" "$M2H_NOMIDI" "$M2H_NOWAVE" "$M2H_NOVIDEO" "$M2H_NOWINLEFT" "$M2H_NOPAUSED" "$M2H_NOPAN" "$M2H_NOMODE" "$M2H_NOVISIBLE" "$M2H_NOLOOP" "$M2H_NOFRAME" "$M2H_NOAUDIO" "$M2H_NOSEG" "$M2H_NOSEGEND" "$M2H_NOPERIOD" "$M2H_NORESINT" "$M2H_NOCURSTOR" "$M2H_NOCURLINE" "$M2H_NOGETNEXT" 2>&1)
      diag_crash "M2h-$M2H_CASE" "$?" "$OUT/tjs2test_m2h" "$M2H_CASE" "$M2H_GAME" "$M2H_CTRL" "$OUT" "$M2H_NOMENU" "$M2H_NOSTYLE" "$M2H_NOTIMER" "$M2H_NOFONT" "$M2H_NOKAGPARSER" "$M2H_NOASYNCTRIGGER" "$GALBOX_KAG_XP3" "$M2H_NODEBUGLEVEL" "$M2H_NOHITTYPE" "$M2H_NOHITTHRESHOLD" "$M2H_NOIMAGESIZE" "$M2H_NOSIZETOIMAGESIZE" "$M2H_NODRAWFACE" "$M2H_NOFILLRECT" "$M2H_NOCURSOR" "$M2H_NONAME" "$M2H_NOFOCUSABLE" "$M2H_NOSIZE" "$M2H_NOIMAGEMODIFIED" "$M2H_NOCOLORRECT" "$M2H_NOSHOWPARENTHINT" "$M2H_NOCURSORX" "$M2H_NOMIDI" "$M2H_NOWAVE" "$M2H_NOVIDEO" "$M2H_NOWINLEFT" "$M2H_NOPAUSED" "$M2H_NOPAN" "$M2H_NOMODE" "$M2H_NOVISIBLE" "$M2H_NOLOOP" "$M2H_NOFRAME" "$M2H_NOAUDIO" "$M2H_NOSEG" "$M2H_NOSEGEND" "$M2H_NOPERIOD" "$M2H_NORESINT" "$M2H_NOCURSTOR" "$M2H_NOCURLINE" "$M2H_NOGETNEXT"
      echo "$OUTPUT_M2H" | sed 's/^/[tjs2]   /'
      case "$M2H_CASE" in
        A|B|NOMENU|NOSTYLE|NOTIMER|NOFONT|NOKAGPARSER|NOASYNCTRIGGER|NODEBUGLEVEL|NOHITTYPE|NOHITTHRESHOLD|NOIMAGESIZE|NOSIZETOIMAGESIZE|NODRAWFACE|NOFILLRECT|NOCURSOR|NONAME|NOFOCUSABLE|NOSIZE|NOIMAGEMODIFIED|NOCOLORRECT|NOSHOWPARENTHINT|NOCURSORX|NOMIDI|NOWAVE|NOVIDEO|NOWINLEFT|NOPAUSED|NOPAN|NOMODE|NOEVALSTORAGE|NOVISIBLE|NOLOOP|NOFRAME|NOAUDIOSTREAM|NOSEGSTART|NOSEGEND|NOPERIOD|NORESINT|NOCURSTOR|NOCURLINE|NOGETNEXT|XP3)
          if ! echo "$OUTPUT_M2H" | grep -q "M2H_RESULT=OK"; then
            echo "[tjs2] 场景 $M2H_CASE 未通过"
            # M2e-32 诊断开关：一次跑完全部场景收集实测停点（判据校准用）。
            # 默认关闭 —— 正常 CI 在第一个失败处退出；需要一次性收集所有
            # 负向对照的实测停点时，用 M2H_COLLECT_ALL=1 bash tools/... 调用。
            if [ "${M2H_COLLECT_ALL:-0}" = "1" ]; then
              M2H_FAILED="$M2H_FAILED $M2H_CASE"
            else
              exit 1
            fi
          fi
          ;;
        *)
          # LEAK / LEAK2 只是诊断, 不参与判定(它们的结论写在 verdict 里)
          ;;
      esac
    done
    if [ -n "${M2H_FAILED:-}" ]; then
      echo "[tjs2] M2H_COLLECT_ALL: 失败场景:$M2H_FAILED"
      exit 1
    fi
    echo "[tjs2] M2e-5 自测通过（官方 Config.tjs 消掉 KAGWindow_config 缺口 + 负向对照）"
    echo "[tjs2] M2e-13 自测通过（debugLevel 只写不读的槽位补齐 + NODEBUGLEVEL 负向对照）"
    echo "[tjs2] M2e-14 自测通过（Layer 命中面 hitType/hitThreshold + 两个独立负向对照）"
    echo "[tjs2] M2e-15 自测通过（Layer 图像面 setImageSize/setSizeToImageSize + 两个独立负向对照）"
    echo "[tjs2] M2e-16 自测通过（Layer 绘制面 face/fillRect + 两个独立负向对照）"
    echo "[tjs2] M2e-17 自测通过（Layer 光标面 cursor + NOCURSOR 独立负向对照）"
    echo "[tjs2] M2e-18 自测通过（Layer 图层名 name + NONAME 独立负向对照）"
    echo "[tjs2] M2e-19 自测通过（Layer 焦点接收面 focusable + NOFOCUSABLE 独立负向对照）"
    echo "[tjs2] M2e-20 自测通过（Layer 显示尺寸 setSize + NOSIZE 独立负向对照）"
    echo "[tjs2] M2e-21 自测通过（Layer 图像脏标记 imageModified + NOIMAGEMODIFIED 独立负向对照）"
    echo "[tjs2] M2e-22 自测通过（Layer 颜色矩形填充 colorRect + NOCOLORRECT 独立负向对照）"
    echo "[tjs2] M2e-23 自测通过（Layer 父层提示开关 showParentHint + NOSHOWPARENTHINT 独立负向对照）"
    echo "[tjs2] M2e-24 自测通过（Layer 层内鼠标坐标 cursorX/cursorY + NOCURSORX 独立负向对照）"
    echo "[tjs2] M2e-25 自测通过（MIDISoundBuffer 音频类骨架 + NOMIDI 独立负向对照）"
    echo "[tjs2] M2e-26 自测通过（WaveSoundBuffer 音频类骨架 + NOWAVE 独立负向对照）"
    echo "[tjs2] M2e-27 自测通过（VideoOverlay 影片类骨架 + NOVIDEO 独立负向对照）"
    echo "[tjs2] M2e-28 自测通过（Window 坐标 left/top + NOWINLEFT 独立负向对照）"
    echo "[tjs2] M2e-29 自测通过（音频缓冲区 paused ×2 + NOPAUSED 独立负向对照）"
    echo "[tjs2] M2e-30 自测通过（WaveSoundBuffer 声像 pan + NOPAN 独立负向对照）"
    echo "[tjs2] M2e-31 自测通过（VideoOverlay 播放模式 mode + NOMODE 独立负向对照）"
    echo "[tjs2] M2e-32 自测通过（Scripts.evalStorage 原生方法 + NOEVALSTORAGE 宿主开关负向对照）"
    echo "[tjs2] M2e-33 自测通过（VideoOverlay 可见标志 visible + NOVISIBLE 独立负向对照）"
    echo "[tjs2] M2e-34 自测通过（VideoOverlay 循环标志 loop + NOLOOP 独立负向对照）"
    echo "[tjs2] M2e-35 自测通过（VideoOverlay 当前帧号 frame + NOFRAME 独立负向对照）"
    echo "[tjs2] M2e-36 自测通过（VideoOverlay 音频流数 numberOfAudioStream + NOAUDIOSTREAM 独立负向对照）"
    echo "[tjs2] M2e-37 自测通过（VideoOverlay 分段循环起点 segmentLoopStartFrame + NOSEGSTART 独立负向对照）"
    echo "[tjs2] M2e-38 自测通过（VideoOverlay 分段循环终点 segmentLoopEndFrame + NOSEGEND 独立负向对照）"
    echo "[tjs2] M2e-39 自测通过（VideoOverlay 周期事件帧 periodEventFrame + NOPERIOD 独立负向对照）"
    echo "[tjs2] M2e-43 自测通过（KAGParser 取下一个标签 getNextTag + NOGETNEXT 独立负向对照）"
    echo "[tjs2] M2e-12b 自测通过（内置 xp3 与目录夹具跑出同一条 trace、同一停点）"
  else
    echo "[tjs2] M2e-5 自测链接失败:"; cat "$OUT/link_m2h.log"; exit 1
  fi
fi

exit 0
