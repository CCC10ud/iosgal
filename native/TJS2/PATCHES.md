# TJS2 vendored 源码的 POSIX/clang 补丁

上游：`jeeb/kirikiri2` @ `master`，路径 `kirikiri2/src/core/tjs2/`。
原始 SHA 与字节数见 `VENDOR_MANIFEST.json`（下载时逐文件校验过）。

上游代码面向旧 Borland C++ / GCC，Apple clang（C++14）会拒绝若干写法。
所有修改集中在 `apply_posix_patches.py`（幂等，可重复运行），**不改动语义**，
只做编译期合规。重新 vendoring 上游源码后运行一次该脚本即可复现。

## 编译开关（由 `tools/build_tjs2_test.sh` 提供，非源码改动）

| 开关 | 作用 |
|------|------|
| `TJS_NO_REGEXP` | 官方原生开关。**M0–M2c 用过**（定义后 `tjs.cpp` / `tjsArray.cpp` 不再引用 RegExp 类，从而避开唯一的 `boost/regex.hpp` 依赖）。**M2d 起已去掉**，改为 P7 的 std::regex 重写版。 |
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

## P7 正则层整体重写（M2d，`tjsRegExp.h` / `tjsRegExp.cpp`）

这两个文件**已不是上游原文**，是 GalBox 用标准库 `std::basic_regex` 重写的整份
替代实现（上游 blob sha 仍在 `VENDOR_MANIFEST.json` 的 `upstream_sha` 字段里，
随时可取回对比）。原因：

- 上游 832 行全部绑在 `boost::reg_expression` / `boost::match_results` /
  `regex_grep` / `std::bind1st(std::mem_fun(...))`，最后一项在 C++11 起已废弃，
  现代 boost 上要改的比写一遍还多；
- 引 boost 意味着 runner 装依赖 + iOS 侧也要能编（`ARCHIVE` 才出的 ipa 里真跑），
  失败面比"自己实现一层"大得多；
- 官方接入点只有三处，都靠**类名/类 ID** 而不是 boost 类型：
  `tjs.cpp:185`（`TJSCreateRegExpClass()` 注册全局 `RegExp`）、
  `tjsArray.cpp:482`（`tTJSNC_RegExp::ClassID` + `tTJSNI_RegExp::Split`）、
  `tjsInterCodeGen.cpp:2476`（正则字面量生成 `new RegExp(); _compile("//flag/body")`）。
  因此**上游其它文件一行不用改**，只是不再定义 `TJS_NO_REGEXP`。

保持一致的对外语义：成员/属性名、`g`（下一轮从上次结束继续）/`i`/`l` 标志、
`lastMatch`/`lastParen`/`leftContext`/`rightContext`/`index`/`lastIndex`/`input`、
`replace` 的「替换串不展开 `$1`」、`split` 的 purgeempty 与子表达式展开顺序。

已知差异（有判据覆盖，见 `native/Host/test_m2d.cpp`）：

| 差异 | 处理 |
|---|---|
| boost 的 `.` 默认**吃换行**，ECMAScript 不吃 | 编译前把字符类外的裸 `.` 翻成 `[\s\S]`，还原上游行为 |
| boost 支持 `\Q...\E` | 未实现（KAG3 系统脚本里未出现） |
| 上游 replace 回调失败时静默截断 | 改为抛 TJS 异常（静默丢字符更难查） |
| 旧 C++ 的 `std::bind1st` | 换成显式循环，`g` 语义与上游回调返回值一致 |

性能/健壮性注意：`std::regex` 是回溯实现，无匹配步数上限；灾难性回溯的正则
（如 `(a+)+$` 打长串）会长时间占 CPU。KAG3 系统脚本全是简单模式，暂不处理；
真遇到再上 `match_step_limit` 或换 RE2 风格引擎。

## 备注

- `tjsDateWordMap.cc` 被 `tjsDateParser.cpp` 直接 `#include`，**不单独编译**。
- `tjs.char` 在 macOS clang 下是 `wchar_t`(4B)，`TJS_W(X) = L##X`；
  不要按 UTF-16/uint16_t 处理。
- 上游授权为 GPL-2（日文原文见 `LICENSE.txt`），vendoring 保留原文件。
- M0 目标只验证「官方 TJS2 能在 macOS/iOS clang 编译、链接、`eval("1+1")==2`」，
  **不含**正则、调试器、宿主文件系统/渲染接口。后续里程碑见 `docs/TJS2_PORT_PLAN.md`。
