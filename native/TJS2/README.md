# native/TJS2 — 官方 TJS2 引擎 vendoring 落点（Milestone 0）

**状态：待 vendoring（网络恢复后拉取）。**

从官方仓库 `kikikracle/kirikiri2` 拉取（镜像备选）：

- 源：`src/core/tjs2/` 全部 `.cpp` / `.h` → 落到本目录 `src/`
- 依赖：`src/core/tp_stub.h` 及最小依赖 → 见 `../Host/`
- 授权：`LICENSE` / `COPYRIGHT` 一并拉入，**保留头注释与版权声明**（BSD 风格宽松许可）

编译：作为静态库 `libtjs2.a` 接入 Xcode（c++14），剥离 `<windows.h>`/`TCHAR`/`OutputDebugString`/`__try` 等，用 POSIX shim 替换。

自测（Milestone 0 出口）：极简 host + `eval("1+1")` 打印 `2`，证明 arm64 iOS 可运行。
