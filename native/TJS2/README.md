# native/TJS2 — 官方 TJS2 引擎 vendoring 落点

**状态：Milestone 0 ✅ / Milestone 1a ✅（以 CI run 为准，见文末）。**

## 来源与授权

- 上游：`jeeb/kirikiri2` @ `master`，目录 `kirikiri2/src/core/tjs2/`
  （早期文档写的 `kikikracle/kirikiri2` 是 404，已更正）。
- vendoring：77 个文件 / 1,269,921 B，通过 Git Data API 按 blob SHA 逐个取回，
  逐文件核对字节数；清单见 `VENDOR_MANIFEST.json`。
- **授权：双许可证** —— 吉里吉里独自许可 **或** GNU GPL，二选一
  （原文见 `LICENSE.txt`）。不是 BSD，也不是纯 GPL-2。
  ⚠️ 若将来要把 GalBox 上架 App Store，GPL 选项与 App Store 条款存在冲突，
  需选择「吉里吉里独自许可」并逐条确认其分发条件，或走开源/侧载分发。
  vendoring 时**必须保留** `LICENSE.txt` 与每个源文件头部的版权注释。

## 目录

| 路径 | 内容 |
|---|---|
| `src/` | 官方 TJS2 核心 77 个文件（未改语义，仅 6 类 POSIX 编译补丁） |
| `stub/malloc.h` | 拦截 macOS/iOS SDK 缺失的 `<malloc.h>` |
| `tjs2_compat.h` | POSIX 兼容注入头（`-include`），Windows 调试输出置空 |
| `apply_posix_patches.py` | 幂等补丁脚本，重放 vendor 后的 clang 编译修复 |
| `PATCHES.md` | 6 类补丁逐条说明（改了官方源码哪些点、为什么） |
| `test_m0.cpp` | M0：`eval("1+1") == 2` |
| `test_m1.cpp` | M1a：宿主流读 XP3 内文件 + `Array.load` 真回调 |
| `build/` | CI 生成的 `.o` / `libtjs2.a` / 可执行自测（不入库） |

宿主实现在 `../Host/HostInterface.hpp`（契约）+ `../Host/HostStreams.cpp`（实现）。

## 编译配置（M0 实测可用）

```
clang++ -std=c++14 -O1 -fexceptions -DTJS_NO_REGEXP -DTJS2_POSIX \
        -I stub -I src -include tjs2_compat.h -c src/*.cpp
ar rcs libtjs2.a *.o
```

- `TJS_NO_REGEXP` 是**官方原生开关**，定义后 `tjs.cpp` 不再引用 RegExp 类，
  只需排除 `tjsRegExp.cpp`（boost regex 唯一入口）；正则功能留到 M3。
- 不定义 `ENABLE_DEBUGGER` / `TJS_SUPPORT_VCL` / `__WIN32__`，Windows 代码自动跳过。
- `tjs_char` 在 macOS/iOS clang 下是 `wchar_t`（4B），`TJS_W(X) = L##X`。

## 里程碑实况

| 里程碑 | 出口 | 实测 |
|---|---|---|
| M0 | macOS clang 编出 `libtjs2.a` + `eval(1+1)==2` | run #11 全绿，`libtjs2.a` 1,229,824 B / 35 objs，TJS 2.4.28 |
| M1a | TJS2 的 4 个流工厂桥到 XP3Archive，Mac runner 真跑 | 见 CI 日志 `M1_RESULT=OK` |
| M1b | iOS arm64 编译链接进 app | run #15 ✅：`libtjs2_ios.a` 1,230,280 B / 35 objs（Mach-O arm64），`-force_load` 链进 app，`ARCHIVE SUCCEEDED` |
| M2~M4 | KAG 插件 / 资源解码 / 产品化 | 见 `docs/TJS2_PORT_PLAN.md` |

⚠️ 本机（Windows 沙盒）**没有 C++ 编译器**，所有原生改动只能靠 GitHub macOS runner
真编译验证。CI 步骤：`tools/build_tjs2_test.sh`（M0 + M1a），在 `xcodebuild` 之前执行。
