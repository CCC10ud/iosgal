#!/usr/bin/env bash
# build_tjs2_test.sh — 编译官方 TJS2 核心为静态库并运行 eval 自测（Milestone 0）
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
    echo "[tjs2] 编译失败: $b"; tail -n 30 "$o.log"; ERR=1
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
    echo "[tjs2] M0 自测链接失败:"; tail -n 40 "$OUT/link.log"; exit 1
  fi
fi
exit 0
