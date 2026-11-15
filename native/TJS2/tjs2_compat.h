// tjs2_compat.h — 官方 TJS2 核心的 POSIX 移植兼容层（Milestone 0 临时 shim）
//
// 由 build_tjs2_test.sh 通过 `clang++ -include tjs2_compat.h` 注入，在 macOS/iOS
// clang 下消掉 tjs2 核心里少量 Windows 专属符号，使其能在 POSIX 下编译。
//
// M0 规避策略（不改官方源码）：
//   - boost regex  : 定义 TJS_NO_REGEXP，排除 tjsRegExp.cpp（官方原生开关）
//   - Windows Dbg  : 不定义 ENABLE_DEBUGGER，debugger.h 代码自动跳过
//   - vcl.h        : 不定义 TJS_SUPPORT_VCL
//   - windows.h    : 不定义 __WIN32__
//   - <malloc.h>   : macOS SDK 无此头；本 shim 重定向到 <stdlib.h>
#ifndef TJS2_COMPAT_H
#define TJS2_COMPAT_H

#include <cstdio>
#include <cstdint>
#include <cstdarg>
#include <stdlib.h>

// 1) macOS 无 <malloc.h>（free/alloca 语义在 <stdlib.h> / <alloca.h>）。
//    tjsCommHead.h 与 bison 生成的 *.tab.cpp 都硬 #include <malloc.h>。
//    用一个空的本地头拦截；free/malloc/realloc 已由 <stdlib.h> 提供。
//    实际拦截由 stub/malloc.h 完成（构建脚本把它加在 -I 最前）；此处仅兜底 define。

// 2) 调试输出：Windows OutputDebugString → 无操作
#ifdef OutputDebugString
#  undef OutputDebugString
#endif
#define OutputDebugString(...)
#define OutputDebugStringA(...)
#define OutputDebugStringW(...)

// 3) tjs_char 由 tjsTypes.h 决定：macOS/iOS clang 下 typedef 为 wchar_t(4B)，
//    TJS_W(X) = L##X，对应宽字符字面量；勿假定是 uint16_t/UTF-16。
// 4) 线程：-lpthread 由构建脚本提供；tjsThread 有 POSIX 分支。

#endif // TJS2_COMPAT_H
