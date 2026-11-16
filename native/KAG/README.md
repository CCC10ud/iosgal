# native/KAG — 官方 KAG3 层（Milestone 2）

> 2026-11-15 更正：早期本文件写的上游 `kikikracle/kirikiri2` 是 **404**，
> 正确仓库是 **`jeeb/kirikiri2` @ master**。另外「KAG 系统脚本随游戏提供、
> 不入库」也不准确 —— 官方仓库自带 `kag3/template/system/` 28 个文件
> （其中 20 个 `.tjs`，合计 448,729 B），M2e-1 已按 git blob 原始字节全量 vendoring。
>
> 2026-11-15 二次更正（M2b）：**官方 git blob 里的脚本是 Shift-JIS + CRLF，不是 UTF-8**。
> GitHub contents API 会静默转码成 UTF-8，M2a 说「逐字节 vendoring 官方原文」是错的。
> 现在 `src/` 下是「M2a 用到的 UTF-8 归一化副本」，`src/original/` 下才是
> 按 `/git/blobs/<sha>` 取的**官方原始字节**（sha/大小见 `VENDOR_MANIFEST.json`）。
> 商业游戏的 `system/` 恰恰是 Shift-JIS，所以 M2b 的自测改用原始字节跑。

## 官方 KAG 的真实结构（按仓库文件树核对）

| 层 | 位置 | 规模 |
|---|---|---|
| 启动入口 | `kag3/template/startup.tjs` | 288 B，唯一有效语句 `Scripts.execStorage("system/Initialize.tjs");` |
| KAG3 系统脚本层 | `kag3/template/system/` | **28 个文件 / 448,729 B**：20 个 `.tjs`（Shift-JIS + CRLF）+ `Config.~new` + 7 个 png/asd |
| KAG 解析器 C++ 插件 | `kirikiri2/src/plugins/win32/KAGParserEx/` | `KAGParser.cpp` 83 KB 等 6 个文件 |
| 平台层 tp_stub | `kirikiri2/src/plugins/win32/tp_stub.{h,cpp}` | 486 KB + 79 KB |
| 核心平台层 | `kirikiri2/src/core/`（base / visual / environ / sound / utils / msg / tjs2） | 4,120 文件 / 38.7 MB |

**关键结论**：`startup.tjs` 不是 KAG 代码，它只是让 TJS2 调一个**原生类** `Scripts.execStorage`。
真正的系统脚本又依赖 `Layer` / `Window` / `System` / `Storages` 等原生类，
这些由 `core/base/ScriptMgnIntf.cpp`、`core/visual/LayerIntf.cpp`(293 KB) 等提供，
且整体绑在 Windows 的 `tp_stub` 上。⇒ **完整移植不是一次能完成的动作，必须拆里程碑。**

## M2 拆分

- **M2a ✅（run #20 全绿）**：vendoring 官方 `startup.tjs` + 纯 TJS2 的 `system/Utils.tjs`；
  在 `HostBridge.cpp` 里注册 TJS2 原生静态类 `Scripts`，实现 `execStorage`，
  让 TJS2 能从游戏包读 `.tjs` 并递归执行。自测用目录模式归档真跑官方 `Utils.tjs`
  （含 `property` 块、`<->` 交换运算符、日文注释），探针 `intrandom(5,5) == 5`。
  这是**官方启动链第一段**，不是完整 KAG。
- **M2b ✅（run #22 全绿）**：官方启动链第二段 —— `Storages` 自动路径（官方
  `Initialize.tjs` 开头连调 13 次 `addAutoPath`，后面所有 `Scripts.execStorage`
  都用短文件名，必须靠 autoPath 展开）+ **Shift-JIS 原始字节真解码**。
  同时 vendoring `Initialize.tjs` 原文作为依赖分析依据（不执行）。
  实测：`lastEncoding=cp932`、`autoPathHits=1`、
  `lastResolvedPath=system/Utils.tjs`、`str2num("１２３")==123`、
  错误 autoPath 明确失败；`M2B_RESULT=OK`。
- **M2c ✅（run #28 全绿）**：官方启动链第三段，且**第一次真跑官方
  `Initialize.tjs` 原文**（前两轮跑的都是自写探针）。实现 `System` / `Debug`
  两个原生静态类，逐条对齐脚本里的真实语句（`exceptionHandler` / `exePath` /
  `getTickCount` / `inform` / `createAppLock` / `getArgument` / `notice` /
  `message` 等）。结论：脚本能一路执行到
  `System.createAppLock(System.exePath.replace(/[^A-Za-z]/g,'_'))`，
  在那里因全局 `RegExp` 缺失而停 —— `TJS_NO_REGEXP` 只影响类注册，不影响词法解析，
  所以是**运行期**失败而非编译期失败。下一个阻塞点因此是正则，不是 System/Debug。
- **M2d ✅（run #32 全绿）**：正则。**不引 boost**，把官方 `tjsRegExp.cpp`
  用 `std::regex` 整份重写（`native/TJS2/PATCHES.md` P7），并去掉官方
  `TJS_NO_REGEXP` 开关 —— 于是 `tjs.cpp` 注册全局 `RegExp`、`tjsArray.cpp`
  的 regexp-split 分支恢复编译，上游其它文件一行不用改。
  判据：`m2d_regex_probe.tjs`（23 条语义检查）+ 官方 `Initialize.tjs` 原文
  推过 `System.exePath.replace(/[^A-Za-z]/g,'_')`，`appLockKey` 与 C++ 独立
  逐字符实现比对一致。执行继续推进到 `KAGLoadScript("KAGLayer.tjs")`（夹具里
  故意没有该文件）——下一个缺口因此从「引擎缺正则」变成「M2e 需要 Layer/Window
  等系统脚本与原生类」。
- **M2e-1 ✅（run #34 一次全绿）**：把 `kag3/template/system/` 的 **28 个上游文件**
  （20 个 `.tjs` + `Config.~new` + 7 个 png/asd，448,729 B）按 `/git/blobs/<sha>`
  **原始字节**全量 vendoring 到 `src/original/system/`。目的不是跑通 KAG，而是把
  run #32 的「缺文件」阻塞点逼成「缺原生类」：`KAGLayer.tjs` 第 87 行
  `class KAGLayer extends Layer` 里的 `Layer` 尚未实现。
  - 保真判据：tree API 列 28 个 blob → 逐个用 sha1(`blob <len>\0`+raw) 重算比对；
    构建脚本再按 `VENDOR_MANIFEST.json` 的 sha256 + bytes 逐文件校验；
    `test_m2e.cpp` 对夹具里 28 个文件校验精确字节数。
  - TJS2 侧判据：`m2e_manifest_probe.tjs` 按官方方式 `addAutoPath("system/")`
    后用短名查 20 个 `.tjs`，另设一个必然不存在的哨兵 →
    `m2eProbe = found*100 + bad`，期望恰好 **2000**（防「存在性检查恒真」）。
  - 启动链判据：新增 `scriptTrace`，**只在流成功打开并读到正文之后**追加短名。
    场景 B 断言 trace 含 `KAGLayer.tjs`、错误里不再有 `script not found`、
    报错块在 KAGLayer 内部；场景 C 是**负向对照**（故意只放旧 4 个文件），
    断言 trace 不含它且错误回到 `script not found`。没有 C，B 就是恒真式自证。
  - **run #34 实测**：A) `probeValue=2000`（20/20 可见、哨兵未命中、`scriptsRead=0`）；
    B) `scriptsRead=10 / attempts=10 / calls=8 / shiftJisReads=10 / cp932`，
    `scriptTrace` 一路到 `AnimationLayer.tjs`，
    `fatal="Member \"Layer\" does not exist" @ KAGLayer.tjs:87`；
    C) 负向对照 trace 只有 3 个脚本，错误回到 `script not found`。
    ⚠️ 根因在 `KAGLayer.tjs:87`，但加载链在该行之后**还继续读了 7 个脚本**，
    最外层冒泡的 execStorage 上下文是 `AnimationLayer.tjs` —— 所以
    `lastScriptError` / `lastResolvedPath` 都不能当「停在哪一句」，只有
    `fatalBlock + fatalLine` 可靠。
  - 顺带修一处会让判据说谎的地方：`Install()` 此前**没有**清零 `shiftJisReads`，
    后一次启动会「借」上一次的解码计数 —— 已归零。
- **M2e-2 ✅（run #36 一次全绿）**：**GalBox 自写的 `Layer` 骨架**。
  官方 `Layer` 是 `kirikiri2/src/core/visual/LayerIntf.cpp` 的**原生类**
  （293 KB，绑死 Windows tp_stub），而 KAG 的 20 个系统脚本里有 **18 个**
  `extends Layer` —— 没有它一个都跑不起来。本轮用一份**纯 TJS2 的骨架**
  （`src/system/GalBoxLayer.tjs`，明确标注非官方）先顶上：提供 `class Layer`
  的占位成员，以及官方脚本直接引用的全局常量
  （`clNone` / `clAdapt` / `stt*` / `stst*`）。它**不渲染、不解码图片、不转场**，
  只负责让继承成立，把下一批缺失成员逼出来。
  - **关键机制（TJS2 的 `super` 是惰性的）**：`class X extends Y` 这条语句
    **不去查** `Y`，引擎把它编译成一个 super proxy，只有在**实例化**或访问
    父类成员时才求值。所以「脚本被成功读完」≠「继承成立」—— 必须真的
    `new` 一次。run #34 里 `KAGLayer.tjs` 进了 `scriptTrace` 却仍报 Layer 缺失，
    就是这个原因。
  - 判据三场景（都真跑）：A) `m2f_layer_probe.tjs` 读入官方 `KAGLayer.tjs`
    原文后 `new KAGLayer(null,null)` 走通 → `global.m2fProbe=1`，`probeValue`
    必须恰好 1（-1 = 实例化抛异常）；B) 完整官方启动链：仍要求 `ok=false`、
    错误里不再有 Layer 缺失、加载脚本数不比 M2e-1 的 10 个倒退；
    C) **负向对照**：同一套夹具但**不放**骨架，必须复现 run #34 的
    `Member "Layer" does not exist` @ `KAGLayer.tjs:87`。
  - **run #36 实测**：A) `probeValue=1`、`shim=M2E2_LAYER_SKELETON`、
    `scriptTrace="KAGLayer.tjs"`（官方 SJIS 原文，`lastEncoding=cp932`）；
    B) 官方脚本加载数 **10 → 15**（`Initialize, Config, Utils, KAGLayer,
    HistoryLayer, BGM, SE, Movie, Conductor, AnimationLayer, GraphicLayer,
    MessageLayer, Menus, DefaultMover, MainWindow`），
    新阻塞点 `Member "Window" does not exist` @ `MainWindow.tjs:7`
    ⇒ **下一个原生类是 `Window`**；C) `shim="absent"`、`probeValue=-1`、
    复现 `Member "Layer" does not exist` @ `KAGLayer.tjs:87`。
  - 宿主侧：`HostBridge` 新增 `LoadEngineShim()`，在跑入口脚本前加载
    `system/GalBoxLayer.tjs`（游戏包里没有就跳过，状态走 JSON 的 `shim` 字段）。
    刻意**不走** `Scripts.execStorage` —— 那会污染 `execStorageAttempts` /
    `scriptTrace` / `lastScriptError` 三条判据；也不调 `CaptureCurrentException`，
    骨架自己的失败不该占用 `fatalBlock`/`fatalLine`。
- **M2e-3 起**：
  渲染/音频回调面 + 事件队列，把 `GameViewController` 接到官方 KAG 播放循环，
  取代手写 `KagEngine`。
- **M3/M4**：TLG/TPP/WAV/OGG 解码、视频换 AVFoundation、产品化切换。

## 当前目录

- `src/startup.tjs`、`src/system/Utils.tjs`：M2a 使用的 **UTF-8 归一化副本**
  （GitHub contents API 转码版，含 CRLF、版权头；**不是**官方原始字节）。
- `src/original/startup.tjs` + `src/original/system/`（**28 个文件**）：
  按 git blob sha 取的**官方原始字节**（`.tjs` 为 Shift-JIS + CRLF，png/asd 为二进制），
  与上游逐字节一致；每份的 blob sha / SHA-256 / 字节数都在 `VENDOR_MANIFEST.json`。
  `Initialize.tjs` 自 M2c 起作为自测入口**原文真跑**（不经任何改写）。
  这些是官方模板脚本，商业游戏自带自己的 `system/` 副本，因此**不打进 app bundle**，
  只在自测夹具里使用；app 里保留的是 `M2E_SYSTEM_SCRIPTS_VENDORED` 二进制标记。
- `src/m2a_probe_startup.tjs`、`src/m2b_probe_startup.tjs`、
  `src/m2d_regex_probe.tjs`、`src/m2e_manifest_probe.tjs`、
  `src/m2f_layer_probe.tjs`：GalBox 自写探针。
- `src/system/GalBoxLayer.tjs`：GalBox 自写的 **`Layer` 骨架**（**非官方**，
  M2e-2 起）。放在 `src/system/` 而不是 `src/original/system/`，就是为了和
  官方原文泾渭分明 —— 它是过渡件，不是官方文件。
- `src/m2c_probe_config.tjs`：M2c 用的**探针版** `Config.tjs`（只写
  `config_version = kagVersion`），用来跳过 `UpdateConfig.tjs`、把执行
  精确推到 `createAppLock` 的正则处。**不是官方文件**，不进 `src/original/`。
- `VENDOR_MANIFEST.json`：两套副本的来源、字节数、SHA-256、blob sha、
  编码差异说明、M2e-1 全量 vendoring 的方法与总字节、未 vendoring 清单。

## M2b 依赖分析（`Initialize.tjs` 原文）

按 20 个官方系统脚本统计，启动链需要的原生类调用面（次数）：

| 类 | 主要成员 |
|---|---|
| `Scripts` | `eval` 31、`exec` 4、`evalStorage` 4、`execStorage` 1、`dump` 1 |
| `Storages` | `isExistentStorage` 25、`addAutoPath` 13、`chopStorageExt` 6、`getPlacedPath` 3、`getLocalName` 2、`selectFile` 2 等 |
| `System` | `getTickCount` 24、`exePath` 12、`inform` 9、`title` 7、`screenWidth/Height` 8、`createAppLock` 1、`exceptionHandler` 1 等 |
| `Debug` | `notice` 5、`message` 3、`logAsError` 2 等 |

`Initialize.tjs` 会依次 `KAGLoadScript` 加载 14 个系统脚本，最后
`global.kag = new KAGWindow()` 并 `kag.process("first.ks")` —— 这一步需要
`Layer` / `Window` / `KAGParserEx` 与渲染回调面属于 M2e；M2c 的自测在
`createAppLock`（正则）处就停了，因此**没有**跑到这一步。

## 授权

KAG3 模板脚本的文件头是「Copyright (C) 2001-2009, W.Dee **改変・配布は自由です**」
（可自由修改、分发），与 TJS2 核心的**双许可证**（吉里吉里独自许可 / GNU GPL 二选一）
不同，见 `../TJS2/LICENSE.txt`。上架 App Store 前须分别评估。

## 设计约束

- 宿主边界继续走 `../Host/HostBridge.{h,cpp}` 的「纯 C 接口 + C++ 适配层」，
  官方 `tp_stub.h` 一律不暴露给 ObjC++ / Swift。
- 文本流支持 UTF-8 / UTF-16LE BOM / Shift-JIS（libiconv，优先 CP932）。
  **官方 KAG3 与商业游戏的 `system/` 都是 Shift-JIS**；M2b 的自测已用官方
  原始字节真跑过这条解码链，`HostStreams.cpp` 会记录实际采用的编码，
  不准靠「没抛异常」自证解码成功。
- `startup.tjs` 的执行**不要放在 UI 主线程**（Initialize.tjs 可能长循环/等待），
  M2b 接线时必须走后台队列。
