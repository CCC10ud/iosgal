# native/KAG — 官方 KAG3 层（Milestone 2）

> 2026-11-15 更正：早期本文件写的上游 `kikikracle/kirikiri2` 是 **404**，
> 正确仓库是 **`jeeb/kirikiri2` @ master**。另外「KAG 系统脚本随游戏提供、
> 不入库」也不准确 —— 官方仓库自带 `kag3/template/system/` 的 28 个 `.tjs`
> （448,729 B），可以直接 vendoring。
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
| KAG3 系统脚本层 | `kag3/template/system/*.tjs` | 28 个 `.tjs` / 448,729 B（UTF-8 + CRLF，日文注释） |
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
- **M2c（已实现，待 CI 验证）**：官方启动链第三段，且**第一次真跑官方
  `Initialize.tjs` 原文**（前两轮跑的都是自写探针）。实现 `System` / `Debug`
  两个原生静态类，逐条对齐脚本里的真实语句（`exceptionHandler` / `exePath` /
  `getTickCount` / `inform` / `createAppLock` / `getArgument` / `notice` /
  `message` 等）。结论：脚本能一路执行到
  `System.createAppLock(System.exePath.replace(/[^A-Za-z]/g,'_'))`，
  在那里因全局 `RegExp` 缺失而停 —— `TJS_NO_REGEXP` 只影响类注册，不影响词法解析，
  所以是**运行期**失败而非编译期失败。下一个阻塞点因此是正则，不是 System/Debug。
- **M2d（已实现，待 CI 验证）**：正则。**不引 boost**，把官方 `tjsRegExp.cpp`
  用 `std::regex` 整份重写（`native/TJS2/PATCHES.md` P7），并去掉官方
  `TJS_NO_REGEXP` 开关 —— 于是 `tjs.cpp` 注册全局 `RegExp`、`tjsArray.cpp`
  的 regexp-split 分支恢复编译，上游其它文件一行不用改。
  判据：`m2d_regex_probe.tjs`（23 条语义检查）+ 官方 `Initialize.tjs` 原文
  推过 `System.exePath.replace(/[^A-Za-z]/g,'_')`，`appLockKey` 与 C++ 独立
  逐字符实现比对一致。执行继续推进到 `KAGLoadScript("KAGLayer.tjs")`（夹具里
  故意没有该文件）——下一个缺口因此从「引擎缺正则」变成「M2e 需要 Layer/Window
  等系统脚本与原生类」。
- **M2e**：`Layer` / `Window` / `KAGParserEx` 原生类 + 渲染/音频回调面 +
  事件队列，把 `GameViewController` 接到官方 KAG 播放循环，取代手写 `KagEngine`。
- **M3/M4**：TLG/TPP/WAV/OGG 解码、视频换 AVFoundation、产品化切换。

## 当前目录

- `src/startup.tjs`、`src/system/Utils.tjs`：M2a 使用的 **UTF-8 归一化副本**
  （GitHub contents API 转码版，含 CRLF、版权头；**不是**官方原始字节）。
- `src/original/{startup.tjs,system/Utils.tjs,system/Initialize.tjs}`：
  按 git blob sha 取的**官方原始 Shift-JIS 字节**，与上游字节完全一致。
  `Initialize.tjs` 自 M2c 起作为自测入口**原文真跑**（不经任何改写）。
- `src/m2a_probe_startup.tjs`、`src/m2b_probe_startup.tjs`：GalBox 自写探针。
- `src/m2c_probe_config.tjs`：M2c 用的**探针版** `Config.tjs`（只写
  `config_version = kagVersion`），用来跳过 `UpdateConfig.tjs`、把执行
  精确推到 `createAppLock` 的正则处。**不是官方文件**，不进 `src/original/`。
- `VENDOR_MANIFEST.json`：两套副本的来源、字节数、SHA-256、blob sha、
  编码差异说明、未 vendoring 清单。

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
