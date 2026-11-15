# GalBox → 完整 Kirikiri 运行时：移植方案与架构

> 状态：**路线 A 已确认（完整移植官方 TJS2 + KAG）**。本回合因 GitHub 全站及镜像被本地代理拦截（`http=000`），**无法拉取上游源码、也无法推送**。本文档与 `native/Host/HostInterface.hpp` 骨架为离线产出的第一阶段设计物，网络恢复后即从 Milestone 0 落地。

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
- 写一个极简 host + `eval("1+1")` 自测，证明引擎在 **arm64 iOS** 上能跑。
- ✅ 出口：Mac runner 上 `libtjs2.a` 编出、自测打印 `2`。

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

核心是两个契约：
- `iTJS2Host`：引擎层回调（`Print` / `Sleep` / `GetMilliseconds` / `GetLocalName` / `GetLanguage` / `ShowMessageBox` / `RequestQuit` / `OnError`）。
- `iTVPHost`（tp_stub 风格）：平台层（`iTVPFileSystem` 文件流、`TVPGetTickCount`、`TVPPrint`、`TVPThrowExceptionMessage`、`TVPPostEvent`/`TVPProcessEvents` 事件队列、`TVPGetAppPath`、随机熵）。

GalBox 的 iOS 实现把上述映射到：`XP3Archive`+`NSFileManager`、`mach_absolute_time`、`os_log`、iOS run loop + `CADisplayLink`、`Documents` 路径。

## 5. Vendoring 计划（网络恢复后第一步执行）

需从官方仓库拉取（默认 `kikikracle/kirikiri2`，镜像备选）：
- `src/core/tjs2/` → `native/TJS2/src/`
- `src/core/tp_stub.h` 及最小依赖 → `native/Host/`（或 `native/TJS2/`）
- `src/plugins/win32/kag/` → `native/KAG/src/`（Milestone 2）
- 游戏 `system/` 脚本随具体游戏提供，不入库。

**授权**：Kirikiri2/TJS2 为 BSD 风格宽松许可，允许再分发与修改，但**必须保留版权与 LICENSE 声明**。vendoring 时一并拉取 `LICENSE`/`COPYRIGHT` 并置于 `native/TJS2/` 内，勿删头注释。

## 6. 风险与未知
- TJS2 源码中仍有少量 Windows 耦合（线程/SEH/调试输出），需逐文件 shim，可能踩坑 → Milestone 0 会暴露。
- KAG 宿主回调面很大（文本/图像/音频/层/转场/输入），Milestone 2 工作量最高。
- 视频（DirectShow）在 iOS 无对应，需 AVFoundation 重写。
- 验证只能靠 Mac runner，迭代慢；每个里程碑拆分到"一次推送即可验证"的粒度。

## 7. 当前阻塞与下一步
- **阻塞**：GitHub 全站 + 镜像 `http=000`，无法 fetch/push（代理层拦截，非仓库问题）。
- **下一步（网络恢复立即执行）**：
  1. 拉取 `tjs2/` 源码与 LICENSE 到 `native/TJS2/`。
  2. 写 Windows→POSIX shim，接入 Xcode 静态库。
  3. 推送触发 Mac runner，跑 Milestone 0 自测。
  4. 回到本计划推进 Milestone 1。
