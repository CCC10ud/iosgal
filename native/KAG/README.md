# native/KAG — 官方 KAG3 层（Milestone 2）

> 2026-11-15 更正：早期本文件写的上游 `kikikracle/kirikiri2` 是 **404**，
> 正确仓库是 **`jeeb/kirikiri2` @ master**。另外「KAG 系统脚本随游戏提供、
> 不入库」也不准确 —— 官方仓库自带 `kag3/template/system/` 的 28 个 `.tjs`
> （448,729 B），可以直接 vendoring。

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
- **M2b**：补 `Storages` / `System` 等原生类 + vendoring `Initialize.tjs` 及其依赖链，
  让官方 `startup.tjs` 能一路执行到 KAG 初始化；需要事件队列（iOS run loop / CADisplayLink）。
- **M2c**：`KAGParserEx` / `KAGParser` 原生类 + 渲染/音频回调面（文本、层、图像、转场），
  把 `GameViewController` 接到官方 KAG 播放循环，取代手写 `KagEngine`。
- **M3/M4**：TLG/TPP/WAV/OGG 解码、视频换 AVFoundation、产品化切换。

## 当前目录

- `src/startup.tjs`、`src/system/Utils.tjs`：官方原文，逐字节（含 CRLF、版权头）。
- `src/m2a_probe_startup.tjs`：GalBox 自写探针，仿官方调用形态但指向 `Utils.tjs`，
  避开尚未移植的 `Initialize.tjs` 依赖链。
- `VENDOR_MANIFEST.json`：来源、字节数、SHA-256、编码、未 vendoring 清单。

## 授权

KAG3 模板脚本的文件头是「Copyright (C) 2001-2009, W.Dee **改変・配布は自由です**」
（可自由修改、分发），与 TJS2 核心的**双许可证**（吉里吉里独自许可 / GNU GPL 二选一）
不同，见 `../TJS2/LICENSE.txt`。上架 App Store 前须分别评估。

## 设计约束

- 宿主边界继续走 `../Host/HostBridge.{h,cpp}` 的「纯 C 接口 + C++ 适配层」，
  官方 `tp_stub.h` 一律不暴露给 ObjC++ / Swift。
- 文本流已支持 UTF-8 / UTF-16LE BOM / SHIFT_JIS，官方 KAG3 脚本是 UTF-8，
  但商业游戏自带的 `system/` 常是 Shift-JIS，M2b 要实测覆盖。
- `startup.tjs` 的执行**不要放在 UI 主线程**（Initialize.tjs 可能长循环/等待），
  M2b 接线时必须走后台队列。
