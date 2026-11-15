# TJS2 vendored 源码的 POSIX/clang 补丁

上游：`jeeb/kirikiri2` @ `master`，路径 `kirikiri2/src/core/tjs2/`。
原始 SHA 与字节数见 `VENDOR_MANIFEST.json`（下载时逐文件校验过）。

上游代码面向旧 Borland C++ / GCC，Apple clang（C++14）会拒绝若干写法。
所有修改集中在 `apply_posix_patches.py`（幂等，可重复运行），**不改动语义**，
只做编译期合规。重新 vendoring 上游源码后运行一次该脚本即可复现。

## 编译开关（由 `tools/build_tjs2_test.sh` 提供，非源码改动）

| 开关 | 作用 |
|------|------|
| `TJS_NO_REGEXP` | 官方原生开关。定义后 `tjs.cpp` / `tjsArray.cpp` 不再引用 RegExp 类，从而避开唯一的 `boost/regex.hpp` 依赖；`tjsRegExp.cpp` 从编译列表排除。M3 补真正则时去掉。 |
| 不定义 `ENABLE_DEBUGGER` | `debugger.h` / `Debugger.h` 及所有 Windows `SendMessage`/`COPYDATASTRUCT` 代码自动跳过。 |
| 不定义 `__WIN32__` / `TJS_SUPPORT_VCL` | `windows.h` / `vcl.h` 分支跳过，走 POSIX 空实现。 |
| `stub/malloc.h` | macOS SDK 无 `<malloc.h>`；用 `-I stub` 拦截，映射到 `<stdlib.h>`。 |
| `tjs2_compat.h` | `-include` 注入，处理 `OutputDebugString` 等。 |

## 源码补丁清单（6 类，来自 run #10 的 43 个编译错误）

- **P1 `tjsHashSearch.h`** — `tTJSHashCache`（派生类模板）调用基类模板成员
  `GetCount()` / `ChopLast()` 时，clang 两阶段名字查找要求显式限定。
  改为 `inherited::GetCount()` / `inherited::ChopLast()`，共 6 处。
  *（29 个 error，因该头被多个 .cpp 包含而重复计数）*

- **P2 `tjsArray.cpp` / `tjsDictionary.cpp` / `tjsInterCodeExec.cpp` / `tjsNative.cpp`**
  — `&tTJSVariantClosure(...)` 取临时对象地址是 clang 的硬错误
  （`-Waddress-of-temporary`）。改为具名局部变量 `tTJSVariantClosure _closureN(...);`
  再传 `&_closureN`，共 10 处。跨行调用时声明必须放在整个语句之前，
  脚本用括号深度回溯定位语句首行，不能插进实参列表。

- **P3 `tjsArray.cpp:1065`** — 上游 typo `stream>Write(...)` 应为 `stream.Write(...)`
  （`TJS_TEXT_OUT_CRLF` 未定义分支，故隐蔽至今）。

- **P4 `tjsInterCodeExec.cpp:2625`** — macOS `wcsstr` 返回 `const wchar_t*`，
  赋给 `tjs_char*` 丢 const。加 `const_cast<tjs_char*>`。

- **P5 `tjsInterCodeExec.cpp:2834`** — `const destLength = ...;` 缺类型说明符
  （旧编译器隐式 int）。补 `const tjs_int` 并显式转换。

- **P6 `tjsArray.cpp:917`** — `i - ni->Items.begin()` 得到 `ptrdiff_t`(long)，
  赋给 `tTJSVariant` 时多个 `operator=` 重载歧义。显式 `(tjs_int)` 转换。

## 备注

- `tjsDateWordMap.cc` 被 `tjsDateParser.cpp` 直接 `#include`，**不单独编译**。
- `tjs.char` 在 macOS clang 下是 `wchar_t`(4B)，`TJS_W(X) = L##X`；
  不要按 UTF-16/uint16_t 处理。
- 上游授权为 GPL-2（日文原文见 `LICENSE.txt`），vendoring 保留原文件。
- M0 目标只验证「官方 TJS2 能在 macOS/iOS clang 编译、链接、`eval("1+1")==2`」，
  **不含**正则、调试器、宿主文件系统/渲染接口。后续里程碑见 `docs/TJS2_PORT_PLAN.md`。
