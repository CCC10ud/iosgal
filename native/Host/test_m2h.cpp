// test_m2h.cpp — Milestone 2e-5 自测：夹具换成官方 Config.tjs + 同进程状态泄漏探测
//
// run #45 的新缺口是 `Member "KAGWindow_config" does not exist`
// @ MainWindow.tjs:273。查清了：**KAGWindow_config 定义在官方 `Config.~new`
// 第 51 行** —— 它属于**游戏自带的 Config.tjs**，不在这 20 个系统脚本里。
// 我们的夹具用的是 M2c 那份「只有一行版本号」的探针版 Config，所以缺它。
// ⇒ 这是**夹具不够真实**，不是引擎缺陷。
//
// 本轮把夹具换成官方 `Config.~new` 的**原始 SJIS 字节**（前面插一行 ASCII 的
// global.config_version 以跳过 UpdateConfig.tjs 分支；ASCII 在 CP932 里恒等，
// 混排安全），让夹具第一次接近真实游戏。
//
// ⚠️ **每个场景跑在独立进程里**（argv[1] 选场景，构建脚本分别调用）。
// 原因：run #40 出现过「同进程内第二次执行会破坏官方启动链」
// （autoPathCount=0 / trace 只 1 项 / appLock=false，诊断自相矛盾）——
// 若三个场景挤在一个进程里，后面的场景会被前面的污染，负向对照就会**假红**。
// 独立进程 ⇒ 每个场景都是干净首跑，判据才可信。
//
// 场景：
//   A)    官方 Config.tjs 的完整启动链（干净进程）：
//         硬判据 —— run #45 的 KAGWindow_config 缺口必须消失，且加载脚本数
//         不倒退（>=15）。
//   B)    负向对照（同一套夹具，但 Config.tjs 换回探针版，干净进程）：
//         必须复现 `Member "KAGWindow_config" does not exist` —— 没有这一条，
//         A 就是恒真式自证。
//   LEAK  同一个进程里连跑两次官方链，打印两次诊断并给出 verdict
//         （LEAK REPRODUCED / no leak）。只诊断，不断言。
//   LEAK2 同 LEAK，但**先**跑一个会抛异常的脚本，再跑官方链 ——
//         用来区分「异常展开污染了全局栈」与「多引擎本身就有问题」。
//   NOMENU 负向对照（M2e-7）：同一套官方 Config，只去掉 Menu 骨架 ——
//         必须复现菜单子系统缺口（Menu / MenuItem），否则 A 的「跨过
//         MenuItem」就是恒真式自证。
//   NOSTYLE 负向对照（M2e-8）：只删掉 Window 骨架里的三个样式成员 ——
//         必须复现 `Member "borderStyle" does not exist`。
//
// A/B/NOMENU/NOSTYLE 全部通过打印 M2H_RESULT=OK。
#include "HostBridge.h"
#include "XP3Archive.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

static int Fail(const char *why, const char *json) {
    std::printf("[m2h] FAIL: %s\n[m2h] json=%s\n", why, json ? json : "(null)");
    std::printf("M2H_RESULT=ERR\n");
    return 3;
}

static bool Has(const char *json, const char *needle) {
    return json && needle && std::strstr(json, needle) != NULL;
}

static long long IntField(const char *json, const char *key) {
    if (!json || !key) return -1;
    std::string pattern = std::string("\"") + key + "\":";
    const char *p = std::strstr(json, pattern.c_str());
    if (!p) return -1;
    return static_cast<long long>(std::atoll(p + pattern.size()));
}

static std::string StrField(const char *json, const char *key) {
    std::string out;
    if (!json || !key) return out;
    std::string pattern = std::string("\"") + key + "\":\"";
    const char *p = std::strstr(json, pattern.c_str());
    if (!p) return out;
    p += pattern.size();
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            ++p;
            switch (*p) {
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                default:  out += *p;   break;
            }
            ++p;
        } else {
            out += *p++;
        }
    }
    return out;
}

static bool Exists(const std::string &path) {
    std::ifstream f(path.c_str(), std::ios::binary);
    return f.good();
}

static int OpenDir(const char *dir, galbox::XP3Archive &arc) {
    std::string err;
    if (!arc.open(dir, &err)) {
        std::printf("[m2h] open folder failed: %s (%s)\n", dir, err.c_str());
        return 0;
    }
    if (!arc.isDirectoryMode()) {
        std::printf("[m2h] %s is not a directory-mode archive\n", dir);
        return 0;
    }
    std::printf("[m2h] opened %s: entries=%zu dirMode=1\n", dir, arc.count());
    return 1;
}

static int CountTrace(const std::string &trace) {
    if (trace.empty()) return 0;
    int n = 1;
    for (size_t i = 0; i < trace.size(); ++i)
        if (trace[i] == ',') ++n;
    return n;
}

static void DumpChain(const char *tag, const char *j) {
    std::printf("[m2h] %s fatalBlock=\"%s\" (line %lld)\n", tag,
                StrField(j, "fatalBlock").c_str(), IntField(j, "fatalLine"));
    std::printf("[m2h] %s fatal=\"%s\"\n", tag, StrField(j, "fatal").c_str());
    std::printf("[m2h] %s inner=\"%s\"\n", tag,
                StrField(j, "lastScriptError").c_str());
    std::printf("[m2h] %s scriptsRead=%lld attempts=%lld calls=%lld "
                "autoPathCount=%lld autoPathHits=%lld hostReadDelta=%lld "
                "appLock=%s\n",
                tag, IntField(j, "scriptsRead"), IntField(j, "execStorageAttempts"),
                IntField(j, "execStorageCalls"), IntField(j, "autoPathCount"),
                IntField(j, "autoPathHits"), IntField(j, "hostReadDelta"),
                Has(j, "\"appLock\":true") ? "true" : "false");
    std::printf("[m2h] %s trace=\"%s\"\n", tag,
                StrField(j, "scriptTrace").c_str());
}

// 跑一次完整官方启动链，返回 JSON（调用方负责 free）；失败返回 NULL。
static char *RunChain(const char *dir, const char *storage,
                      galbox::XP3Archive &arc) {
    if (!OpenDir(dir, arc)) return NULL;
    if (!GalBoxTJS2Install(&arc, storage, ""))
        return NULL;
    char *j = GalBoxTJS2ExecScript("startup.tjs", NULL);
    GalBoxTJS2Uninstall();
    arc.close();
    return j;
}

// ---- 场景 A：官方 Config.tjs，干净进程 ---------------------------------
static int RunCaseA(const char *game, const char *storage) {
    if (!Exists(std::string(game) + "/system/Config.tjs"))
        return Fail("fixture missing Config.tjs", NULL);

    galbox::XP3Archive arc;
    char *j = RunChain(game, storage, arc);
    if (!j) return Fail("cannot run the official startup chain", NULL);
    std::printf("[m2h] A (official Config.tjs) -> %s\n", j);
    DumpChain("A", j);

    if (!Has(j, "\"ok\":false"))
        return Fail("official startup unexpectedly reported success", j);

    // 硬判据 1：run #45 的缺口（KAGWindow_config 缺失）必须消失 ——
    // 它来自游戏自带的 Config.tjs，夹具换成官方 Config.~new 后就该有。
    const std::string fatal = StrField(j, "fatal");
    const std::string inner = StrField(j, "lastScriptError");
    if (fatal.find("KAGWindow_config") != std::string::npos ||
        inner.find("KAGWindow_config") != std::string::npos)
        return Fail("the official Config.tjs did not take effect "
                    "(KAGWindow_config still missing)", j);

    // 硬判据 2（run #54 修正）：官方 Config.tjs 让 Initialize.tjs 走进
    // UpdateConfig.tjs 分支（run #45 用探针版 Config 时走的是另一条 15 脚本链）。
    // 原来的「脚本数 >= 15」是**探针版路径的假设**，换官方 Config 后不再适用：
    // 现在 3 个脚本(Initialize -> Config -> UpdateConfig)后就撞到新缺口 new Pad()。
    // 真正的「往前推进」判据是 trace 到达 UpdateConfig.tjs，且缺口已从
    // KAGWindow_config 前进到 Pad（见下方记录）。
    const std::string trace = StrField(j, "scriptTrace");
    const int scripts = CountTrace(trace);
    std::printf("[m2h] scripts in trace = %d\n", scripts);
    if (trace.find("UpdateConfig.tjs") == std::string::npos)
        return Fail("official Config.tjs did not advance to UpdateConfig.tjs", j);

    // 硬判据 3：官方 Config.tjs 必须真被读到（不是探针版）。
    if (trace.find("Config.tjs") == std::string::npos)
        return Fail("Config.tjs was never read", j);

    // M2e-6 硬判据：UpdateConfig.tjs 的 3 处缺口必须全部跨过（不再是回归，
    // 而是本轮的出口）。run #56 停在 79 行 new Pad()；补完 Pad 骨架 +
    // Storages.getLocalName(83) + Scripts.exec(93) 后，这三处都不该再出现在
    // fatal / inner 里。若仍报其中之一，说明本轮改动没生效。
    const std::string gapBlock = StrField(j, "fatalBlock");
    const long long gapLine = IntField(j, "fatalLine");
    for (const char *gap : {"\"Pad\"", "getLocalName", "Scripts.exec"}) {
        if (fatal.find(gap) != std::string::npos ||
            inner.find(gap) != std::string::npos) {
            std::printf("[m2h] A still stuck at %s (%s:%lld)\n",
                        gap, gapBlock.c_str(), gapLine);
            return Fail("M2e-6 gap not cleared", j);
        }
    }

    // M2e-7 硬判据：run #57 的下一缺口是 Menus.tjs:149 的
    // `class KAGMenuItem extends MenuItem`。装上 GalBoxMenu.tjs 之后，
    // 它不该再出现在 fatal / inner；脚本链也不该再停在那一行。
    for (const char *gap : {"\"MenuItem\"", "Member \"Menu\" does not exist"}) {
        if (fatal.find(gap) != std::string::npos ||
            inner.find(gap) != std::string::npos)
            return Fail("M2e-7 gap (Menu/MenuItem) not cleared", j);
    }
    if (trace.find("Menus.tjs") == std::string::npos)
        return Fail("the chain did not reach Menus.tjs", j);
    if (gapBlock.find("Menus.tjs") != std::string::npos &&
        gapLine > 0 && gapLine <= 150)
        return Fail("the chain is still stuck at the old Menus.tjs:149 gap", j);
    if (scripts < 16)
        return Fail("script chain regressed below the M2e-7 length", j);

    // M2e-8 硬判据：run 44045387957 的下一缺口是 MainWindow.tjs:313 的
    // `borderStyle = bsSingle;`（bsSingle 常量已在官方常量表里，缺的是
    // Window 原生对象上的可写属性）。连同紧随的 innerSunken / showScrollBars
    // 一起必须消失；链要真到 MainWindow.tjs，且不能停在那三行。
    for (const char *gap : {"\"borderStyle\"", "\"innerSunken\"",
                            "\"showScrollBars\""}) {
        if (fatal.find(gap) != std::string::npos ||
            inner.find(gap) != std::string::npos)
            return Fail("M2e-8 gap (window style) not cleared", j);
    }
    if (trace.find("MainWindow.tjs") == std::string::npos)
        return Fail("the chain did not reach MainWindow.tjs", j);
    if (gapBlock.find("MainWindow.tjs") != std::string::npos &&
        gapLine > 0 && gapLine <= 320)
        return Fail("the chain is still stuck at the old MainWindow.tjs style lines", j);

    // 新缺口只记录，不预设下一缺口的名字 —— 避免判据变成自我实现的预言。
    std::printf("[m2h] A next gap: '%s' @ %s:%lld (after M2e-8)\n",
                fatal.c_str(), gapBlock.c_str(), gapLine);

    // 历史标记回归：官方常量表必须还在（appLock 在新路径下尚未创建，不再当判据）。
    // run #55：M2E4_OFFICIAL_CONSTS 在 GalBoxTJS2Install 的 shim 字段里
    // （形如 "M2E4_OFFICIAL_CONSTS+..."），不是带引号的顶层 JSON 字段。
    const std::string shim = StrField(j, "shim");
    if (shim.find("M2E4_OFFICIAL_CONSTS") == std::string::npos &&
        !Has(j, "\"M2E4_OFFICIAL_CONSTS\""))
        return Fail("M2e-4 regression: official constants missing", j);
    // M2e-6：Pad 骨架必须真的装进引擎（shim tag 只在 ExecScript 成功后写入）。
    if (shim.find("M2E6_PAD_SKELETON") == std::string::npos)
        return Fail("M2e-6 regression: Pad skeleton was not loaded", j);
    // M2e-7：Menu 骨架必须真的装进引擎（tag 只在 ExecScript 成功后写入）。
    if (shim.find("M2E7_MENU_SKELETON") == std::string::npos)
        return Fail("M2e-7 regression: Menu skeleton was not loaded", j);

    std::printf("[m2h] A verdict: official Config.tjs took effect, UpdateConfig.tjs "
                "gaps cleared, advanced to %s:%lld (%d scripts read)\n",
                gapBlock.c_str(), gapLine, scripts);
    GalBoxTJS2Free(j);
    return 0;
}

// ---- 场景 B：负向对照（探针版 Config.tjs），干净进程 ---------------------
static int RunCaseB(const char *ctrl, const char *storage) {
    if (!Exists(std::string(ctrl) + "/system/Config.tjs"))
        return Fail("negative-control fixture missing", NULL);

    galbox::XP3Archive arc;
    char *j = RunChain(ctrl, storage, arc);
    if (!j) return Fail("cannot run the negative control", NULL);
    std::printf("[m2h] B (negative control, probe Config.tjs) -> %s\n", j);
    DumpChain("B", j);

    const std::string fatal = StrField(j, "fatal");
    const std::string inner = StrField(j, "lastScriptError");
    if (fatal.find("KAGWindow_config") == std::string::npos &&
        inner.find("KAGWindow_config") == std::string::npos) {
        std::printf("[m2h] fatal=\"%s\" inner=\"%s\"\n",
                    fatal.c_str(), inner.c_str());
        return Fail("negative control did not reproduce the KAGWindow_config gap", j);
    }
    std::printf("[m2h] B verdict: the gap really comes from Config.tjs\n");
    GalBoxTJS2Free(j);
    return 0;
}

// ---- 场景 LEAK / LEAK2：同进程连跑两次官方链 ----------------------------
// withExceptionFirst = true 时，先跑一个**必然抛异常**的脚本
// （m2g_window_probe.tjs 会因 KAGWindow_config 缺失而抛），
// 用来区分「异常展开污染了 TJS2 的进程级全局栈」与「多引擎本身就有问题」。
static int RunLeak(const char *game, const char *storage, bool withExceptionFirst) {
    if (withExceptionFirst) {
        galbox::XP3Archive arc;
        if (!OpenDir(game, arc)) return Fail("cannot open fixture for warmup", NULL);
        if (!GalBoxTJS2Install(&arc, storage, "")) {
            arc.close();
            return Fail("Install(warmup) returned 0", NULL);
        }
        char *w = GalBoxTJS2ExecScript("m2g_window_probe.tjs", NULL);
        if (w) {
            std::printf("[m2h] warmup (expected to throw) -> fatal=\"%s\"\n",
                        StrField(w, "fatal").c_str());
            GalBoxTJS2Free(w);
        }
        GalBoxTJS2Uninstall();
        arc.close();
    }

    galbox::XP3Archive arc;
    char *j1 = RunChain(game, storage, arc);
    if (!j1) return Fail("cannot run the first chain", NULL);
    std::printf("[m2h] LEAK first run -> %s\n", j1);
    DumpChain("first", j1);
    const int s1 = CountTrace(StrField(j1, "scriptTrace"));
    GalBoxTJS2Free(j1);

    galbox::XP3Archive arc2;
    char *j2 = RunChain(game, storage, arc2);
    if (!j2) return Fail("cannot run the second chain", NULL);
    std::printf("[m2h] LEAK second run -> %s\n", j2);
    DumpChain("second", j2);
    const int s2 = CountTrace(StrField(j2, "scriptTrace"));
    const long long apc2 = IntField(j2, "autoPathCount");

    // run #54：原判据含 s2 < 15，那是探针版 Config 的脚本链长度；官方 Config
    // 走 UpdateConfig 分支只有 3 个脚本，s2 < 15 会恒真 → LEAK 假阳性。
    // 真泄漏的表现是「第二次 autoPath 一个都没加」或「第二次比第一次短」。
    const bool broken = (apc2 <= 0) || (s2 < s1);
    std::printf("[m2h] LEAK verdict: %s (first=%d scripts, second=%d scripts, "
                "autoPathCount=%lld)\n",
                broken ? "LEAK REPRODUCED (second run is broken)"
                       : "no leak (second run is fine)",
                s1, s2, apc2);
    std::printf("M2H_LEAK_RESULT=%s\n", broken ? "LEAK" : "CLEAN");
    GalBoxTJS2Free(j2);
    return 0;
}

// ---- 场景 NOMENU：去掉 Menu 骨架的负向对照（M2e-7） -------------------
static int RunCaseNoMenu(const char *nomenu, const char *storage) {
    if (!Exists(std::string(nomenu) + "/system/Config.tjs"))
        return Fail("NOMENU fixture missing Config.tjs", NULL);
    if (Exists(std::string(nomenu) + "/system/GalBoxMenu.tjs"))
        return Fail("NOMENU fixture must NOT contain the Menu skeleton", NULL);

    galbox::XP3Archive arc;
    char *j = RunChain(nomenu, storage, arc);
    if (!j) return Fail("cannot run the NOMENU control", NULL);
    DumpChain("NOMENU", j);

    if (StrField(j, "shim").find("M2E7_MENU_SKELETON") != std::string::npos)
        return Fail("NOMENU control unexpectedly loaded the Menu skeleton", j);

    const std::string fatal = StrField(j, "fatal");
    const std::string inner = StrField(j, "lastScriptError");
    const std::string block = StrField(j, "fatalBlock");
    // 去掉 Menu 骨架后可能是两处停点之一：
    //   - Window 骨架构造里的 `new Menu()`（GalBoxWindow.tjs）
    //   - 官方 Menus.tjs 的 `class KAGMenuItem extends MenuItem`
    // 二者都属于菜单子系统缺口，都足以证明骨架不是摆设。
    const bool menuGap =
        fatal.find("\"Menu\"") != std::string::npos ||
        inner.find("\"Menu\"") != std::string::npos ||
        fatal.find("\"MenuItem\"") != std::string::npos ||
        inner.find("\"MenuItem\"") != std::string::npos;
    const bool menuBlock =
        block.find("Menus.tjs") != std::string::npos ||
        block.find("GalBoxWindow.tjs") != std::string::npos;
    if (!menuGap || !menuBlock)
        return Fail("NOMENU control did not reproduce a menu-subsystem gap", j);

    std::printf("[m2h] NOMENU verdict: fatal=\"%s\" @ %s:%lld\n",
                fatal.c_str(), block.c_str(), IntField(j, "fatalLine"));
    GalBoxTJS2Free(j);
    return 0;
}

// ---- 场景 NOSTYLE：删掉 Window 样式成员的负向对照（M2e-8） -----------
static int RunCaseNoStyle(const char *nostyle, const char *storage) {
    if (!Exists(std::string(nostyle) + "/system/GalBoxWindow.tjs"))
        return Fail("NOSTYLE fixture missing GalBoxWindow.tjs", NULL);
    std::ifstream f((std::string(nostyle) + "/system/GalBoxWindow.tjs").c_str());
    std::string body((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    if (body.find("M2E8_STYLE_BEGIN") != std::string::npos)
        return Fail("NOSTYLE fixture must have the style block removed", NULL);

    galbox::XP3Archive arc;
    char *j = RunChain(nostyle, storage, arc);
    if (!j) return Fail("cannot run the NOSTYLE control", NULL);
    DumpChain("NOSTYLE", j);

    // Menu 骨架仍应装上 —— 唯一差异是 Window 的三个样式成员。
    if (StrField(j, "shim").find("M2E7_MENU_SKELETON") == std::string::npos)
        return Fail("NOSTYLE control lost the Menu skeleton", j);

    const std::string fatal = StrField(j, "fatal");
    const std::string inner = StrField(j, "lastScriptError");
    const std::string block = StrField(j, "fatalBlock");
    const bool styleGap =
        fatal.find("\"borderStyle\"") != std::string::npos ||
        inner.find("\"borderStyle\"") != std::string::npos ||
        fatal.find("\"innerSunken\"") != std::string::npos ||
        inner.find("\"innerSunken\"") != std::string::npos;
    if (!styleGap || block.find("MainWindow.tjs") == std::string::npos)
        return Fail("NOSTYLE control did not reproduce the borderStyle gap", j);

    std::printf("[m2h] NOSTYLE verdict: fatal=\"%s\" @ %s:%lld\n",
                fatal.c_str(), block.c_str(), IntField(j, "fatalLine"));
    GalBoxTJS2Free(j);
    return 0;
}

int main(int argc, char **argv) {
    std::setbuf(stdout, NULL); // 崩溃诊断: 不丢已打印进度
    const char *which   = (argc > 1) ? argv[1] : "A";
    const char *game    = (argc > 2) ? argv[2] : "native/TJS2/build/m2h_game";
    const char *ctrl    = (argc > 3) ? argv[3] : "native/TJS2/build/m2h_ctrl";
    const char *storage = (argc > 4) ? argv[4] : ".";
    const char *nomenu  = (argc > 5) ? argv[5] : "";
    const char *nostyle = (argc > 6) ? argv[6] : "";

    if (std::strcmp(which, "A") == 0) {
        if (RunCaseA(game, storage) != 0) return 3;
        std::printf("M2H_RESULT=OK\n");
        return 0;
    }
    if (std::strcmp(which, "B") == 0) {
        if (RunCaseB(ctrl, storage) != 0) return 3;
        std::printf("M2H_RESULT=OK\n");
        return 0;
    }
    if (std::strcmp(which, "NOMENU") == 0) {
        if (RunCaseNoMenu(nomenu, storage) != 0) return 3;
        std::printf("M2H_RESULT=OK\n");
        return 0;
    }
    if (std::strcmp(which, "NOSTYLE") == 0) {
        if (RunCaseNoStyle(nostyle, storage) != 0) return 3;
        std::printf("M2H_RESULT=OK\n");
        return 0;
    }
    if (std::strcmp(which, "LEAK") == 0)
        return RunLeak(game, storage, false);
    if (std::strcmp(which, "LEAK2") == 0)
        return RunLeak(game, storage, true);

    std::printf("[m2h] unknown case '%s' (A / B / NOMENU / NOSTYLE / LEAK / LEAK2)\n", which);
    return 2;
}
