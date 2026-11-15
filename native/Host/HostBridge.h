// HostBridge.h — Milestone 1c：app 侧调用官方 TJS2 的纯 C 边界
//
// 为什么不直接在 GBEngineBridge.mm 里 include "tjs.h"：
//   官方 TJS2 头依赖一整套构建开关（-DTJS_NO_REGEXP / -DTJS2_POSIX /
//   -include tjs2_compat.h / stub/malloc.h），而 app target 是 ObjC++ + ARC。
//   两边编译选项一旦不一致，tjs_char / 类布局就会错位。
//   因此把「碰官方头」的代码全部关在 HostBridge.cpp 里，它由
//   build_tjs2_*.sh 用与核心库完全相同的一套 flags 编译；app 只见纯 C 函数。
//
// 二进制边界：app 链接 libtjs2_ios.a + HostStreams_ios.o + HostBridge_ios.o；
// XP3Archive.o 由 app target 自己编译，绝不重复打包。
//
// archive 用 void* 传递：它就是 GBEngineBridge.mm 里那个
// static galbox::XP3Archive 的地址，HostBridge.cpp 内 reinterpret_cast 回来。

#ifndef GALBOX_HOST_BRIDGE_H
#define GALBOX_HOST_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

/* 打开游戏包后安装宿主：把 4 个流工厂指到 XP3Archive + Documents + bundle。
   返回 1 表示宿主已安装；archive 为 NULL 时返回 0。 */
int  GalBoxTJS2Install(void *archive, const char *storageRoot,
                       const char *bundleRoot);

/* 卸载游戏包时恢复官方默认流工厂（返回 NULL）。 */
void GalBoxTJS2Uninstall(void);

/* 真跑一次 TJS2：构造引擎 → eval → Array.load(probeFile) 回调宿主读 →
   Array.save 回调宿主写。返回 UTF-8 JSON 字符串（malloc），由调用方 Free。
   probeFile 可传 NULL 或 ""，此时跳过读回调检查。 */
char *GalBoxTJS2SmokeTest(const char *probeFile);

/* Milestone 2a: execute a .tjs entry script from the game package.
   The official startup.tjs calls Scripts.execStorage(...), which is registered
   as a TJS2 native static class by this layer and loads further .tjs files
   through the GalBox host stream. probeExpression (optional, UTF-8 TJS2 code)
   is evaluated afterwards to prove functions from the loaded script work.
   Returns UTF-8 JSON (malloc), caller frees via GalBoxTJS2Free. */
char *GalBoxTJS2ExecScript(const char *entryName, const char *probeExpression);

/* Milestone 2c: tell TJS2 the real screen size (System.screenWidth /
   desktopWidth). Optional; defaults to 1280x720 until the UI layer calls it. */
void GalBoxTJS2SetScreenSize(int width, int height);

void GalBoxTJS2Free(char *p);

/* 供 UI 判断本包是否真的链接了 TJS2（无库时降级出包会返回 0）。 */
int  GalBoxTJS2Available(void);

#ifdef __cplusplus
}
#endif

#endif  /* GALBOX_HOST_BRIDGE_H */
