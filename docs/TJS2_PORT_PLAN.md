# GalBox → 完整 Kirikiri 运行时：移植方案与架构

> 状态（2026-11-15 更新）：**路线 A 已确认（完整移植官方 TJS2 + KAG）**。
> M0 ✅（run #11）、M1a ✅（run #13）、M1b ✅（run #15）、
> **M1c ✅（run #18 全绿）**：app 内第一次真的调用 TJS2。
> **M2a ✅（run #20 全绿）**：vendoring 官方 KAG3 系统脚本 + 补齐
> `Scripts.execStorage` 原生类，TJS2 已能执行包内 `.tjs` 脚本（官方启动链第一段）。
> **M2b ✅（run #22 一次全绿）**：`Storages` 自动路径 + 官方原始
> Shift-JIS 字节真解码 + `Scripts.eval`，并更正 M2a 的 vendoring 编码误判。
> **M2c ✅（run #28 全绿）**：`System` / `Debug` 原生类 +
> **官方 `Initialize.tjs` 原文真跑**（第一次跑官方脚本，不再只跑自写探针）。
> 实测已把下一个阻塞点钉死在**正则**：`Member "RegExp" does not exist`
> @`system/Initialize.tjs:164`。
> **M2d ✅（run #32 全绿）**：正则层——**不引 boost**，把官方 `tjsRegExp.cpp`
> 用 `std::regex` 整份重写（`native/TJS2/PATCHES.md` P7），并去掉官方
> `TJS_NO_REGEXP` 开关。官方 `Initialize.tjs` 已能推过
> `System.exePath.replace(/[^A-Za-z]/g,'_')`（`appLock=true`，键值与独立实现一致），
> 继续执行到 `KAGLoadScript("KAGLayer.tjs")` 才停 —— 阻塞点正式从「引擎缺正则」
> 推进到「M2e 缺 KAG 层的 Layer/Window 与系统脚本」。
> 交付包 `dist/GalBoxNative-adhoc-M2d-a3916cac.ipa`（691,692 B，Mach-O arm64）。
> 上一轮交付 `dist/GalBoxNative-adhoc-M2c-e53d1c66.ipa`（662,319 B）。
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
- **M2c**（已按实际执行顺序调整）：`System` / `Debug` 原生类，让官方
  `Initialize.tjs` 原文能真跑（原计划里的 KAGParser 挪到 M2e 之后）。
- **M2d**（实际执行顺序）：正则层 `RegExp`（`std::regex` 重写），拆掉
  `createAppLock` 那一行的阻塞点。
- **M2e**：`Layer` / `Window` / `KAGParserEx` 原生类 + KAG→渲染/音频宿主回调面，
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
- **M2e-3** ✅（run #41 一次全绿）：**GalBox 自写的 `Window` 骨架** +
  `System.dataPath` + `Debug.logLocation`。
  官方 `Window` 同样是 `core/visual/WindowIntf.cpp` 的原生类，而
  `MainWindow.tjs`（150 KB / 5893 行）第 7 行就是 `class KAGWindow extends
  Window` ⇒ run #36 的阻塞点。沿用 M2e-2 的路子，用纯 TJS2 骨架顶
  （`native/KAG/src/system/GalBoxWindow.tjs`，明确标注非官方、放在与官方原文
  不同的目录）。成员清单来自对 `MainWindow.tjs` 的实际统计，不是照抄文档猜的。
  - 顺带补两个宿主侧成员，它们**就在实例化路径上**，缺一个就挡住：
    `System.dataPath`（KAGWindow 的**类成员初始化**就写了
    `var saveDataLocation = System.dataPath;`，MainWindow.tjs:38 ——
    实例化的那一刻就会读它）与 `Debug.logLocation`（构造器里
    `Debug.logLocation = saveDataLocation;`，MainWindow.tjs:290）。
  - **踩到两个 TJS2 语法雷**（都已写进静态测试防复发）：
    ① `function close(...)` —— TJS2 **不支持** `...` 变参定义（源码里没有这个
    记号，官方 20 个脚本里也没有一处 `function xxx(...)`；`...` 只在**调用**时
    展开数组）；② `function invalidate()` —— `invalidate` 是 TJS2 的对象失效
    **关键字**，官方脚本从不当函数名用。撞上任何一条都会让**整个** shim 语法
    错误、连带 `Window` 完全装不上。
  - **判据加固**：骨架 tag 之前是在尝试装载**之前**就写进状态串的，于是骨架
    语法错误时状态串照样出现 `M2E3_WINDOW_SKELETON` —— 判据差点被骗过去。
    现在 tag 只在 `ExecScript` 成功之后才进 `status`，失败另进 `failed` 并附
    **block/line**（run #38 只看到一句 `Syntax error`，定位不到行，只能二分试）。
  - **run #41 实测（macOS host 真跑）**：
    A) `new KAGWindow()` **越过了父类查找**，撞到下一个缺口
       `Member "crArrow" does not exist` @ `MainWindow.tjs:205`
       ⇒ 又补了 `crArrow` / `crHandPoint` / `crSizeAll` 等**全局光标常量**
       （官方由原生层提供；KAGWindow 的类成员初始化直接读它们，缺一个会在
       **加载** `MainWindow.tjs` 时就炸，不是实例化时）。
    B) 完整官方启动链（干净首跑）：`scriptsRead=15 / attempts=15 / calls=14 /
       autoPathCount=11 / autoPathHits=14 / appLock=true`，
       新缺口 `Member "VK_SPACE" does not exist` @ `MainWindow.tjs:210`
       ⇒ **下一个缺口是 `VK_*` 键码常量**（官方由原生层提供）。
    C) 负向对照（无 `Window` 骨架）：`Member "Window" does not exist`
       @ `MainWindow.tjs` —— 精确复现 run #36 的阻塞点。
  - ⚠️ **发现一个未解现象（下一轮的课题）**：run #40 里「场景 B 排在场景 A
    之后」时，官方启动链**整个坏掉** —— `autoPathCount=0`（13 次
    `addAutoPath` 一次都没成功）、`trace` 只 1 项、`appLock=false`、
    `debugNotices=3`（说明 `Initialize.tjs` 明明执行过），诊断自相矛盾。
    **隔离实验**（把 B 调到最前跑）显示干净首跑一切正常 ⇒ 确认是
    **同进程内前一个场景的状态泄漏**，不是官方脚本本身的问题。根因待查。
  - 回归 M0~M2e-2 全绿；静态 464 PASS / 0 FAIL；`ARCHIVE SUCCEEDED`；
    app 二进制 893,184 B；八个标记（含新增 M2E3）全命中。
  - 交付 `dist/GalBoxNative-adhoc-M2e3-c61a4ce5.ipa`（696,000 B，Mach-O arm64）。
- **M2e-2** ✅（run #36 一次全绿）：**GalBox 自写的 `Layer` 骨架**。
  官方 `Layer` 是 `kirikiri2/src/core/visual/LayerIntf.cpp` 的**原生类**
  （293 KB，绑死 Windows tp_stub），而 KAG 的 20 个系统脚本里有 **18 个**
  `extends Layer` —— 没有它一个都跑不起来。本轮用一份**纯 TJS2 的骨架**
  （`native/KAG/src/system/GalBoxLayer.tjs`，明确标注非官方、放在与官方原文
  不同的目录）先顶上：`class Layer` 占位成员 + 官方脚本直接引用的全局常量
  （`clNone` / `clAdapt` / `stt*` / `stst*`）。它**不渲染、不解码图片、不转场**。
  - **关键机制（TJS2 的 `super` 是惰性的）**：`class X extends Y` 这条语句
    **不去查** `Y`，引擎把它编译成 super proxy，只有在**实例化**或访问父类
    成员时才求值。⇒「脚本被成功读完」≠「继承成立」，必须真的 `new` 一次。
    run #34 里 `KAGLayer.tjs` 进了 `scriptTrace` 却仍报 Layer 缺失，就是这个。
  - 判据三场景（都真跑）：A) `m2f_layer_probe.tjs` 读入官方 `KAGLayer.tjs`
    原文后 `new KAGLayer(null,null)` 走通 → `global.m2fProbe=1`，`probeValue`
    必须**恰好 1**（-1 = 实例化抛异常，探针压根没走到最后一行）；
    B) 完整官方启动链：仍要求 `ok=false`、错误里不再有 Layer 缺失、
    加载脚本数不比 M2e-1 的 10 个倒退；
    C) **负向对照**：同一套夹具但**不放**骨架，必须复现 run #34 的
    `Member "Layer" does not exist` @ `KAGLayer.tjs:87`。没有 C，A 就是恒真式自证。
  - **run #36 实测（macOS host 真跑）**：
    A) `{"ok":true,"probeValue":1,"shim":"M2E2_LAYER_SKELETON",
       "scriptTrace":"KAGLayer.tjs","lastEncoding":"cp932",...}`
       → 官方脚本类**成功继承** GalBox 的 Layer 骨架。
    B) `{"ok":false,"scriptsRead":15,"execStorageAttempts":15,
       "execStorageCalls":14,"shiftJisReads":15,"autoPathCount":11,
       "appLock":true,"lastDebug":"スクリプトの読み込みに 35ms かかりました",
       "fatal":"Member \"Window\" does not exist","fatalBlock":"MainWindow.tjs",
       "fatalLine":7,"lastScriptError":"exception in execStorage(system/Initialize.tjs)"}`
       `scriptTrace` = Initialize, Config, Utils, KAGLayer, HistoryLayer, BGM,
       SE, Movie, Conductor, AnimationLayer, GraphicLayer, MessageLayer, Menus,
       DefaultMover, **MainWindow**
       ⇒ 官方脚本加载数 **10 → 15**，新阻塞点精确落在
       **`MainWindow.tjs:7` 缺 `Window` 原生类**。
       ⚠️ `lastScriptError` 指向的是 `system/Initialize.tjs`（异常冒泡回它的
       `KAGLoadScript`），**根因只在 `fatalBlock + fatalLine`** —— 这已经是第
       两次验证「只有 fatal 定位可信」。
    C) `{"ok":false,"probeValue":-1,"shim":"absent",
       "fatal":"Member \"Layer\" does not exist","fatalBlock":"KAGLayer.tjs",
       "fatalLine":87}` → 精确复现 run #34 的阻塞点。
  - 宿主侧：`HostBridge` 新增 `LoadEngineShim()`，在跑入口脚本前加载
    `system/GalBoxLayer.tjs`（游戏包里没有就跳过，状态走 JSON 的 `shim` 字段）。
    刻意**不走** `Scripts.execStorage`（那会污染 `execStorageAttempts` /
    `scriptTrace` / `lastScriptError` 三条判据），也不调 `CaptureCurrentException`
    （骨架自己的失败不该占用 `fatalBlock`/`fatalLine`）。
  - 回归 M0~M2e-1 全绿；静态 419 PASS / 0 FAIL；`ARCHIVE SUCCEEDED`；
    app 二进制 893,184 B；七个标记（含新增 M2E2）全命中。
  - 交付 `dist/GalBoxNative-adhoc-M2e2-323514e1.ipa`（693,381 B，Mach-O arm64）。
- **M2e-1** ✅（run #34 一次全绿）：**官方 KAG 系统脚本全量 vendoring**。
  run #32 停在 `Storages: script not found via autoPath: KAGLayer.tjs`（缺文件），
  本轮把 `kag3/template/system/` 的 **28 个上游 blob**（20 个 `.tjs` +
  `Config.~new` + 7 个 png/asd，448,729 B）按 `/git/blobs/<sha>` 原始字节
  落到 `native/KAG/src/original/system/`，把阻塞点逼成**缺原生类**。
  - 保真：tree API 列 28 个 blob → 逐个重算 `sha1("blob <len>\0"+raw)` 与 tree
    里的 sha 比对，28/28 一致；`VENDOR_MANIFEST.json` 逐条记 blob sha /
    SHA-256 / 字节数；构建脚本按清单逐文件校验 sha256+bytes 且总数=448,729；
    `test_m2e.cpp` 再对夹具里 28 个文件校验精确字节数。**绝不用 contents API**
    （会静默转 UTF-8，M2a 踩过）。
  - 三层判据（都不是「没报错就算过」）：
    ① `m2e_manifest_probe.tjs` 按官方方式 `addAutoPath("system/")` 后用短名查
       20 个 `.tjs`，另设**必然不存在的哨兵** → `m2eProbe=found*100+bad`，
       期望恰好 `2000`。若 `isExistentStorage` 是恒真函数，哨兵会让它翻车。
    ② `HostBridge` 新增 `scriptTrace`：只在**流成功打开并读到正文之后**追加
       短名，所以「缺文件」与「读到但执行失败」能分开。
    ③ **负向对照**：故意只放旧的 4 个文件，断言 trace 不含 `KAGLayer.tjs`
       且错误回到 `script not found`。没有这条，②就是恒真式自证。
  - **run #34 实测（macOS host 真跑）**：
    A) `{"ok":true,"probeValue":2000,...,"scriptsRead":0}`
       → 20/20 官方 `.tjs` 经 autoPath 可见，哨兵正确地没命中；
       探针没加载任何脚本，轨迹为空（与 B 配对证明计数器不是恒真）。
    B) 官方 `startup.tjs` → `Initialize.tjs` 原文 + 全量系统脚本：
       `{"ok":false,"appLock":true,"scriptsRead":10,"execStorageAttempts":10,
       "execStorageCalls":8,"shiftJisReads":10,"autoPathCount":11,
       "autoPathHits":9,"debugMessages":9,"tickCalls":18,
       "lastEncoding":"cp932","lastDebug":"Conductor.tjs を読み込みました(1ms)",
       "fatal":"Member \"Layer\" does not exist","fatalBlock":"KAGLayer.tjs",
       "fatalLine":87,"lastScriptError":"exception in execStorage(AnimationLayer.tjs)"}`
       `scriptTrace = system/Initialize.tjs, Config.tjs, Utils.tjs, KAGLayer.tjs,
       HistoryLayer.tjs, BGM.tjs, SE.tjs, Movie.tjs, Conductor.tjs, AnimationLayer.tjs`
       ① **根因被精确定位到 `KAGLayer.tjs:87`**
          （`class KAGLayer extends Layer` 的 `Layer` 原生类未实现）；
       ② 启动链没有在该行立刻断掉 —— 之后又真读了 7 个官方脚本，
          最外层冒泡的 execStorage 上下文是 `AnimationLayer.tjs`；
          `calls=8 / attempts=10` 说明有 2 次内层执行抛异常。
          ⇒ **别把 `lastScriptError` / `lastResolvedPath` 当成「停在哪一句」**，
          唯一可靠的定位是 `fatalBlock + fatalLine`（run #30 也踩过同类坑）。
       ③ 10 次 SJIS 解码、`appLockKey` 与 M2d 一致 ⇒ 正则与解码链没被本轮弄坏。
    C) 负向对照：`scriptTrace=system/Initialize.tjs,Config.tjs,Utils.tjs`，
       `lastScriptError="Storages: script not found via autoPath: KAGLayer.tjs"`。
  - 顺带修正：`Install()` 此前不清零 `shiftJisReads`，后一次启动会「借」上一次
    的解码计数 ⇒ 已归零。这些是官方模板脚本，商业游戏自带自己的 `system/` 副本，
    **不打进 app bundle**；app 里保留 `M2E_SYSTEM_SCRIPTS_VENDORED` 二进制标记。
  - 回归 M0/M1/M1a/M1c/M2a/M2b/M2c/M2d 全绿；真编译 34 PASS / 0 FAIL、
    静态 374 PASS / 0 FAIL、出包链路 48 PASS / 0 FAIL；
    `libtjs2_ios.a` 1,387,328 B / 36 objs；`ARCHIVE SUCCEEDED`；
    app 二进制 893,184 B；六个标记（含新增 M2e）全命中。
  - 交付 `dist/GalBoxNative-adhoc-M2e-021b7d12.ipa`（691,877 B，Mach-O arm64）。
- **M2d** ✅（run #32 一次全绿）：**正则层**。M2c 停下的那句
  `System.createAppLock(System.exePath.replace(/[^A-Za-z]/g,'_'))` 现在通了。
  - 路线选 **(b)**：不引 boost、不上 runner 依赖，把上游 `tjsRegExp.{h,cpp}`
    **整份重写**为标准库 `std::basic_regex` 实现（`PATCHES.md` P7）。
    可行原因：官方接入点只有三处，且都只依赖**类名/类 ID**，不依赖 boost 类型 ——
    `tjs.cpp:185`（`TJSCreateRegExpClass()`）、`tjsArray.cpp:482`
    （`tTJSNC_RegExp::ClassID` + `tTJSNI_RegExp::Split`）、
    `tjsInterCodeGen.cpp:2476`（字面量生成 `new RegExp(); _compile("//flag/body")`）。
    ⇒ 上游其它文件**一行没改**，只是不再定义 `TJS_NO_REGEXP`。
  - 语义逐条对齐上游：`g`/`i`/`l` 标志、`lastMatch`/`lastParen`/`leftContext`/
    `rightContext`/`index`/`lastIndex`/`input`/`matches`/静态 `last`、
    `replace` **不展开 `$1`**（上游就是如此，不是 JS 语义）、`split` 的
    purgeempty 与子表达式展开顺序、`Compile` 对空模式替换成 `(?:)`。
  - 已知差异与处理（都有判据）：boost 的 `.` 默认**吃换行** ⇒ 编译前把字符类外
    的裸 `.` 翻成 `[\s\S]`；`\Q...\E` 未实现（KAG3 系统脚本未出现）；
    replace 回调失败改为抛错（上游静默截断，静默丢字符更难查）。
  - 自测 `native/Host/test_m2d.cpp` 两个场景，**都真跑**：
    A) `m2d_regex_probe.tjs` 23 条语义检查（replace/test/match/exec/split/
       标志/`_compile` 内部表示/`.` 吃换行/非 ASCII）。判据是
       `probeValue = checks*100 + fails`，能区分「探针压根没执行」(-1)、
       「有 check 失败」(余数≠0)、「断言太少」(checks<20) —— 不是「没报错就算过」。
    B) 官方 `startup.tjs` → `Initialize.tjs` 原文 + 探针 `Config.tjs`。
  - **run #32 实测（macOS host 真跑）**：
    A) `{"ok":true,"probeValue":2300,...,"lastEncoding":"utf-8"}`
       → `regex checks ran=23, fails=0`。
    B) `{"ok":false,"appLock":true,
       "appLockKey":"_Users_runner_work_iosgal_iosgal_native_TJS__build_",
       "exePath":".../native/TJS2/build/","execStorageAttempts":3,
       "execStorageCalls":2,"debugMessages":3,
       "lastDebug":"Utils.tjs を読み込みました(0ms)",
       "lastScriptError":"Storages: script not found via autoPath: KAGLayer.tjs",
       "error":"Storages: script not found via autoPath: KAGLayer.tjs"}`
       → ① 正则真的算出了键值（`appLockKey` 与 C++ 独立逐字符实现完全一致）；
         ② 官方脚本继续加载了 `Config.tjs` 与 `Utils.tjs`；
         ③ 停在 `KAGLayer.tjs` —— **下一个缺口已经不是引擎能力，是 M2e 的 KAG 系统脚本/原生类**。
  - 顺带修正两处**会让判据说谎**的地方：
    * `System.exePath` 之前恒为空串，那句正则等于空操作的假验证 ⇒ 改为宿主
      真实上报游戏根目录（带尾斜杠），与官方 `System.exePath + "video/"` 语义一致。
    * `execStorage` 的 catch 会把内层原因覆盖成一句
      `exception in execStorage(...)` ⇒ 改为「只补空、不覆盖更具体信息」，
      并把最内层原因单独走 JSON 的 `lastScriptError`（脚本自己 try/catch 吞掉的
      失败不该被当成整体失败，但也不能从诊断里消失）。
  - 回归 M0/M1/M1a/M1c/M2a/M2b/M2c 全绿；静态 305 PASS / 0 FAIL；
    `libtjs2_ios.a` 1,387,328 B / **36 objs**（+1 = tjsRegExp.o）；
    `ARCHIVE SUCCEEDED`，app 二进制 893,184 B，五个标记
    （`M1C_HOST_v1` / `M2A…` / `M2B…` / `M2C…` / `M2D_REGEXP_STDREGEX`）全命中。
  - ⚠️ 踩坑（run #30/#31，供下一轮直接用）：
    * `lastResolvedPath` 是**最后一次**解析结果，不能用它判「某个脚本是否被解析」——
      M2d 让 `Initialize.tjs` 继续加载 `Utils.tjs` 后它就变成了 `Utils.tjs`。
      判「Config.tjs 真执行过」要走因果链：`global.config_version` 未定义时会先去
      加载 `UpdateConfig.tjs`，那一步就失败了，根本走不到 `createAppLock`。
    * `execStorageAttempts` 计在**存在性检查之后**，缺失的 `KAGLayer.tjs` 不计入，
      所以 startup+Config+Utils = 3 而不是 4。
- **M2c** ✅（run #28 一次全绿）：官方启动链第三段 —— `System` / `Debug` 原生类，
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
       断言 `execStorageAttempts=1` 且 `execStorageCalls=0`（调用过但内层抛异常 ——
       两个计数必须分开，否则「跑了没跑」分不清，run #24 就是踩了这个）、
       `autoPathCount=11`、`debugNotices=3`、`tickCalls≥1`、
       `exceptionHandlerSet=true`、`appLock=false`、`fatalLine>0`、
       `fatalException` 含 `Config.tjs`、`lastEncoding=cp932`。
       ⚠️ run #26 实测 `fatalBlock` 是 **`startup.tjs:6`**（execStorage 调用点），
       `Initialize.tjs` 的真定位丢了 —— 同一个失败被捕获两次（execStorage 内层抓到
       真异常 → 返回 `TJS_E_FAIL` → TJS2 转成笼统的 `Unknown failure : FFFFFFFF`
       抛给外层），第二次无条件覆盖把精确定位冲掉。已改为「先存局部变量，再按
       不覆盖更具体信息提交」。run #27 验证生效：`fatalBlock=system/Initialize.tjs`、
       `fatal="script exception : Config.tjs が見つかりません。"`。
       ⚠️ 但 `fatalLine` 对「跨原生调用抛出的异常」**不可靠**：#27 实测报 280
       （文件共 280 行，即末行），而 `throw` 实际在第 148 行附近。
       所以断言只要求 `fatalLine > 0` + block 指向某个 `.tjs`，不写死行号。
    B) 加一个**探针版** `Config.tjs`（只写 `config_version = "3.29-dev.20071022"`，明确标注非官方）
       （run #26/#27 两次踩坑：TJS2 里**裸赋值不创建成员** —— 写
       `config_version = "..."` 会当场抛 `Member "config_version" does not exist`
       并带 `fatalBlock=Config.tjs fatalLine=20`；必须写成 `global.config_version`，
       与官方 `global.kag = new KAGWindow()` 同一套语义。
       #26 曾误判成「`kagVersion` 是块内变量读不到」，#27 换成字面量后报同一个错，
       才确认是赋值目标本身，不是右值。）
       → 版本核对通过、跳过 `UpdateConfig.tjs`，执行推进到
       `System.createAppLock(System.exePath.replace(/[^A-Za-z]/g,'_'))`，
       因全局 `RegExp` 缺失而失败。断言 `execStorageAttempts=2` /
       `execStorageCalls≥1`、`lastResolvedPath=system/Config.tjs`、`debugMessages≥1`
       （证明原生方法当函数值调用 `dm(...)` 真的生效），且失败原因含 `RegExp`
       —— 这就是**下一个阻塞点的硬证据**。
  - app 侧：`GameViewController` 打开含 `startup.tjs` 的包后，**在后台队列**
    跑一次 `tjs2ExecStartup`，把「autoPath / notice / tick 次数 + 停在哪一句」
    做成第 4 秒的 toast 显示出来（主线程同步执行曾导致导入卡死，不重犯）。
  - **run #28 实测（macOS host 真跑）**：
    A) `{"ok":false,"execStorageAttempts":1,"execStorageCalls":0,
       "autoPathCount":11,"debugNotices":3,"tickCalls":1,
       "exceptionHandlerSet":true,"appLock":false,"exePath":"",
       "lastEncoding":"cp932",
       "lastDebug":"Kirikiri : TJS2 2.4.28 (GalBox)",
       "fatal":"script exception : Config.tjs が見つかりません。",
       "fatalBlock":"system/Initialize.tjs","fatalLine":280,
       "fatalException":"Config.tjs が見つかりません。"}`
    B) `{"ok":false,"execStorageAttempts":2,"execStorageCalls":1,
       "autoPathHits":1,"lastResolvedPath":"system/Config.tjs",
       "debugMessages":1,"lastDebug":"Config.tjs を読み込みました(0ms)",
       "fatal":"Member \"RegExp\" does not exist",
       "fatalBlock":"system/Initialize.tjs","fatalLine":164}`
    `execStorageCalls=1` + `lastDebug="Config.tjs を読み込みました"` 两件事同时成立，
    才真正证明「取原生方法当函数值调用」（`var dm = Debug.message; dm(...)`）可用 ——
    这是本轮唯一没法靠静态断言确认、必须真跑的语义。
    回归 `M0/M1/M1a/M1c/M2a/M2b` 全绿；静态 274 PASS / 0 FAIL；
    `ARCHIVE SUCCEEDED`，app 二进制 849,040 B，四个标记
    （`M1C_HOST_v1` / `M2A_SCRIPTS_EXEC_STORAGE` / `M2B_STORAGES_AUTOPATH` /
    `M2C_SYSTEM_DEBUG`）全部命中。
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
1. **M2e-4（原生层提供的常量 + 同进程状态泄漏）** —— 两件事：
   ① **补 `VK_*` 键码常量**（run #41 的新缺口：`Member "VK_SPACE" does not
      exist` @ `MainWindow.tjs:210`）。这批常量官方由原生层提供
      （`VK_SPACE` / `VK_RETURN` / `VK_LEFT` / `VK_PAD1` … 共 15 个被
      `MainWindow.tjs` 用到），Windows 虚拟键码有标准取值，直接补进
      `GalBoxWindow.tjs` 即可。同类还有 `AsyncTrigger`（`new AsyncTrigger(...)`，
      官方在 `core/base/EventIntf.cpp`）—— 那是真正的原生类，要单列。
   ② **查清「同进程内第二次执行会破坏官方启动链」**（run #40 现象，run #41
      隔离实验已定位为状态泄漏）。怀疑对象：`GalBoxTJS2ExecScript` 每次
      `new tTJS()` 但状态是进程级的（`TJSRegisterNativeClass` 的全局类表、
      `tTJSNativeClass::ClassID` 静态成员、字符串映射表）。做法：二分 ——
      先让场景 A 只 `execStorage` 不 `new KAGWindow()`，再逐项加回，看哪一步
      开始污染。
   ③ 修掉之后，把自测顺序改回 A → B → C 并**恢复**「脚本数不倒退」断言
      （run #41 为了做隔离实验暂时放宽成了只打印）。
2. **M2e-5 之后的接口约束**：`new KAGWindow()` 与
2. **M2e 之后的接口约束**：`new KAGWindow()` 与
   `kag.process("first.ks")` 需要的原生类，接 iOS run loop / CADisplayLink
   与渲染/音频回调面。宿主回调面继续走「纯 C 边界 + C++ 适配层」，
   不要把官方 `tp_stub` 头暴露给 ObjC++/Swift。
   ⚠️ app 已编译 `XP3Archive.cpp`，宿主层**只能链 `HostStreams_ios.o` /
   `HostBridge_ios.o`**，勿重复打包 XP3Archive.o。
   ⚠️ 商业 `startup.tjs` 的执行必须放后台队列，切勿在主线程同步调用。
3. **M3/M4**：TLG/TPP/WAV/OGG 解码、视频换 AVFoundation、产品化切换。

**当前边界**：`Scripts` / `Storages` / `System` / `Debug` / `RegExp` 五个原生类
+ GalBox 的 `Layer` / `Window` 骨架就位，官方 28 个 `system/` 文件全部按原始
字节在位。`Initialize.tjs` 原文能推过配置检查、算完二重起動键值，并真实读取
**15 个**系统脚本；根因推进到 `MainWindow.tjs:210` 缺 `VK_*` 键码常量，
**不是缺文件、也不是引擎缺语言能力**。`Layer` / `Window` 目前是**纯脚本骨架**，
不渲染、不解码图片 —— 能让官方脚本跑起来，但离出画面还差渲染/音频回调面、
事件循环与 `AsyncTrigger` 等原生类。⚠️ 另有一个未解现象：同进程内第二次执行
会破坏官方启动链（隔离实验已确认是状态泄漏，根因待查）。商业游戏仍不可运行。

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
