# GalBox → 完整 Kirikiri 运行时：移植方案与架构

> 状态（2026-11-15 更新）：**路线 A 已确认（完整移植官方 TJS2 + KAG）**。
> M0 ✅（run #11）、M1a ✅（run #13）、M1b ✅（run #15）、
> **M1c ✅（run #18 全绿）**：app 内第一次真的调用 TJS2。
> 交付包 `dist/GalBoxNative-adhoc-M1c-773cf357.ipa`（Mach-O arm64）。
> 早期"网络被代理拦截"的判断**已推翻**：沙盒默认无代理变量，是我手动 `export`
> 了失效的 `127.0.0.1:54662` 才导致 `http=000`；直连即可。上游仓库也从
> `kikikracle/kirikiri2`（404）更正为 `jeeb/kirikiri2`。
> 本文件保留路线图与设计依据，已按官方源码核对过的部分标注更正。

## 1. 为什么要走"完整移植"而不是继续手写 KAG

GalBox 当前的 `KagEngine.cpp`（805 行）只是一个**硬编码 38 条命令白名单 + 玩具表达式求值器**的解释器。Kirikiri 的真实结构是两层堆叠：

- **TJS2**：一门完整脚本语言（类 JS：类/函数/对象/原型、内建类 `Array/Dictionary/Regex/String/Math`、异常、GC、宿主回调）。商业游戏的 `startup.tjs`、系统配置、游戏逻辑、自定义命令、存档全写在这层。
- **KAG**：**建在 TJS2 之上的 TJS2 库/宏层**。`system/System.tjs` 等系统脚本 + KAG 解析器/播放器都是 TJS2。`.ks` 里的 `@bg @play @wait` 在真引擎里全是 TJS2 函数。

GalBox 缺的是"程序语言那一半"。手写补 TJS2 等于从零造一门语言，且无法对齐官方语义 → 不可行。**唯一正确路径是把官方开源 TJS2 + KAG 真正移植进来**，GalBox 自己的 `KagEngine` 在 Milestone 2 后会被 TJS2+KAG 路径取代（导入/资源层保留）。

## 2. 一套"完整运行时"需要的零件

| 模块 | 内容 | 来源 |
|---|---|---|
| TJS2 语言引擎 | 词法/语法、字节码或 AST 解释器、对象原型、内建类、异常、GC、多线程 dispatcher | 官方 `kirikiri2/src/core/tjs2/` |
| 宿主接口 tp_stub | 文件/流、tick、日志、异常、事件队列、应用路径 —— TJS2/插件回调平台的边界 | `kirikiri2/src/core/tp_stub.h` + `tjs2/tjs.h` 的 `iTJS2Host` |
| KAG 层 | KAG 解析器/播放器（TJS2 库）+ `system/` 系统脚本 | `kirikiri2/src/plugins/win32/kag/` + 游戏 `system/` |
| 资源解码 | TLG/TPP 图像、WAV/OGG 音频、视频、字符编码(Shift-JIS/EUC-JP) | 官方解码器（视频改 AVFoundation，Windows 上用 DirectShow 需替换） |
| XP3 | 归档读取（已有，需补加密/分卷完整支持） | GalBox 现有 `XP3Archive` |

## 3. 里程碑路线图（每个里程碑都以 Mac runner 验证为出口）

> 约束：**本机无 C++ 编译器**，原生改动只能靠 GitHub 免费档 Mac runner 真编译验证（几分钟/次、runner 不一定随时有）。因此每个里程碑必须小步、可独立编译、有自测。

### Milestone 0 — Vendor + 编译 TJS2 核心（无宿主集成，仅自测）
- 拉取 `kirikiri2/src/core/tjs2/` 全部 `.cpp/.h`（约 30k+ 行）。
- 剥离 Windows 依赖：用 shim 替换 `<windows.h>`、`TCHAR`、`OutputDebugString`、`__try` 等；`tjs2` 本身相对平台无关，重点在少数几个文件。
- 作为静态库 `libtjs2.a` 接入 Xcode（或用 CMake 生成后拖入）。c++14 标准，关异常/RTTI 视上游而定。
- 写一个极简 `eval("1+1")` 自测，先在 **macOS host** 上证明引擎能编译并求值
  （iOS arm64 编译链接放到 M1b，避免两件事混在一轮 CI 里排查）。
- ✅ 出口：Mac runner 上 `libtjs2.a` 编出、自测打印 `2`。
  **实测（run #11）**：TJS 2.4.28，`libtjs2.a` 1,229,824 B / 35 objs，13 个 CI 步骤全绿。

### Milestone 1 — 宿主接口（tp_stub / iTJS2Host）的 iOS 实现
- 实现 `native/Host/HostInterface.hpp` 定义的契约：
  - **文件/流**：桥到 GalBox `XP3Archive` + `NSFileManager`(Documents)，覆盖 `TVPOpenStream`/`TVPGetFileSize` 等。
  - **tick / 定时器**：`mach_absolute_time()` / `CACurrentMediaTime()`。
  - **日志/异常**：`os_log` + Swift 控制台 sink；`TVPThrowExceptionMessage`。
  - **事件队列**：把 TJS2/KAG 的计时与 `@wait` 驱动接到 iOS run loop + `CADisplayLink`（关键：KAG 的 wait 由宿主事件循环推进）。
  - **应用路径 / 本地化**：返回 Documents、bundle 路径、`TVPGetLanguage`。
- ✅ 出口：TJS2 能跑真实脚本，能用 `TVPPrint` 输出、能 `openStorage` 读 xp3 内文件。

### Milestone 2 — KAG 层（移植官方 KAG 插件）
- 拉取 `kirikiri2/src/plugins/win32/kag/` 的 KAG 解析器/播放器 + 游戏的 `system/` 脚本（`System.tjs` 等）。
- 实现 **KAG→渲染/音频宿主回调**（GalBox 现有 Swift/ObjC++ 层即渲染/音频后端）：KAG 每帧把文本/图片/音频指令推给 `GameViewController`。
- 启动改为：检测到 `startup.tjs` 时由 TJS2 走官方启动路径加载 KAG，而非 GalBox 手写解释器。
- ✅ 出口：一个真实 KAG 游戏（如内置 demo 升级为用 KAG 跑）能从头播到尾；`[iscript]` 块不再被丢弃。

### Milestone 3 — 资源解码
- TLG/TPP → `CGImage`；WAV/OGG → `AVAudioPlayer`/`AudioQueue`；视频 → `AVPlayer`（替换 DirectShow）；字符编码检测（Shift-JIS/EUC-JP/UTF-8）。
- ✅ 出口：含 TLG 立绘、OGG BGM、日文文本的游戏资源正常解码。

### Milestone 4 — 产品化
- `GBEngineBridge` / `GameViewController` 切到 TJS2+KAG 路径，旧 `KagEngine` 保留为降级或移除。
- 保留已修的导入能力（文件夹/分卷/后台复制）——它们喂给文件宿主。
- 扩充自测（native.test.py / ci 链路），出签名 ipa。
- ✅ 出口：商业 TJS2 游戏可导入并运行（重度视频/特效类仍可能有个别不支持，逐个补）。

## 4. 宿主接口设计（拱心石，详见 `native/Host/HostInterface.hpp`）

**⚠️ 已按 vendored 官方源码更正**：TJS2 **核心**里不存在 `iTJS2Host` / `iTVPHost`，
那是早期离线凭印象写的近似契约。核对 `tjs.h` 后确认真实宿主边界只有：

1. `tjs.h:192-199` 的 **4 个全局流工厂函数指针**（默认实现返回 NULL）：
   `TJSCreateTextStreamForRead/Write`、`TJSCreateBinaryStreamForRead/Write`。
   调用点：`tjsArray.cpp:269/375/422/434`、`tjsDictionary.cpp:76/88`
   （`Array.load/save`、`Dictionary.load/save`、字节码）。
2. `tjsConfig.cpp:30` 的 `TJSGetTickCount()`（POSIX 分支 M0 已编译通过）。

`tp_stub` / `TVP*` 平台层属于 kirikiri2 的**插件层**（KAG、视觉、音频、事件），
是 M2 的工作，不是 TJS2 核心的前置条件。

**M1a 已落地**：`native/Host/HostInterface.hpp`（对齐后的契约）+
`native/Host/HostStreams.cpp`（4 个流工厂桥到 `XP3Archive` + Documents + bundle，
含 UTF-8 / UTF-16LE / SHIFT_JIS 解码）。`test_m1.cpp` 在 Mac runner 上真跑验证
「二进制流读 XP3 内文件 → 文本流解日文 → TJS2 的 `Array.load` 回调宿主 → 写回 Documents」。

M1b / M2 才需要：`mach_absolute_time`、`os_log`、iOS run loop + `CADisplayLink`
事件队列（KAG `@wait` 靠它推进）、渲染/音频回调面。

## 5. Vendoring（M0 已完成）

- 上游更正为 **`jeeb/kirikiri2` @ master**，`kirikiri2/src/core/tjs2/`
  （`kikikracle/kirikiri2` 是 404）。
- 77 个源文件 / 1,269,921 B 已落到 `native/TJS2/src/`，按 blob SHA 逐个取回并核对字节数，
  清单 `VENDOR_MANIFEST.json`；6 类 clang 移植补丁见 `PATCHES.md`。
- 官方 `src/core/base/TextStream.h` 属于插件层，TJS2 核心不需 vendoring。
- `src/plugins/win32/kag/` → `native/KAG/src/`（Milestone 2 待做）。
- 游戏 `system/` 脚本随具体游戏提供，不入库。

**授权更正**：Kirikiri2 / KAG 3 是**双许可证**（吉里吉里独自许可 **或** GNU GPL，
二选一），**不是 BSD**。原文在 `native/TJS2/LICENSE.txt`。必须保留 LICENSE 与
每个源文件头注释。⚠️ 上架 App Store 前须确认所选许可与商店条款兼容
（GPL 选项通常冲突），或维持侧载/开源分发。

## 6. 风险与未知
- TJS2 源码中仍有少量 Windows 耦合（线程/SEH/调试输出），需逐文件 shim，可能踩坑 → Milestone 0 会暴露。
- KAG 宿主回调面很大（文本/图像/音频/层/转场/输入），Milestone 2 工作量最高。
- 视频（DirectShow）在 iOS 无对应，需 AVFoundation 重写。
- 验证只能靠 Mac runner，迭代慢；每个里程碑拆分到"一次推送即可验证"的粒度。

## 7. 当前状态与下一步

**已完成**
- **M0** ✅（run #11）：官方 TJS2 核心 vendoring + 6 类 POSIX 补丁 + `libtjs2.a` / `eval` 自测。
- **M1b 校正**：run #15 的 `OTHER_LDFLAGS` **只 `-force_load` 了 `libtjs2_ios.a`**，
  `HostStreams_ios.o` 并没有进包；app 二进制里也没有任何 TJS2 调用点。
  也就是说 M1b 只证明「iOS 链接器能解析核心符号」，**不等于 app 会跑 TJS2**。
  M1c 才把宿主层与调用点补齐，并以二进制固定标记 `M1C_HOST_v1` 硬校验。
- **M1c**（本轮实现）：`native/Host/HostBridge.{h,cpp}` 用纯 C 边界把官方 `tjs.h`
  隔离在 C++ 侧（编译开关必须与静态库一致，不能让 ObjC++ target 直接 include）；
  `GBEngineBridge` 打开包时 `Install(&gArchive, Documents, bundle)`、关闭时 `Uninstall`，
  新增 `+tjs2SmokeTest` 真跑 `tTJS` → `eval(1+2*3)` → `Array.load/save` 回调宿主读写；
  UI 打开包后显示 `TJS2 2.4.28 ✓ eval=7 · 宿主读1/写1`。
  CI 侧因为**无法运行 iOS app**，同一份 `HostBridge.cpp` 在 macOS host 上由
  `test_m1c.cpp` 真跑（含未安装/已卸载的失败路径），iOS 侧只负责编译 + 链接 +
  `strings` 检查 `M1C_HOST_v1` 标记。
  **run #18 实测**：`M1C_RESULT=OK`；
  `{"ok":true,"version":"2.4.28","eval":7,"hostReadDelta":1,"hostWriteDelta":1}`；
  写盘文件 `m1c_smoke.txt` 7 B = `galbox`；`OTHER_LDFLAGS` 三个产物齐备；
  `** ARCHIVE SUCCEEDED **`，app 二进制 790,680 B，`M1C_HOST_v1` 进包。
  ⚠️ Release 会 strip 符号表，`nm` 显示 `GalBoxTJS2`/`tTJS` 符号数 0 属预期，
  因此 CI 用 `strings` 固定标记作硬断言，不用 `nm` 计数当判据。
- **M1a** ✅（run #13）：宿主契约对齐官方 + 4 个流工厂桥到 XP3Archive；
  实测 `Array.load via TJS2: host read calls 2 -> 3`，`M1_RESULT=OK`。
- **M1b** ✅（run #15）：`xcrun --sdk iphoneos clang++ -arch arm64` 编出
  `libtjs2_ios.a`（1,230,280 B / 35 objs，Mach-O arm64）+ `HostStreams_ios.o`；
  `build_native.sh` 以 `-liconv -force_load` 注入 `OTHER_LDFLAGS`（只链核心库），
  Ld 目标 `arm64-apple-ios15.0`，`ARCHIVE SUCCEEDED`，app 二进制含 TJS2 符号。

**无阻塞项**：网络直连可用；`push_via_api.py` 走 Git Data API 原子提交（git 协议仍被墙）。

**下一步（按序）**
1. **M2**：vendoring `src/plugins/win32/kag/`，实现 KAG 宿主回调面 + 事件队列
   （iOS run loop / CADisplayLink），这是工作量最高的里程碑，需再拆细。
   M2 的宿主回调面建议继续走 `HostBridge` 这种「纯 C 边界 + C++ 适配层」结构，
   不要把官方 `tp_stub` 头直接暴露给 ObjC++/Swift。
   ⚠️ app 已编译 `XP3Archive.cpp`，宿主层**只能链 `HostStreams_ios.o` /
   `HostBridge_ios.o`**，勿重复打包 XP3Archive.o。
2. **M3/M4**：TLG/TPP/WAV/OGG 解码、视频换 AVFoundation、产品化切换。
3. **M1c 之后仍需验证的边界**：`startup.tjs` 依然不会被执行 —— TJS2 引擎能跑，
   但商业游戏的系统脚本依赖 KAG 插件层（M2）；此时界面文案已改为准确表述，
   不再说「内核未实现 TJS2」。

**踩坑记录**：
- Windows 工作区的 `.sh` 易带 CRLF，macOS bash 3.2 会把
  `set -euo pipefail\r` 判为 `invalid option name`（run #14 失败根因）。
  `push_via_api.py` 已加自动 LF 守卫；所有 `.sh`/`.yml` 推送前统一 LF。
- **run #17（M1c 首次 CI 失败）**：冒烟脚本写了 `new Array("galbox","m1c")`，
  但官方 `tTJSArrayNI::Construct` 要求 `numparams == 0`（`tjsArray.cpp:1002`），
  直接抛 `TJS_E_BADPARAMCOUNT`，导致 `Array.save` 从未执行 → `hostWriteDelta=0`。
  改成 `new Array(); b.add("galbox"); b.save(...)`。
  同时发现两处诊断缺陷：① 整个 try 只报一句笼统的 "TJS2 exception"，
  藏住了失败阶段；② 异常时 `engine->Release()` 被跳过。
  现已改为 eval/load/save 三段独立 try + 收尾必 Release。
  **闭环补强**：`test_m1c.cpp` 现在会打开 `m1c_smoke.txt` 核对内容含 `galbox`，
  不再只靠「写计数 +1」自证落盘。
- **判据铁律补充**：链接成功 / 回调计数增加都只是「线接上了」，
  不等于数据真的落盘、更不等于游戏能跑；每加一层都要有独立可证伪的出口。

**长期约束**：本机无 C++ 编译器，每个原生改动都必须靠 GitHub macOS runner 真编译验证；
一次推送能验证的粒度要小，避免把多个未验证改动堆在一轮里。
