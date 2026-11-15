# GalBox → 完整 Kirikiri 运行时：移植方案与架构

> 状态（2026-11-15 更新）：**路线 A 已确认（完整移植官方 TJS2 + KAG）**。
> M0 ✅（run #11）、M1a ✅（run #13）、M1b ✅（run #15）、
> **M1c ✅（run #18 全绿）**：app 内第一次真的调用 TJS2。
> **M2a ✅（run #20 全绿）**：vendoring 官方 KAG3 系统脚本 + 补齐
> `Scripts.execStorage` 原生类，TJS2 已能执行包内 `.tjs` 脚本（官方启动链第一段）。
> **M2b ✅（run #22 一次全绿）**：`Storages` 自动路径 + 官方原始
> Shift-JIS 字节真解码 + `Scripts.eval`，并更正 M2a 的 vendoring 编码误判。
> **M2c 已实现，待 macOS runner 验证**：`System` / `Debug` 原生类 +
> **官方 `Initialize.tjs` 原文真跑**（第一次跑官方脚本，不再只跑自写探针）。
> 交付包 `dist/GalBoxNative-adhoc-M2b-9dbbef2a.ipa`（654,875 B，Mach-O arm64）。
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

### Milestone 2 — KAG 层（已按官方源码重新拆分）

**⚠️ 早期写法有误**：不存在 `src/plugins/win32/kag/`；KAG 的 C++ 解析器在
`src/plugins/win32/KAGParserEx/`（`KAGParser.cpp` 83 KB），而且 KAG3 的**系统脚本
本来就在官方仓库里**：`kag3/template/system/*.tjs`（28 个 / 448,729 B）。
官方 `kag3/template/startup.tjs` 只有一行有效代码：
`Scripts.execStorage("system/Initialize.tjs");` —— 即启动链的第一环是一个
**TJS2 原生类**，不是 KAG 解析器。完整规模：`src/core` 4,120 文件 / 38.7 MB，
`tp_stub.h` 486 KB，`LayerIntf.cpp` 293 KB，且整体绑 Windows。
⇒ 一轮推不完，拆成三段：

- **M2a（本轮）**：vendoring 官方 `startup.tjs` + 纯 TJS2 的 `system/Utils.tjs`；
  在 `HostBridge.cpp` 注册原生静态类 `Scripts` 实现 `execStorage`，
  让 TJS2 经宿主读流加载并执行包内 `.tjs`（含递归）。
  ✅ 出口：`test_m2a.cpp` 用目录模式归档真跑官方 `Utils.tjs`
  （`intrandom(5,5)==5`），iOS 侧 `strings` 校验 `M2A_SCRIPTS_EXEC_STORAGE` 进包。
- **M2b**：补 `Storages` / `System` 等原生类 + `Initialize.tjs` 依赖链，
  让官方 `startup.tjs` 一路执行到 KAG 初始化；接 iOS 事件队列
  （run loop / CADisplayLink）。⚠️ 执行必须放后台队列，不能占主线程。
- **M2c**：`KAGParserEx` / `KAGParser` 原生类 + KAG→渲染/音频宿主回调面，
  把 `GameViewController` 接到官方 KAG 播放循环，取代手写 `KagEngine`。
  ✅ 出口：一个真实 KAG 游戏能从头播到尾；`[iscript]` 块不再被丢弃。

详见 `native/KAG/README.md` 与 `native/KAG/VENDOR_MANIFEST.json`。

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
- **M2c（已实现，待 CI 验证）**：官方启动链第三段 —— `System` / `Debug` 原生类，
  并且第一次真跑**官方 `Initialize.tjs` 原文**（一个字不改）。
  - 决定性发现：`TJS_NO_REGEXP` **只**影响 `tjs.cpp:183` 是否把全局 `RegExp`
    类注册进 Global（`tjs.cpp:36` 的另一处只是省掉 include）。词法器
    `TJSParseRegExp`（`tjsLex.cpp:861/1320`）照常把 `/[^A-Za-z]/g` 解析成
    `T_REGEXP`，代码生成器只是把 `"RegExp"` 当成一次全局成员查找
    （`tjsInterCodeGen.cpp:2476`）。**结论：脚本能编译，只有在执行到那一行时才失败。**
    所以 Initialize.tjs 里 `createAppLock` 之前的全部语句都可以真跑。
  - 实现成员（逐条对齐官方脚本的真实语句，不按文档猜）：
    `exceptionHandler`（可写，第一句就是它）/ `exePath` / `osName` /
    `platformName` / `versionString` / `title` / `eventDisabled` /
    `screenWidth|Height` / `desktopLeft|Top|Width|Height` /
    `getTickCount` / `inform` / `exit` / `getArgument` / `createAppLock` /
    `addContinuousHandler` / `removeContinuousHandler` / `getKeyState`；
    `Debug.message`（**必须可当函数值取用**，官方第一句 `var dm = Debug.message`）/
    `Debug.notice` / `Debug.logAsError`。
  - 诊断升级：以前所有异常都被压成一句 "exception while executing entry script"
    （run #17 的真根因就是这样被藏了一整轮）。现在 `CaptureCurrentException()`
    抓 `eTJSScriptException` / `eTJSScriptError`，把 **脚本名（fatalBlock）、
    行号（fatalLine）、`throw new Exception(...)` 的正文（fatalException）**
    一并回传；`Scripts.execStorage` 自己的 catch 也改为先捕获再返回，
    不再吞掉内层脚本的真实异常。
  - 两个场景（都**期望 ok=false**，本轮不假装跑通）：
    A) 无 `Config.tjs` → 一路执行到官方自己那句
       `throw new Exception("Config.tjs が見つかりません。")`；
       断言 `execStorageCalls=1`、`autoPathCount=11`、`debugNotices=3`、
       `tickCalls≥1`、`exceptionHandlerSet=true`、`appLock=false`、
       `fatalBlock` 指向 `Initialize.tjs`、`fatalLine>0`、`fatalException` 含 `Config.tjs`。
    B) 加一个**探针版** `Config.tjs`（只写 `config_version = kagVersion`，明确标注非官方）
       → 版本核对通过、跳过 `UpdateConfig.tjs`，执行推进到
       `System.createAppLock(System.exePath.replace(/[^A-Za-z]/g,'_'))`，
       因全局 `RegExp` 缺失而失败。断言 `execStorageCalls=2`、
       `lastResolvedPath=system/Config.tjs`、`debugMessages≥1`
       （证明原生方法当函数值调用 `dm(...)` 真的生效），且失败原因含 `RegExp`
       —— 这就是**下一个阻塞点的硬证据**。
  - app 侧：`GameViewController` 打开含 `startup.tjs` 的包后，**在后台队列**
    跑一次 `tjs2ExecStartup`，把「autoPath / notice / tick 次数 + 停在哪一句」
    做成第 4 秒的 toast 显示出来（主线程同步执行曾导致导入卡死，不重犯）。
- **M2b** ✅（run #22 一次全绿）：官方启动链第二段。
  **实测**（macOS host 自测，非推断）：
  `{"ok":true,"probeValue":5,"hostReadDelta":2,"execStorageCalls":1,
  "autoPathCount":1,"autoPathHits":1,"shiftJisReads":1,
  "lastEncoding":"cp932","lastResolvedPath":"system/Utils.tjs"}`；
  第二个探针 `str2num("１２３")==123` 也通过（证明脚本内全角字面量解码正确，
  不是 latin-1 兜底碰巧语法过关）；错误 autoPath 明确失败并报
  `Storages: script not found via autoPath`，证明解析严格、不再静默执行空脚本。
  `M2B_RESULT=OK`、`M2A_RESULT=OK`（回归未破）、静态 224 PASS / 0 FAIL、
  `ARCHIVE SUCCEEDED`、Mach-O 807,648 B、`strings` 三个标记全部命中。
  交付 `dist/GalBoxNative-adhoc-M2b-9dbbef2a.ipa`（654,875 B）。
  - **编码纠错**：M2a 说「逐字节 vendoring 官方 UTF-8 原文」是错的 —— 上游 git blob
    实际是 **Shift-JIS + CRLF**（`Utils.tjs` 3,658 B / `Initialize.tjs` 7,246 B），
    GitHub **contents API 会静默转码成 UTF-8**（3,940 B / 8,182 B），M2a 拿到的是转码副本。
    本轮按 `/git/blobs/<sha>` 取原始字节落到 `native/KAG/src/original/`，
    `VENDOR_MANIFEST.json` 分开记录两套副本。转码还把 `〜`/`−` 映射成 CP932 的
    `～`/`－`，故解码必须先试 `CP932` 再 `SHIFT_JIS`。
  - **能力**：`HostStreams.cpp` 实现官方 `Storages.addAutoPath` 语义（后加优先、
    严格精确匹配，不用 `XP3Archive::find()` 的 basename 宽松回退）；
    `HostBridge.cpp` 注册原生静态类 `Storages`（`addAutoPath` /
    `isExistentStorage` / `getPlacedPath` / `chopStorageExt`）并补官方高频的 `Scripts.eval`。
    `execStorage` 增加存在性检查：此前缺失 `.tjs` 会被「空流」静默当空脚本执行。
  - **判据**：`test_m2b.cpp` 用目录归档装**官方原始 SJIS 字节**，探针
    `intrandom(5,5)==5`（脚本真执行）、`str2num("１２３")==123`（脚本内全角字面量
    解码正确，latin-1 兜底必失配）、`lastEncoding` / `shiftJisReads`（解码链走 SJIS）、
    `autoPathHits` / `lastResolvedPath`（短名经 `system/` 展开）、错误 autoPath 必须失败。
    静态 224 PASS / 0 FAIL。
- **M2a** ✅（run #20 一次全绿）：官方 `kag3/template/startup.tjs` + `system/Utils.tjs`
  的 **UTF-8 转码副本**（非官方原始字节，见上条更正；SHA-256 见 `VENDOR_MANIFEST.json`）；
  `HostBridge.cpp` 注册 TJS2 原生静态类 `Scripts` 实现 `execStorage`，
  让 TJS2 经宿主读流加载并递归执行包内 `.tjs`。
  **实测**：目录模式归档 `entries=2, dirMode=1`；
  `{"ok":true,"entry":"startup.tjs","probe":"intrandom(5,5)","probeValue":5,
  "hostReadDelta":2,"execStorageCalls":1}` — 官方 `Utils.tjs` 里的
  `intrandom` 真的被 TJS2 执行并返回值（不是靠读计数自证）；
  `M2A_RESULT=OK`；静态 181 PASS / 0 FAIL；`ARCHIVE SUCCEEDED`，
  app 二进制 807,104 B，`strings` 命中 `M1C_HOST_v1` 与 `M2A_SCRIPTS_EXEC_STORAGE`。
  ⚠️ app 里**仍不自动执行**商业 `startup.tjs`：M2b 前 `Initialize.tjs` 会因缺原生类抛异常，
  且可能长循环/等待，放主线程有卡死风险；`+tjs2ExecStartup` 仅作诊断入口，接线须走后台队列。
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
1. **M2d（正则）**：官方 `Initialize.tjs` 的下一句就是
   `System.exePath.replace(/[^A-Za-z]/g, '_')`，全局 `RegExp` 缺失即失败。
   两条路：**(a)** 让 `tjsRegExp.cpp` 复活 —— 它 832 行全部绑定
   `boost::reg_expression` / `boost::match_results` 的 C++ API，需要 runner
   装 boost（`brew install boost`）并解决这一版老代码与现代 boost 的编译差异；
   **(b)** 自己写一个 `std::regex` 背书的 `RegExp` 原生类，接口对齐
   `tjsRegExp.h`（`compile/test/match/exec/replace/split` + `lastMatch` 等属性）。
   (b) 不引外部依赖、可控性更好，但要自己保证 `replace/split` 语义与官方一致。
2. **M2e（Layer / Window + 事件队列）**：`new KAGWindow()` 与
   `kag.process("first.ks")` 需要的原生类，接 iOS run loop / CADisplayLink
   与渲染/音频回调面。宿主回调面继续走「纯 C 边界 + C++ 适配层」，
   不要把官方 `tp_stub` 头暴露给 ObjC++/Swift。
   ⚠️ app 已编译 `XP3Archive.cpp`，宿主层**只能链 `HostStreams_ios.o` /
   `HostBridge_ios.o`**，勿重复打包 XP3Archive.o。
   ⚠️ 商业 `startup.tjs` 的执行必须放后台队列，切勿在主线程同步调用。
3. **M3/M4**：TLG/TPP/WAV/OGG 解码、视频换 AVFoundation、产品化切换。

**当前边界**：`System` / `Debug` 已就位，官方 `Initialize.tjs` 原文能真跑并被
精确停在 `createAppLock`（缺 `RegExp`）或配置检查处；`Layer` / `Window` /
`KAGParserEx` 与事件循环仍是 M2e。界面文案已随本轮更新，不再说「System 未移植」。

**踩坑记录**：
- **GitHub contents API 会静默转码**（M2b 发现）：`/repos/.../contents/<path>`
  对非 UTF-8 文本返回的是**转码后的 UTF-8**，不是 git blob 原始字节。
  用它做「逐字节 vendoring」必然失真。取原文必须用
  `/git/blobs/<sha>` + `git/trees/master?recursive=1` 的 sha 校验。
  官方 KAG3 脚本是 Shift-JIS，商业游戏同理 —— 只测 UTF-8 副本等于没测编码链。
- **「没抛异常」不等于「解码正确」**：文本流对未知编码的逐字节 latin-1 兜底
  同样能让注释/字符串语法过关。必须记录实际采用的编码，并用脚本内的
  日文字符串字面量（如 `str2num("１２３")`）做实质判据。
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
