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
//   NOTIMER 负向对照（M2e-9）：唯一区别是不放 Timer 骨架 ——
//         必须复现 `Member "Timer" does not exist`。
//   NOFONT 负向对照（M2e-10）：唯一区别是 Layer 骨架里删掉 font 属性段 ——
//         必须复现 `Member "font" does not exist` @ AnimationLayer.tjs:343。
//   NOKAGPARSER 负向对照（M2e-11）：唯一区别是不放 KAGParser 骨架 ——
//         必须复现 `Member "KAGParser" does not exist` @ Conductor.tjs:13。
//   NOASYNCTRIGGER 负向对照（M2e-12）：唯一区别是不放 AsyncTrigger 骨架 ——
//         必须复现 `Member "AsyncTrigger" does not exist` @ Conductor.tjs:33。
//   XP3（M2e-12b）：只跑 .xp3 这种容器（设备端只认它）一次官方链 ——
//         必须走到 MainWindow.tjs、九个骨架全部装载成功、停在当前缺口。
//         目录侧的同口径断言在 A 场景；两边各在独立进程，不做运行期互比
//         （同进程第二次执行会破坏启动链：run #40 实测，M2e-6 未决项）。
//
// A/B/NOMENU/NOSTYLE/NOTIMER/NOFONT/NOKAGPARSER/NOASYNCTRIGGER/XP3 全部通过打印 M2H_RESULT=OK。
#include "HostBridge.h"
#include "XP3Archive.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/stat.h>   // XP3 场景要给它一个干净 storage（不被前面场景写脏）

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

    // ok:true 意味着整条官方启动链**跑通了** —— 那是 M2e 系列的终点，按成功收。
    // 但「跑通」不能只是一句 ok：必须同时满足脚本数 / autoPath / 骨架 tag 三项
    // 硬指标，否则就是半路静默跳过（run #38 的教训：状态串里有 tag ≠ 真装上）。
    // ok:false 才走下面逐缺口的判据。
    if (Has(j, "\"ok\":true")) {
        const std::string traceOk = StrField(j, "scriptTrace");
        const int nOk = CountTrace(traceOk);
        const long long apOk = IntField(j, "autoPathCount");
        const std::string shimOk = StrField(j, "shim");
        std::printf("[m2h] A: startup chain COMPLETED (ok=true, %d scripts, "
                    "autoPath=%lld)\n", nOk, apOk);
        if (nOk < 16)
            return Fail("startup reported success after reading too few scripts", j);
        if (apOk < 5)
            return Fail("startup reported success without registering autoPaths", j);
        if (traceOk.find("MainWindow.tjs") == std::string::npos)
            return Fail("startup reported success without reaching MainWindow.tjs", j);
        for (const char *tag : {"M2E4_OFFICIAL_CONSTS", "M2E11_KAGPARSER_SKELETON",
                                "M2E12_ASYNCTRIGGER_SKELETON"}) {
            if (shimOk.find(tag) == std::string::npos) {
                std::printf("[m2h] A: success path missing shim tag %s\n", tag);
                return Fail("startup reported success without the expected skeletons", j);
            }
        }
        if (shimOk.find("!failed:") != std::string::npos)
            return Fail("startup reported success but a skeleton failed to load", j);
        GalBoxTJS2Free(j);
        return 0;
    }
    if (!Has(j, "\"ok\":false"))
        return Fail("startup result JSON has neither ok:true nor ok:false", j);

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
    // 精确行号 313-320（borderStyle 等样式赋值区），不用 <=320 宽范围，避免误伤同文件新缺口。
    if (gapBlock.find("MainWindow.tjs") != std::string::npos &&
        gapLine >= 313 && gapLine <= 320)
        return Fail("the chain is still stuck at the old MainWindow.tjs style lines", j);

    // M2e-9 硬判据：run 44046384978 的下一缺口是 MainWindow.tjs:337 的
    // `new Timer(onQuakeTimerInterval, '')`。骨架装上后它必须消失，且链
    // 不能停在那几行（337-339 是构造/登记/interval 赋值）。
    if (fatal.find("\"Timer\"") != std::string::npos ||
        inner.find("\"Timer\"") != std::string::npos)
        return Fail("M2e-9 gap (Timer) not cleared", j);
    // 同样只用精确行号（Timer 缺口在 337-339），不用 <=340 宽范围，避免误伤同文件新缺口。
    if (gapBlock.find("MainWindow.tjs") != std::string::npos &&
        gapLine >= 337 && gapLine <= 339)
        return Fail("the chain is still stuck at the old MainWindow.tjs Timer lines", j);

    // M2e-10 硬判据：run 44047626865 的下一缺口是 AnimationLayer.tjs:343 的
    // `Anim_defaultFontInfo.face = font.face;`（以及紧随的 .height）。
    // Layer 骨架装上 font 属性后，它必须消失，且链不能停在旧那两行。
    if (fatal.find("\"font\"") != std::string::npos ||
        inner.find("\"font\"") != std::string::npos)
        return Fail("M2e-10 gap (Layer.font) not cleared", j);
    // ⚠️ 判据不能用 `gapLine <= 345` 这种宽范围：脚本执行顺序与源码行号无关，
    // run 44057349589 的新缺口 debugLevel 落在同一文件 266 行（比旧停点还小），
    // 被宽判据误判成「还卡在旧 font」。只精确匹配旧缺口那两行。
    if (gapBlock.find("AnimationLayer.tjs") != std::string::npos &&
        (gapLine == 343 || gapLine == 344))
        return Fail("the chain is still stuck at the old AnimationLayer.tjs font lines", j);
    if (trace.find("AnimationLayer.tjs") == std::string::npos)
        return Fail("the chain did not reach AnimationLayer.tjs", j);

    // M2e-11 硬判据：run 44049497384 的下一缺口是 Conductor.tjs:13 的
    // `class BaseConductor extends KAGParser`（类定义阶段就需要父类）。
    // KAGParser 骨架装上后它必须消失，且链不能停在那一行。
    if (fatal.find("\"KAGParser\"") != std::string::npos ||
        inner.find("\"KAGParser\"") != std::string::npos)
        return Fail("M2e-11 gap (KAGParser) not cleared", j);
    if (gapBlock.find("Conductor.tjs") != std::string::npos &&
        gapLine > 0 && gapLine <= 13)
        return Fail("the chain is still stuck at the old Conductor.tjs:13 KAGParser line", j);
    if (trace.find("Conductor.tjs") == std::string::npos)
        return Fail("the chain did not reach Conductor.tjs", j);

    // M2e-12 硬判据：run 44052678080 的下一缺口是 Conductor.tjs:33 的
    // `oneshot = new AsyncTrigger(timerCallback, '')`。骨架装上后它必须消失，
    // 且链不能停在那几行（33-35 是构造/cached 赋值）。
    if (fatal.find("\"AsyncTrigger\"") != std::string::npos ||
        inner.find("\"AsyncTrigger\"") != std::string::npos)
        return Fail("M2e-12 gap (AsyncTrigger) not cleared", j);
    if (gapBlock.find("Conductor.tjs") != std::string::npos &&
        gapLine > 0 && gapLine <= 35)
        return Fail("the chain is still stuck at the old Conductor.tjs:33 AsyncTrigger lines", j);

    // M2e-13 硬判据：run 44065648958（M2e-12b 出包后设备端与自测同口径）的
    // 下一缺口是 AnimationLayer.tjs:266 的 `debugLevel = tkdlNone;`；同一轮
    // audit 里 Conductor.tjs:350 的 `debugLevel = tkdlVerbose;` 是同类赋值。
    // 这两处都是**只写不读**的调试等级槽位（全项目 2 处赋值、0 处读取），
    // 官方原生 KAGParser 有该成员，GalBox 骨架漏了 —— 赋值时 TJS2 直接抛
    // `Member "debugLevel" does not exist`，整个 AnimationConductor 构造失败。
    if (fatal.find("debugLevel") != std::string::npos ||
        inner.find("debugLevel") != std::string::npos)
        return Fail("M2e-13 gap (debugLevel) not cleared", j);
    // 精确行号：AnimationLayer.tjs 265-266（ignoreCR / debugLevel 两行紧邻，
    // 报错行落在哪一行取决于求值顺序，两行都算旧停点）；Conductor.tjs 350。
    if (gapBlock.find("AnimationLayer.tjs") != std::string::npos &&
        (gapLine == 265 || gapLine == 266))
        return Fail("the chain is still stuck at the old AnimationLayer.tjs debugLevel line", j);
    if (gapBlock.find("Conductor.tjs") != std::string::npos && gapLine == 350)
        return Fail("the chain is still stuck at the old Conductor.tjs:350 debugLevel line", j);

    // M2e-14 硬判据：run 44075552094 的下一缺口是 GraphicLayer.tjs:337 的
    // `hitType = htMask;`（紧邻 338 `hitThreshold = 0;`）。两者都是官方
    // LayerIntf.cpp 的命中测试面成员，而 Layer 骨架（M2e-2）只给了几何与可见性。
    // 337-338 是同一个赋值块，因此本轮一并补上，但分别有独立负向对照：
    //   NOHITTYPE      -> 必须复现 hitType@337
    //   NOHITTHRESHOLD -> 必须复现 hitThreshold@338（证明第二个成员也不是摆设）
    if (fatal.find("\"hitType\"") != std::string::npos ||
        inner.find("\"hitType\"") != std::string::npos)
        return Fail("M2e-14 gap (hitType) not cleared", j);
    if (fatal.find("\"hitThreshold\"") != std::string::npos ||
        inner.find("\"hitThreshold\"") != std::string::npos)
        return Fail("M2e-14 gap (hitThreshold) not cleared", j);
    if (gapBlock.find("GraphicLayer.tjs") != std::string::npos &&
        (gapLine == 337 || gapLine == 338))
        return Fail("the chain is still stuck at the old GraphicLayer.tjs hit-test lines", j);

    // M2e-15 硬判据：run 44082272762 的下一缺口是 MainWindow.tjs:348 的
    // `fore.base.setImageSize(scWidth, scHeight);`，紧接着 349 是
    // `setSizeToImageSize();`。两者是官方 LayerIntf.cpp 的**图像面**
    // （imageWidth/imageHeight + 这两个方法），而 Layer 骨架（M2e-2/M2e-14）
    // 只给了几何、可见性、字体、命中面，没给图像面。
    // 348-353 是同一段「建表/裏背景层」初始化，本轮一并补；但两个方法各有
    // 独立负向对照，证明后补的那个也不是摆设：
    //   NOIMAGESIZE       -> 必须复现 setImageSize@348
    //   NOSIZETOIMAGESIZE -> 必须复现 setSizeToImageSize@349
    if (fatal.find("setImageSize") != std::string::npos ||
        inner.find("setImageSize") != std::string::npos)
        return Fail("M2e-15 gap (setImageSize) not cleared", j);
    if (fatal.find("setSizeToImageSize") != std::string::npos ||
        inner.find("setSizeToImageSize") != std::string::npos)
        return Fail("M2e-15 gap (setSizeToImageSize) not cleared", j);
    if (gapBlock.find("MainWindow.tjs") != std::string::npos &&
        (gapLine == 348 || gapLine == 349))
        return Fail("the chain is still stuck at the MainWindow.tjs background-layer image lines", j);

    // M2e-16 硬判据：run 44090843151 的下一缺口是 GraphicLayer.tjs:422 的
    // `face = dfAlpha;`，紧接 423 `fillRect(0, 0, imageWidth, imageHeight, 0xff000000);`。
    // 这条路径由 MainWindow.tjs:356 的 `fore.base.freeImage()` →
    // AnimationLayer.freeImage() → GraphicLayer.clearImage() 走到 —— 也就是说
    // M2e-15 补上的图像面刚一生效，立刻撞上绘制面。两个成员各有独立负向对照：
    //   NODRAWFACE -> 必须复现 face@422
    //   NOFILLRECT -> 必须复现 fillRect@423
    if (fatal.find("\"face\"") != std::string::npos ||
        inner.find("\"face\"") != std::string::npos)
        return Fail("M2e-16 gap (face) not cleared", j);
    if (fatal.find("\"fillRect\"") != std::string::npos ||
        inner.find("\"fillRect\"") != std::string::npos)
        return Fail("M2e-16 gap (fillRect) not cleared", j);
    if (gapBlock.find("GraphicLayer.tjs") != std::string::npos &&
        (gapLine == 422 || gapLine == 423))
        return Fail("the chain is still stuck at the GraphicLayer.tjs clearImage draw lines", j);

    // M2e-17 硬判据：run 44104356887 的下一缺口是 GraphicLayer.tjs:58 的
    // `cursor = defaultCursor = c;`（setDefaultCursor 内），由 MainWindow.tjs:359-360
    // 给表/裏两张背景层各调一次。`cursor` 全脚本无 var 声明（第 13 行那个属于
    // ProvinceContext）⇒ 是原生 Layer 成员。独立负向对照：
    //   NOCURSOR -> 必须复现 cursor@GraphicLayer.tjs:58
    if (fatal.find("\"cursor\"") != std::string::npos ||
        inner.find("\"cursor\"") != std::string::npos)
        return Fail("M2e-17 gap (cursor) not cleared", j);
    if (gapBlock.find("GraphicLayer.tjs") != std::string::npos && gapLine == 58)
        return Fail("the chain is still stuck at the GraphicLayer.tjs setDefaultCursor line", j);

    // 新缺口只记录，不预设下一缺口的名字 —— 避免判据变成自我实现的预言。
    std::printf("[m2h] A next gap: '%s' @ %s:%lld (after M2e-17)\n",
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
    // M2e-9：Timer 骨架必须真的装进引擎。
    if (shim.find("M2E9_TIMER_SKELETON") == std::string::npos)
        return Fail("M2e-9 regression: Timer skeleton was not loaded", j);
    // M2e-10：Font 骨架必须真的装进引擎。
    if (shim.find("M2E10_FONT_SKELETON") == std::string::npos)
        return Fail("M2e-10 regression: Font skeleton was not loaded", j);
    // M2e-11：KAGParser 骨架必须真的装进引擎。
    if (shim.find("M2E11_KAGPARSER_SKELETON") == std::string::npos)
        return Fail("M2e-11 regression: KAGParser skeleton was not loaded", j);
    // M2e-12：AsyncTrigger 骨架必须真的装进引擎。
    if (shim.find("M2E12_ASYNCTRIGGER_SKELETON") == std::string::npos)
        return Fail("M2e-12 regression: AsyncTrigger skeleton was not loaded", j);

    std::printf("[m2h] A verdict: official Config.tjs took effect, UpdateConfig.tjs "
                "gaps cleared, advanced to %s:%lld (%d scripts read)\n",
                gapBlock.c_str(), gapLine, scripts);    GalBoxTJS2Free(j);
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

// ---- 场景 NOTIMER：去掉 Timer 骨架的负向对照（M2e-9） ----------------
static int RunCaseNoTimer(const char *notimer, const char *storage) {
    if (!Exists(std::string(notimer) + "/system/Config.tjs"))
        return Fail("NOTIMER fixture missing Config.tjs", NULL);
    if (Exists(std::string(notimer) + "/system/GalBoxTimer.tjs"))
        return Fail("NOTIMER fixture must NOT contain the Timer skeleton", NULL);

    galbox::XP3Archive arc;
    char *j = RunChain(notimer, storage, arc);
    if (!j) return Fail("cannot run the NOTIMER control", NULL);
    DumpChain("NOTIMER", j);

    if (StrField(j, "shim").find("M2E9_TIMER_SKELETON") != std::string::npos)
        return Fail("NOTIMER control unexpectedly loaded the Timer skeleton", j);

    const std::string fatal = StrField(j, "fatal");
    const std::string inner = StrField(j, "lastScriptError");
    const std::string block = StrField(j, "fatalBlock");
    const bool timerGap =
        fatal.find("\"Timer\"") != std::string::npos ||
        inner.find("\"Timer\"") != std::string::npos;
    if (!timerGap || block.find("MainWindow.tjs") == std::string::npos)
        return Fail("NOTIMER control did not reproduce the Timer gap", j);

    std::printf("[m2h] NOTIMER verdict: fatal=\"%s\" @ %s:%lld\n",
                fatal.c_str(), block.c_str(), IntField(j, "fatalLine"));
    GalBoxTJS2Free(j);
    return 0;
}

// ---- 场景 NOFONT：Layer 缺 font 属性的负向对照（M2e-10） ---------------
static int RunCaseNoFont(const char *nofont, const char *storage) {
    if (!Exists(std::string(nofont) + "/system/GalBoxLayer.tjs"))
        return Fail("NOFONT fixture missing GalBoxLayer.tjs", NULL);
    if (!Exists(std::string(nofont) + "/system/GalBoxFont.tjs"))
        return Fail("NOFONT fixture must keep the Font skeleton", NULL);
    {   // Layer 骨架里必须已经删掉 font 属性段（标记消失）
        std::ifstream f((std::string(nofont) + "/system/GalBoxLayer.tjs").c_str());
        std::string body((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        if (body.find("M2E10_FONT_BEGIN") != std::string::npos)
            return Fail("NOFONT fixture must have the Layer.font block removed", NULL);
    }

    galbox::XP3Archive arc;
    char *j = RunChain(nofont, storage, arc);
    if (!j) return Fail("cannot run the NOFONT control", NULL);
    DumpChain("NOFONT", j);

    // Font 骨架文件仍在、shim tag 仍应装上 —— 唯一差异是 Layer.font。
    if (StrField(j, "shim").find("M2E10_FONT_SKELETON") == std::string::npos)
        return Fail("NOFONT control lost the Font skeleton tag", j);

    const std::string fatal = StrField(j, "fatal");
    const std::string inner = StrField(j, "lastScriptError");
    const std::string block = StrField(j, "fatalBlock");
    const bool fontGap =
        fatal.find("\"font\"") != std::string::npos ||
        inner.find("\"font\"") != std::string::npos;
    if (!fontGap || block.find("AnimationLayer.tjs") == std::string::npos)
        return Fail("NOFONT control did not reproduce the Layer.font gap", j);

    std::printf("[m2h] NOFONT verdict: fatal=\"%s\" @ %s:%lld\n",
                fatal.c_str(), block.c_str(), IntField(j, "fatalLine"));
    GalBoxTJS2Free(j);
    return 0;
}

// ---- 场景 NOKAGPARSER：去掉 KAGParser 骨架的负向对照（M2e-11） --------
static int RunCaseNoKAGParser(const char *nokagparser, const char *storage) {
    if (!Exists(std::string(nokagparser) + "/system/Config.tjs"))
        return Fail("NOKAGPARSER fixture missing Config.tjs", NULL);
    if (Exists(std::string(nokagparser) + "/system/GalBoxKAGParser.tjs"))
        return Fail("NOKAGPARSER fixture must NOT contain the KAGParser skeleton", NULL);

    galbox::XP3Archive arc;
    char *j = RunChain(nokagparser, storage, arc);
    if (!j) return Fail("cannot run the NOKAGPARSER control", NULL);
    DumpChain("NOKAGPARSER", j);

    if (StrField(j, "shim").find("M2E11_KAGPARSER_SKELETON") != std::string::npos)
        return Fail("NOKAGPARSER control unexpectedly loaded the KAGParser skeleton", j);

    const std::string fatal = StrField(j, "fatal");
    const std::string inner = StrField(j, "lastScriptError");
    const std::string block = StrField(j, "fatalBlock");
    const bool gap =
        fatal.find("\"KAGParser\"") != std::string::npos ||
        inner.find("\"KAGParser\"") != std::string::npos;
    if (!gap || block.find("Conductor.tjs") == std::string::npos)
        return Fail("NOKAGPARSER control did not reproduce the KAGParser gap", j);

    std::printf("[m2h] NOKAGPARSER verdict: fatal=\"%s\" @ %s:%lld\n",
                fatal.c_str(), block.c_str(), IntField(j, "fatalLine"));
    GalBoxTJS2Free(j);
    return 0;
}

// ---- 场景 NOASYNCTRIGGER：去掉 AsyncTrigger 骨架的负向对照（M2e-12） --
static int RunCaseNoAsyncTrigger(const char *noat, const char *storage) {
    if (!Exists(std::string(noat) + "/system/Config.tjs"))
        return Fail("NOASYNCTRIGGER fixture missing Config.tjs", NULL);
    if (Exists(std::string(noat) + "/system/GalBoxAsyncTrigger.tjs"))
        return Fail("NOASYNCTRIGGER fixture must NOT contain the AsyncTrigger skeleton", NULL);

    galbox::XP3Archive arc;
    char *j = RunChain(noat, storage, arc);
    if (!j) return Fail("cannot run the NOASYNCTRIGGER control", NULL);
    DumpChain("NOASYNCTRIGGER", j);

    if (StrField(j, "shim").find("M2E12_ASYNCTRIGGER_SKELETON") != std::string::npos)
        return Fail("NOASYNCTRIGGER control unexpectedly loaded the AsyncTrigger skeleton", j);

    const std::string fatal = StrField(j, "fatal");
    const std::string inner = StrField(j, "lastScriptError");
    const std::string block = StrField(j, "fatalBlock");
    const bool gap =
        fatal.find("\"AsyncTrigger\"") != std::string::npos ||
        inner.find("\"AsyncTrigger\"") != std::string::npos;
    if (!gap || block.find("Conductor.tjs") == std::string::npos)
        return Fail("NOASYNCTRIGGER control did not reproduce the AsyncTrigger gap", j);

    std::printf("[m2h] NOASYNCTRIGGER verdict: fatal=\"%s\" @ %s:%lld\n",
                fatal.c_str(), block.c_str(), IntField(j, "fatalLine"));
    GalBoxTJS2Free(j);
    return 0;
}

// ---- 场景 NODEBUGLEVEL：KAGParser 骨架缺 debugLevel 的负向对照（M2e-13）
// 精准对照：KAGParser 骨架文件**保留**（M2E11 tag 仍应出现），只把
// M2E13_BEGIN/END 之间那几行（`var debugLevel;` 与注释）整段删掉。
// 它必须复现同一处停点，否则 A 场景「跨过 debugLevel」就是恒真式自证。
static int RunCaseNoDebugLevel(const char *nodl, const char *storage) {
    if (!Exists(std::string(nodl) + "/system/Config.tjs"))
        return Fail("NODEBUGLEVEL fixture missing Config.tjs", NULL);
    const std::string skel = std::string(nodl) + "/system/GalBoxKAGParser.tjs";
    if (!Exists(skel))
        return Fail("NODEBUGLEVEL fixture must keep the KAGParser skeleton", NULL);
    {
        std::ifstream f(skel.c_str());
        std::string body((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        if (body.find("M2E13_BEGIN") != std::string::npos)
            return Fail("NODEBUGLEVEL fixture must have the debugLevel block removed", NULL);
        if (body.find("var debugLevel") != std::string::npos)
            return Fail("NODEBUGLEVEL fixture still declares debugLevel", NULL);
        if (body.find("class KAGParser") == std::string::npos)
            return Fail("NODEBUGLEVEL fixture lost the KAGParser class itself", NULL);
    }

    galbox::XP3Archive arc;
    char *j = RunChain(nodl, storage, arc);
    if (!j) return Fail("cannot run the NODEBUGLEVEL control", NULL);
    DumpChain("NODEBUGLEVEL", j);

    // 骨架文件仍在、tag 仍应出现 —— 唯一差异就是 debugLevel 这一个成员。
    if (StrField(j, "shim").find("M2E11_KAGPARSER_SKELETON") == std::string::npos)
        return Fail("NODEBUGLEVEL control lost the KAGParser skeleton tag", j);

    const std::string fatal = StrField(j, "fatal");
    const std::string inner = StrField(j, "lastScriptError");
    const std::string block = StrField(j, "fatalBlock");
    const long long line = IntField(j, "fatalLine");
    const std::string both = fatal + " || " + inner;
    // 报错行可能落在 266（debugLevel 赋值）或紧邻的 265（ignoreCR，同一构造里
    // 前一行）—— 两行都属于这一处缺口，只认精确行号，不用 <=N 宽范围。
    const bool atGapLine =
        (block.find("AnimationLayer.tjs") != std::string::npos &&
         (line == 265 || line == 266)) ||
        (block.find("Conductor.tjs") != std::string::npos && line == 350);
    if (both.find("does not exist") == std::string::npos || !atGapLine)
        return Fail("NODEBUGLEVEL control did not reproduce the debugLevel gap", j);

    std::printf("[m2h] NODEBUGLEVEL verdict: fatal=\"%s\" @ %s:%lld\n",
                fatal.c_str(), block.c_str(), line);
    GalBoxTJS2Free(j);
    return 0;
}

// ---- 场景 NOHITTYPE：Layer 缺命中面（hitType + hitThreshold）的负向对照（M2e-14）
// 精准对照：Layer 骨架文件保留（其余成员都在），只删 M2E14_BEGIN/END 整段。
// 必须复现 `Member "hitType" does not exist @ GraphicLayer.tjs:337`。
static int RunCaseNoHitType(const char *nohit, const char *storage) {
    if (!Exists(std::string(nohit) + "/system/Config.tjs"))
        return Fail("NOHITTYPE fixture missing Config.tjs", NULL);
    const std::string skel = std::string(nohit) + "/system/GalBoxLayer.tjs";
    if (!Exists(skel))
        return Fail("NOHITTYPE fixture must keep the Layer skeleton", NULL);
    {
        std::ifstream f(skel.c_str());
        std::string body((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        if (body.find("M2E14_BEGIN") != std::string::npos)
            return Fail("NOHITTYPE fixture must have the M2E14 block removed", NULL);
        if (body.find("var hitType") != std::string::npos ||
            body.find("var hitThreshold") != std::string::npos)
            return Fail("NOHITTYPE fixture still declares the hit-test members", NULL);
        // 除命中面外，Layer 的其余成员必须完好 —— 否则我们测的就不是这一处缺口。
        for (const char *keep : {"var visible", "var width", "var font", "class Layer"}) {
            if (body.find(keep) == std::string::npos) {
                std::printf("[m2h] NOHITTYPE fixture lost '%s'\n", keep);
                return Fail("NOHITTYPE fixture lost unrelated Layer members", NULL);
            }
        }
    }

    galbox::XP3Archive arc;
    char *j = RunChain(nohit, storage, arc);
    if (!j) return Fail("cannot run the NOHITTYPE control", NULL);
    DumpChain("NOHITTYPE", j);

    if (StrField(j, "shim").find("M2E2_LAYER_SKELETON") == std::string::npos)
        return Fail("NOHITTYPE control lost the Layer skeleton tag", j);

    const std::string fatal = StrField(j, "fatal");
    const std::string inner = StrField(j, "lastScriptError");
    const std::string block = StrField(j, "fatalBlock");
    const long long line = IntField(j, "fatalLine");
    const bool gap = (fatal + " || " + inner).find("\"hitType\"") != std::string::npos;
    if (!gap || block.find("GraphicLayer.tjs") == std::string::npos ||
        line != 337)
        return Fail("NOHITTYPE control did not reproduce the hitType gap", j);

    std::printf("[m2h] NOHITTYPE verdict: fatal=\"%s\" @ %s:%lld\n",
                fatal.c_str(), block.c_str(), line);
    GalBoxTJS2Free(j);
    return 0;
}

// ---- 场景 NOHITTHRESHOLD：只缺 hitThreshold 的负向对照（M2e-14 第二半）---
// 与 NOHITTYPE 的区别：hitType 仍在，只有 hitThreshold 被删。若这一轮把
// hitThreshold 一起补上是「顺手」而非必要，这个场景就会跟 A 一样跑过去 ——
// 必须复现 `Member "hitThreshold" does not exist @ GraphicLayer.tjs:338`。
static int RunCaseNoHitThreshold(const char *noth, const char *storage) {
    if (!Exists(std::string(noth) + "/system/Config.tjs"))
        return Fail("NOHITTHRESHOLD fixture missing Config.tjs", NULL);
    const std::string skel = std::string(noth) + "/system/GalBoxLayer.tjs";
    if (!Exists(skel))
        return Fail("NOHITTHRESHOLD fixture must keep the Layer skeleton", NULL);
    {
        std::ifstream f(skel.c_str());
        std::string body((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        if (body.find("M2E14_THRESHOLD_BEGIN") != std::string::npos)
            return Fail("NOHITTHRESHOLD fixture must have the threshold line removed", NULL);
        if (body.find("var hitThreshold") != std::string::npos)
            return Fail("NOHITTHRESHOLD fixture still declares hitThreshold", NULL);
        if (body.find("var hitType") == std::string::npos)
            return Fail("NOHITTHRESHOLD fixture must keep hitType", NULL);
    }

    galbox::XP3Archive arc;
    char *j = RunChain(noth, storage, arc);
    if (!j) return Fail("cannot run the NOHITTHRESHOLD control", NULL);
    DumpChain("NOHITTHRESHOLD", j);

    const std::string fatal = StrField(j, "fatal");
    const std::string inner = StrField(j, "lastScriptError");
    const std::string block = StrField(j, "fatalBlock");
    const long long line = IntField(j, "fatalLine");
    const bool gap = (fatal + " || " + inner).find("\"hitThreshold\"") != std::string::npos;
    if (!gap || block.find("GraphicLayer.tjs") == std::string::npos ||
        line != 338)
        return Fail("NOHITTHRESHOLD control did not reproduce the hitThreshold gap", j);

    std::printf("[m2h] NOHITTHRESHOLD verdict: fatal=\"%s\" @ %s:%lld\n",
                fatal.c_str(), block.c_str(), line);
    GalBoxTJS2Free(j);
    return 0;
}

// ---- 场景 NOIMAGESIZE：Layer 缺整块图像面的负向对照（M2e-15）-------------
// 精准对照：Layer 骨架文件保留（几何/字体/命中面都在），只删 M2E15_BEGIN/END。
// 必须复现 `Member "setImageSize" does not exist @ MainWindow.tjs:348`。
static int RunCaseNoImageSize(const char *noimg, const char *storage) {
    if (!Exists(std::string(noimg) + "/system/Config.tjs"))
        return Fail("NOIMAGESIZE fixture missing Config.tjs", NULL);
    const std::string skel = std::string(noimg) + "/system/GalBoxLayer.tjs";
    if (!Exists(skel))
        return Fail("NOIMAGESIZE fixture must keep the Layer skeleton", NULL);
    {
        std::ifstream f(skel.c_str());
        std::string body((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        if (body.find("M2E15_BEGIN") != std::string::npos)
            return Fail("NOIMAGESIZE fixture must have the M2E15 block removed", NULL);
        for (const char *gone : {"var imageWidth", "var imageHeight",
                                 "function setImageSize",
                                 "function setSizeToImageSize"}) {
            if (body.find(gone) != std::string::npos) {
                std::printf("[m2h] NOIMAGESIZE fixture still has '%s'\n", gone);
                return Fail("NOIMAGESIZE fixture still declares the image-surface members", NULL);
            }
        }
        // 除图像面外，Layer 的其余成员必须完好 —— 否则测的就不是这一处缺口。
        for (const char *keep : {"var visible", "var hitType", "var font", "class Layer"}) {
            if (body.find(keep) == std::string::npos) {
                std::printf("[m2h] NOIMAGESIZE fixture lost '%s'\n", keep);
                return Fail("NOIMAGESIZE fixture lost unrelated Layer members", NULL);
            }
        }
    }

    galbox::XP3Archive arc;
    char *j = RunChain(noimg, storage, arc);
    if (!j) return Fail("cannot run the NOIMAGESIZE control", NULL);
    DumpChain("NOIMAGESIZE", j);

    if (StrField(j, "shim").find("M2E2_LAYER_SKELETON") == std::string::npos)
        return Fail("NOIMAGESIZE control lost the Layer skeleton tag", j);

    const std::string fatal = StrField(j, "fatal");
    const std::string inner = StrField(j, "lastScriptError");
    const std::string block = StrField(j, "fatalBlock");
    const long long line = IntField(j, "fatalLine");
    const bool gap = (fatal + " || " + inner).find("\"setImageSize\"") != std::string::npos;
    if (!gap || block.find("MainWindow.tjs") == std::string::npos || line != 348)
        return Fail("NOIMAGESIZE control did not reproduce the setImageSize gap", j);

    std::printf("[m2h] NOIMAGESIZE verdict: fatal=\"%s\" @ %s:%lld\n",
                fatal.c_str(), block.c_str(), line);
    GalBoxTJS2Free(j);
    return 0;
}

// ---- 场景 NOSIZETOIMAGESIZE：只缺 setSizeToImageSize 的负向对照（M2e-15 第二半）
// 与 NOIMAGESIZE 的区别：imageWidth/imageHeight 与 setImageSize 都还在，
// 只删 setSizeToImageSize()。若把第二个方法一起补是「顺手」，这个场景就会跟 A
// 一样跑过去 —— 必须复现 `Member "setSizeToImageSize" does not exist @
// MainWindow.tjs:349`。
static int RunCaseNoSizeToImageSize(const char *nosz, const char *storage) {
    if (!Exists(std::string(nosz) + "/system/Config.tjs"))
        return Fail("NOSIZETOIMAGESIZE fixture missing Config.tjs", NULL);
    const std::string skel = std::string(nosz) + "/system/GalBoxLayer.tjs";
    if (!Exists(skel))
        return Fail("NOSIZETOIMAGESIZE fixture must keep the Layer skeleton", NULL);
    {
        std::ifstream f(skel.c_str());
        std::string body((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        if (body.find("M2E15_SIZETO_BEGIN") != std::string::npos)
            return Fail("NOSIZETOIMAGESIZE fixture must have the setSizeToImageSize method removed", NULL);
        if (body.find("function setSizeToImageSize") != std::string::npos)
            return Fail("NOSIZETOIMAGESIZE fixture still declares setSizeToImageSize", NULL);
        if (body.find("function setImageSize") == std::string::npos)
            return Fail("NOSIZETOIMAGESIZE fixture must keep setImageSize", NULL);
        if (body.find("var imageWidth") == std::string::npos)
            return Fail("NOSIZETOIMAGESIZE fixture must keep imageWidth", NULL);
    }

    galbox::XP3Archive arc;
    char *j = RunChain(nosz, storage, arc);
    if (!j) return Fail("cannot run the NOSIZETOIMAGESIZE control", NULL);
    DumpChain("NOSIZETOIMAGESIZE", j);

    const std::string fatal = StrField(j, "fatal");
    const std::string inner = StrField(j, "lastScriptError");
    const std::string block = StrField(j, "fatalBlock");
    const long long line = IntField(j, "fatalLine");
    const bool gap = (fatal + " || " + inner).find("\"setSizeToImageSize\"") != std::string::npos;
    if (!gap || block.find("MainWindow.tjs") == std::string::npos || line != 349)
        return Fail("NOSIZETOIMAGESIZE control did not reproduce the setSizeToImageSize gap", j);

    std::printf("[m2h] NOSIZETOIMAGESIZE verdict: fatal=\"%s\" @ %s:%lld\n",
                fatal.c_str(), block.c_str(), line);
    GalBoxTJS2Free(j);
    return 0;
}

// ---- 场景 NODRAWFACE：Layer 缺绘制面成员 face 的负向对照（M2e-16）---------
// 精准对照：Layer 骨架保留（几何/图像面/命中面都在），只删 M2E16_FACE。
// 必须复现 `Member "face" does not exist @ GraphicLayer.tjs:422`。
static int RunCaseNoDrawFace(const char *noface, const char *storage) {
    if (!Exists(std::string(noface) + "/system/Config.tjs"))
        return Fail("NODRAWFACE fixture missing Config.tjs", NULL);
    const std::string skel = std::string(noface) + "/system/GalBoxLayer.tjs";
    if (!Exists(skel))
        return Fail("NODRAWFACE fixture must keep the Layer skeleton", NULL);
    {
        std::ifstream f(skel.c_str());
        std::string body((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        if (body.find("M2E16_FACE_BEGIN") != std::string::npos)
            return Fail("NODRAWFACE fixture must have the face member removed", NULL);
        if (body.find("var face") != std::string::npos)
            return Fail("NODRAWFACE fixture still declares face", NULL);
        // 同段的 fillRect 必须还在 —— 否则测的就是"整段没补"，不是"只缺 face"。
        if (body.find("function fillRect") == std::string::npos)
            return Fail("NODRAWFACE fixture must keep fillRect", NULL);
        for (const char *keep : {"var imageWidth", "var hitType", "class Layer"}) {
            if (body.find(keep) == std::string::npos) {
                std::printf("[m2h] NODRAWFACE fixture lost '%s'\n", keep);
                return Fail("NODRAWFACE fixture lost unrelated Layer members", NULL);
            }
        }
    }

    galbox::XP3Archive arc;
    char *j = RunChain(noface, storage, arc);
    if (!j) return Fail("cannot run the NODRAWFACE control", NULL);
    DumpChain("NODRAWFACE", j);

    if (StrField(j, "shim").find("M2E2_LAYER_SKELETON") == std::string::npos)
        return Fail("NODRAWFACE control lost the Layer skeleton tag", j);

    const std::string fatal = StrField(j, "fatal");
    const std::string inner = StrField(j, "lastScriptError");
    const std::string block = StrField(j, "fatalBlock");
    const long long line = IntField(j, "fatalLine");
    const bool gap = (fatal + " || " + inner).find("\"face\"") != std::string::npos;
    if (!gap || block.find("GraphicLayer.tjs") == std::string::npos || line != 422)
        return Fail("NODRAWFACE control did not reproduce the face gap", j);

    std::printf("[m2h] NODRAWFACE verdict: fatal=\"%s\" @ %s:%lld\n",
                fatal.c_str(), block.c_str(), line);
    GalBoxTJS2Free(j);
    return 0;
}

// ---- 场景 NOFILLRECT：只缺绘制入口 fillRect 的负向对照（M2e-16 第二半）----
// 与 NODRAWFACE 的区别：face 仍在，只删 fillRect()。若把 fillRect 一起补是
// 「顺手」，这个场景就会跟 A 一样跑过去 —— 必须复现
// `Member "fillRect" does not exist @ GraphicLayer.tjs:423`。
static int RunCaseNoFillRect(const char *nofr, const char *storage) {
    if (!Exists(std::string(nofr) + "/system/Config.tjs"))
        return Fail("NOFILLRECT fixture missing Config.tjs", NULL);
    const std::string skel = std::string(nofr) + "/system/GalBoxLayer.tjs";
    if (!Exists(skel))
        return Fail("NOFILLRECT fixture must keep the Layer skeleton", NULL);
    {
        std::ifstream f(skel.c_str());
        std::string body((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        if (body.find("M2E16_FILLRECT_BEGIN") != std::string::npos)
            return Fail("NOFILLRECT fixture must have the fillRect method removed", NULL);
        if (body.find("function fillRect") != std::string::npos)
            return Fail("NOFILLRECT fixture still declares fillRect", NULL);
        if (body.find("var face") == std::string::npos)
            return Fail("NOFILLRECT fixture must keep face", NULL);
    }

    galbox::XP3Archive arc;
    char *j = RunChain(nofr, storage, arc);
    if (!j) return Fail("cannot run the NOFILLRECT control", NULL);
    DumpChain("NOFILLRECT", j);

    const std::string fatal = StrField(j, "fatal");
    const std::string inner = StrField(j, "lastScriptError");
    const std::string block = StrField(j, "fatalBlock");
    const long long line = IntField(j, "fatalLine");
    const bool gap = (fatal + " || " + inner).find("\"fillRect\"") != std::string::npos;
    if (!gap || block.find("GraphicLayer.tjs") == std::string::npos || line != 423)
        return Fail("NOFILLRECT control did not reproduce the fillRect gap", j);

    std::printf("[m2h] NOFILLRECT verdict: fatal=\"%s\" @ %s:%lld\n",
                fatal.c_str(), block.c_str(), line);
    GalBoxTJS2Free(j);
    return 0;
}

// ---- 场景 NOCURSOR：Layer 缺光标面成员 cursor 的负向对照（M2e-17）--------
// 精准对照：Layer 骨架保留（几何/命中面/图像面/绘制面都在），只删 M2E17 段。
// 必须复现 `Member "cursor" does not exist @ GraphicLayer.tjs:58`。
static int RunCaseNoCursor(const char *nocursor, const char *storage) {
    if (!Exists(std::string(nocursor) + "/system/Config.tjs"))
        return Fail("NOCURSOR fixture missing Config.tjs", NULL);
    const std::string skel = std::string(nocursor) + "/system/GalBoxLayer.tjs";
    if (!Exists(skel))
        return Fail("NOCURSOR fixture must keep the Layer skeleton", NULL);
    {
        std::ifstream f(skel.c_str());
        std::string body((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        if (body.find("M2E17_BEGIN") != std::string::npos)
            return Fail("NOCURSOR fixture must have the cursor member removed", NULL);
        if (body.find("var cursor") != std::string::npos)
            return Fail("NOCURSOR fixture still declares cursor", NULL);
        // 别把 defaultCursor 一起删了 —— 那是官方 GraphicLayer.tjs:31 自己声明的，
        // 骨架里本来就没有，删不删都测不出 cursor 这条。
        for (const char *keep : {"var face", "function fillRect", "var imageWidth",
                                 "var hitType", "class Layer"}) {
            if (body.find(keep) == std::string::npos) {
                std::printf("[m2h] NOCURSOR fixture lost '%s'\n", keep);
                return Fail("NOCURSOR fixture lost unrelated Layer members", NULL);
            }
        }
    }

    galbox::XP3Archive arc;
    char *j = RunChain(nocursor, storage, arc);
    if (!j) return Fail("cannot run the NOCURSOR control", NULL);
    DumpChain("NOCURSOR", j);

    if (StrField(j, "shim").find("M2E2_LAYER_SKELETON") == std::string::npos)
        return Fail("NOCURSOR control lost the Layer skeleton tag", j);

    const std::string fatal = StrField(j, "fatal");
    const std::string inner = StrField(j, "lastScriptError");
    const std::string block = StrField(j, "fatalBlock");
    const long long line = IntField(j, "fatalLine");
    const bool gap = (fatal + " || " + inner).find("\"cursor\"") != std::string::npos;
    if (!gap || block.find("GraphicLayer.tjs") == std::string::npos || line != 58)
        return Fail("NOCURSOR control did not reproduce the cursor gap", j);

    std::printf("[m2h] NOCURSOR verdict: fatal=\"%s\" @ %s:%lld\n",
                fatal.c_str(), block.c_str(), line);
    GalBoxTJS2Free(j);
    return 0;
}

// 打开任意容器（目录 / .xp3）跑一次官方链，返回 JSON。
// dirMode / entryCount 必须在 close() 之前取（close 会清掉它们）。
static char *RunArchive(const char *path, const char *storage,
                        galbox::XP3Archive &arc, bool *opened,
                        bool *dirMode, size_t *entryCount) {
    *opened = false;
    *dirMode = false;
    *entryCount = 0;
    std::string err;
    if (!arc.open(path, &err)) {
        std::printf("[m2h] open archive failed: %s (%s)\n", path, err.c_str());
        return NULL;
    }
    *opened = true;
    *dirMode = arc.isDirectoryMode();
    *entryCount = arc.count();
    if (!GalBoxTJS2Install(&arc, storage, "")) return NULL;
    char *j = GalBoxTJS2ExecScript("startup.tjs", NULL);
    GalBoxTJS2Uninstall();
    arc.close();
    return j;
}

// ---- 场景 XP3：设备端容器（真 .xp3）独立跑通同一条官方链（M2e-12b） -----
// ⚠️ 本场景**只跑一次**链。项目铁律：每个场景独立进程 —— run #40 已证明
// 同一进程内第二次 ExecScript 会破坏官方启动链（autoPath/trace 全失真），
// 这也是 M2e-6 至今未决的那条。
// 上一版把「目录跑一次 + xp3 跑一次」放进同一进程做逐字比较，第二次被
// 第一次污染：trace 与计数完全相同，但停点从 debugLevel@266 漂到了
// `Cannot convert (void) to Object @398` —— 那不是容器差异，是我的装置错。
// ⇒ 等价性不靠运行期互比，而靠两件事：
//    ① 同一份夹具现场打包（构建脚本保证，不手工列举清单）；
//    ② 目录侧由 A 场景、xp3 侧由本场景各自独立断言同一组绝对指标。
static int RunCasePacked(const char *dir_game, const char *xp3,
                         const char *storage) {
    if (!Exists(std::string(xp3)))
        return Fail("packed xp3 not found (build script did not pack it?)", NULL);

    // 只读条目数：open + close，不 Install、不执行任何脚本，不会污染进程。
    size_t cntDir = 0;
    {
        galbox::XP3Archive probe;
        std::string err;
        if (!probe.open(dir_game, &err))
            return Fail("cannot open the directory fixture for a file count", NULL);
        if (!probe.isDirectoryMode())
            return Fail("directory fixture is not in directory mode", NULL);
        cntDir = probe.count();
        probe.close();
    }

    galbox::XP3Archive arc;
    bool opened = false, dirMode = false;
    size_t cntXp3 = 0;
    // 干净 storage：$OUT 里已经躺了 A/B/NOMENU… 写的文件，直接拿来用会让
    // 官方 Initialize 走「配置已存在」的分支，停点就不是 A 场景那个了。
    std::string st = std::string(storage) + "/xp3_storage";
    mkdir(st.c_str(), 0755);
    char *j = RunArchive(xp3, st.c_str(), arc, &opened, &dirMode, &cntXp3);
    if (!j) return Fail("cannot open/run the packed xp3", NULL);
    DumpChain("XP3", j);

    if (!opened) return Fail("packed xp3 did not open", j);
    if (dirMode) return Fail("packed fixture must be a real .xp3, not a folder", j);
    std::printf("[m2h] XP3 entries: dir=%zu pack=%zu\n", cntDir, cntXp3);
    if (cntXp3 < 30)
        return Fail("packed xp3 holds too few files to be the KAG fixture", j);
    if (cntXp3 != cntDir)
        return Fail("packed xp3 and the directory fixture differ in file count", j);

    // 链必须一路走到最后一段脚本，且是真的走完（不是中途静默跳过）。
    const std::string trace = StrField(j, "scriptTrace");
    if (trace.find("Conductor.tjs") == std::string::npos)
        return Fail("packed xp3 did not reach Conductor.tjs", j);
    if (trace.find("AnimationLayer.tjs") == std::string::npos)
        return Fail("packed xp3 did not reach AnimationLayer.tjs", j);
    {
        const size_t pos = trace.rfind("MainWindow.tjs");
        if (pos == std::string::npos || pos + std::strlen("MainWindow.tjs") != trace.size())
            return Fail("packed xp3 did not end the chain at MainWindow.tjs", j);
    }
    const long long scripts = IntField(j, "scriptsRead");
    if (scripts < 15)
        return Fail("packed xp3 read too few scripts to prove the chain works", j);
    if (IntField(j, "autoPathCount") < 5)
        return Fail("packed xp3 registered too few autoPaths (Storages broken?)", j);

    // 九个骨架必须**全部装载成功**：状态串里的 "!failed:" 段一个都不许有。
    // 这比「文件在包里」强 —— 它证明 IsExistentStorage 经 xp3 真的命中了。
    const std::string shim = StrField(j, "shim");
    if (shim.find("!failed:") != std::string::npos)
        return Fail("some skeleton failed to load from the packed xp3", j);
    static const char *kMustHave[] = {
        "M2E4_OFFICIAL_CONSTS", "M2E10_FONT_SKELETON", "M2E2_LAYER_SKELETON",
        "M2E3_WINDOW_SKELETON", "M2E6_PAD_SKELETON", "M2E7_MENU_SKELETON",
        "M2E9_TIMER_SKELETON", "M2E11_KAGPARSER_SKELETON",
        "M2E12_ASYNCTRIGGER_SKELETON",
    };
    for (size_t i = 0; i < sizeof(kMustHave) / sizeof(kMustHave[0]); ++i) {
        if (shim.find(kMustHave[i]) == std::string::npos) {
            std::printf("[m2h] XP3 missing shim tag: %s\n", kMustHave[i]);
            return Fail("packed xp3 did not load a skeleton that the fixture has", j);
        }
    }

    // 停点必须与 A 场景同口径（目录侧的等价结论由 A 在独立进程里断言）：
    // M2e-17 之后设备容器里也不许再出现 debugLevel / hitType / hitThreshold /
    // setImageSize / setSizeToImageSize / face / fillRect / cursor。
    // 这几条判据非恒真 —— 八个对应负向对照（NODEBUGLEVEL / NOHITTYPE /
    // NOHITTHRESHOLD / NOIMAGESIZE / NOSIZETOIMAGESIZE / NODRAWFACE /
    // NOFILLRECT / NOCURSOR）在独立进程里分别证明了「删掉成员 → 缺口复现」。
    const std::string fatal = StrField(j, "fatal");
    const std::string inner = StrField(j, "lastScriptError");
    for (const char *closed : {"debugLevel", "\"hitType\"", "\"hitThreshold\"",
                               "\"setImageSize\"", "\"setSizeToImageSize\"",
                               "\"face\"", "\"fillRect\"", "\"cursor\""}) {
        if (fatal.find(closed) != std::string::npos ||
            inner.find(closed) != std::string::npos) {
            std::printf("[m2h] XP3 still stuck at closed gap: %s\n", closed);
            return Fail("packed xp3 is still stuck at a gap this milestone closed", j);
        }
    }

    std::printf("[m2h] XP3 verdict: real .xp3 ran the same chain "
                "(%lld scripts, debugLevel/hitType/hitThreshold/"
                "setImageSize/setSizeToImageSize/face/fillRect/cursor all cleared "
                "in the device container)\n", scripts);
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
    const char *notimer = (argc > 7) ? argv[7] : "";
    const char *nofont  = (argc > 8) ? argv[8] : "";
    const char *nokagparser = (argc > 9) ? argv[9] : "";
    const char *noat    = (argc > 10) ? argv[10] : "";
    const char *kagxp3  = (argc > 11) ? argv[11] : "";
    const char *nodl    = (argc > 12) ? argv[12] : "";
    const char *nohit   = (argc > 13) ? argv[13] : "";
    const char *noth    = (argc > 14) ? argv[14] : "";
    const char *noimg   = (argc > 15) ? argv[15] : "";
    const char *nosz    = (argc > 16) ? argv[16] : "";
    const char *noface  = (argc > 17) ? argv[17] : "";
    const char *nofr    = (argc > 18) ? argv[18] : "";
    const char *nocursor = (argc > 19) ? argv[19] : "";

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
    if (std::strcmp(which, "NOTIMER") == 0) {
        if (RunCaseNoTimer(notimer, storage) != 0) return 3;
        std::printf("M2H_RESULT=OK\n");
        return 0;
    }
    if (std::strcmp(which, "NOFONT") == 0) {
        if (RunCaseNoFont(nofont, storage) != 0) return 3;
        std::printf("M2H_RESULT=OK\n");
        return 0;
    }
    if (std::strcmp(which, "NOKAGPARSER") == 0) {
        if (RunCaseNoKAGParser(nokagparser, storage) != 0) return 3;
        std::printf("M2H_RESULT=OK\n");
        return 0;
    }
    if (std::strcmp(which, "NOASYNCTRIGGER") == 0) {
        if (RunCaseNoAsyncTrigger(noat, storage) != 0) return 3;
        std::printf("M2H_RESULT=OK\n");
        return 0;
    }
    if (std::strcmp(which, "NODEBUGLEVEL") == 0) {
        if (RunCaseNoDebugLevel(nodl, storage) != 0) return 3;
        std::printf("M2H_RESULT=OK\n");
        return 0;
    }
    if (std::strcmp(which, "NOHITTYPE") == 0) {
        if (RunCaseNoHitType(nohit, storage) != 0) return 3;
        std::printf("M2H_RESULT=OK\n");
        return 0;
    }
    if (std::strcmp(which, "NOHITTHRESHOLD") == 0) {
        if (RunCaseNoHitThreshold(noth, storage) != 0) return 3;
        std::printf("M2H_RESULT=OK\n");
        return 0;
    }
    if (std::strcmp(which, "NOIMAGESIZE") == 0) {
        if (RunCaseNoImageSize(noimg, storage) != 0) return 3;
        std::printf("M2H_RESULT=OK\n");
        return 0;
    }
    if (std::strcmp(which, "NOSIZETOIMAGESIZE") == 0) {
        if (RunCaseNoSizeToImageSize(nosz, storage) != 0) return 3;
        std::printf("M2H_RESULT=OK\n");
        return 0;
    }
    if (std::strcmp(which, "NODRAWFACE") == 0) {
        if (RunCaseNoDrawFace(noface, storage) != 0) return 3;
        std::printf("M2H_RESULT=OK\n");
        return 0;
    }
    if (std::strcmp(which, "NOFILLRECT") == 0) {
        if (RunCaseNoFillRect(nofr, storage) != 0) return 3;
        std::printf("M2H_RESULT=OK\n");
        return 0;
    }
    if (std::strcmp(which, "NOCURSOR") == 0) {
        if (RunCaseNoCursor(nocursor, storage) != 0) return 3;
        std::printf("M2H_RESULT=OK\n");
        return 0;
    }
    if (std::strcmp(which, "XP3") == 0) {
        // argv[2] 是目录夹具，argv[11] 是同一份内容打成的内置包。
        if (RunCasePacked(game, kagxp3, storage) != 0) return 3;
        std::printf("M2H_RESULT=OK\n");
        return 0;
    }
    if (std::strcmp(which, "LEAK") == 0)
        return RunLeak(game, storage, false);
    if (std::strcmp(which, "LEAK2") == 0)
        return RunLeak(game, storage, true);

    std::printf("[m2h] unknown case '%s' (A / B / NOMENU / NOSTYLE / NOTIMER / NOFONT / "
                "NOKAGPARSER / NOASYNCTRIGGER / NODEBUGLEVEL / NOHITTYPE / "
                "NOHITTHRESHOLD / NOIMAGESIZE / NOSIZETOIMAGESIZE / NODRAWFACE / "
                "NOFILLRECT / NOCURSOR / XP3 / LEAK / LEAK2)\n", which);
    return 2;
}
