# native/KAG — 官方 KAG 插件 vendoring 落点（Milestone 2）

**状态：待 vendoring（Milestone 2 执行，依赖 Milestone 0/1 完成）。**

从官方仓库 `kikikracle/kirikiri2` 拉取：

- 源：`src/plugins/win32/kag/` 的 KAG 解析器/播放器 → 落到本目录 `src/`
- 游戏 `system/` 系统脚本（`System.tjs` 等）随具体游戏提供，**不入库**

KAG 是建在 TJS2 之上的 TJS2 库，必须随 TJS2 真实运行。落地后：
- 启动改为由 `startup.tjs`（TJS2）走官方路径加载 KAG，取代 GalBox 手写 `KagEngine`。
- 实现 KAG→渲染/音频宿主回调（见 `../Host/HostInterface.hpp` 的渲染/音频 TODO），后端即现有 Swift/ObjC++ 层。

注意：KAG 含 Windows 位图/字体/GDI 耦合，需按需 shim；文本层用 CoreText，图像用 CGImage。
