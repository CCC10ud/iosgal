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
    std::printf("[m2h] %s probe=\"%s\" probeValue=%lld\n", tag,
                StrField(j, "probe").c_str(), IntField(j, "probeValue"));
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

// M2e-44 的 probe 表达式：链条跑完后求值，整数结果进 JSON 的 probeValue。
// 官方 Conductor.onTag(elm)（433 行）只在 handler 命中分支里写 lastTagName，
// 它等于 "clickskip" 就证明 onTag 分发链路真跑过了；没跑过时官方初始值
// 是 ''（Conductor.tjs:341），表达式取 0。
static const char *kTagPathProbe =
    "kag.mainConductor.lastTagName == \"clickskip\" ? 444 : 0";

// 跑一次完整官方启动链，返回 JSON（调用方负责 free）；失败返回 NULL。
// probe 可选：非 NULL 时链条成功收尾后求值（见 kTagPathProbe）。
static char *RunChain(const char *dir, const char *storage,
                      galbox::XP3Archive &arc, const char *probe = NULL) {
    if (!OpenDir(dir, arc)) return NULL;
    if (!GalBoxTJS2Install(&arc, storage, ""))
        return NULL;
    char *j = GalBoxTJS2ExecScript("startup.tjs", probe);
    GalBoxTJS2Uninstall();
    arc.close();
    return j;
}

// ---- 场景 A：官方 Config.tjs，干净进程 ---------------------------------
static int RunCaseA(const char *game, const char *storage) {
    if (!Exists(std::string(game) + "/system/Config.tjs"))
        return Fail("fixture missing Config.tjs", NULL);

    galbox::XP3Archive arc;
    char *j = RunChain(game, storage, arc, kTagPathProbe);
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
        // 🔴 M2e-43 的**出口判据**：官方 KAG 启动链必须**完整跑通**。
        // 补上 getNextTag() 返回 void 后，官方 91 行 `obj === void` 成立 ⇒
        // timerCallback 走「シナリオ終了」分支正常收尾 ⇒ **不进 catch 块** ⇒
        // 链条 16 个脚本全部读完、无任何脚本级异常。
        //
        // ⚠️ 判据不能用 `ok` 字段本身（ok = reason.empty()，reason 是宿主层
        // 执行结果）；脚本级异常要看 fatal / lastScriptError / fatalLine。
        // 这三项在本分支里必须全空 —— 否则就是「宿主说成功但脚本其实抛了错」。
        const std::string fatalOk = StrField(j, "fatal");
        const std::string innerOk = StrField(j, "lastScriptError");
        if (!fatalOk.empty() || !innerOk.empty())
            return Fail("M2e-43: the startup chain reported success but a "
                        "script-level exception was still raised", j);
        if (IntField(j, "fatalLine") != -1)
            return Fail("M2e-43: the startup chain reported success but still "
                        "reports a fatal line", j);
        // 🔴 M2e-44 的出口判据：链条不只收尾干净，还必须真走过 onTag 分发。
        // getNextTag() 首调返回最小 tag 字典 ⇒ 官方 Conductor.tjs:104
        // `var step = onTag(obj);` 首次真执行 ⇒ handlers["clickskip"] 命中 ⇒
        // 官方 handler 把 lastTagName 写成 "clickskip" ⇒ probe 表达式取 444。
        // lastTagName 是官方 Conductor 成员（341 行初始 ''），只在 onTag 的
        // handler 分支里被写 —— 它等于 clickskip 就证明分发链路真跑了。
        // 判据匹配判断语句 + return Fail 连续文本（铁律 1：把 444 改成别的
        // 值、或把判断改成 if(false) 都必须让静态断言变红）。
        const long long probeOk = IntField(j, "probeValue");
        if (probeOk != 444)
            return Fail("M2e-44: the tag dispatch path did not run "
                        "(mainConductor.lastTagName != \"clickskip\")", j);
        std::printf("[m2h] A: startup chain COMPLETED (fatal empty, %d scripts, "
                    "trace ends at MainWindow.tjs)\n", nOk);
        std::printf("[m2h] A: tag dispatch path verified (probeValue=%lld, "
                    "lastTagName=clickskip)\n", probeOk);
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

    // M2e-18 硬判据：run 44203542010 的下一缺口是 HistoryLayer.tjs:110 的
    // `name = "メッセージ履歴レイヤ";`（Layer 的图层名属性）。独立负向对照：
    //   NONAME -> 必须复现 name@HistoryLayer.tjs:110
    if (fatal.find("\"name\"") != std::string::npos ||
        inner.find("\"name\"") != std::string::npos)
        return Fail("M2e-18 gap (name) not cleared", j);
    if (gapBlock.find("HistoryLayer.tjs") != std::string::npos && gapLine == 110)
        return Fail("the chain is still stuck at the HistoryLayer.tjs layer-name line", j);

    // M2e-19 硬判据：run 44205401352 的下一缺口是 HistoryLayer.tjs:130 的
    // `focusable = true;`（HistoryLayer 构造末尾）。独立负向对照：
    //   NOFOCUSABLE -> 必须复现 focusable@HistoryLayer.tjs:130
    if (fatal.find("\"focusable\"") != std::string::npos ||
        inner.find("\"focusable\"") != std::string::npos)
        return Fail("M2e-19 gap (focusable) not cleared", j);
    if (gapBlock.find("HistoryLayer.tjs") != std::string::npos && gapLine == 130)
        return Fail("the chain is still stuck at the HistoryLayer.tjs focusable line", j);

    // M2e-20 硬判据：run 44206906047 的下一缺口是 AnimationLayer.tjs:367 的
    // `setSize(32, 32);`（clearImage() 首句）。独立负向对照：
    //   NOSIZE -> 必须复现 setSize@AnimationLayer.tjs:367
    if (fatal.find("\"setSize\"") != std::string::npos ||
        inner.find("\"setSize\"") != std::string::npos)
        return Fail("M2e-20 gap (setSize) not cleared", j);
    if (gapBlock.find("AnimationLayer.tjs") != std::string::npos && gapLine == 367)
        return Fail("the chain is still stuck at the AnimationLayer.tjs setSize line", j);

    // M2e-21 硬判据：run 44211167189 的下一缺口是 MessageLayer.tjs:493 的
    // `imageModified = true;`（MessageLayer 构造）。独立负向对照：
    //   NOIMAGEMODIFIED -> 必须复现 imageModified@MessageLayer.tjs:493
    if (fatal.find("\"imageModified\"") != std::string::npos ||
        inner.find("\"imageModified\"") != std::string::npos)
        return Fail("M2e-21 gap (imageModified) not cleared", j);
    if (gapBlock.find("MessageLayer.tjs") != std::string::npos && gapLine == 493)
        return Fail("the chain is still stuck at the MessageLayer.tjs imageModified line", j);

    // M2e-22 硬判据：run 44212149103 的下一缺口是 MessageLayer.tjs:587 的
    // `colorRect(0, 0, imageWidth, imageHeight, 0);`（clear() 里清完图像后填底
    // 色，mode 省略）。独立负向对照：
    //   NOCOLORRECT -> 必须复现 colorRect@MessageLayer.tjs:587
    if (fatal.find("\"colorRect\"") != std::string::npos ||
        inner.find("\"colorRect\"") != std::string::npos)
        return Fail("M2e-22 gap (colorRect) not cleared", j);
    if (gapBlock.find("MessageLayer.tjs") != std::string::npos && gapLine == 587)
        return Fail("the chain is still stuck at the MessageLayer.tjs colorRect line", j);

    // M2e-23 硬判据：run 44213673737 的下一缺口是 MessageLayer.tjs:1987 的
    // `showParentHint = false;`（link 隐藏路径，全库零读点）。独立负向对照：
    //   NOSHOWPARENTHINT -> 必须复现 showParentHint@MessageLayer.tjs:1987
    if (fatal.find("\"showParentHint\"") != std::string::npos ||
        inner.find("\"showParentHint\"") != std::string::npos)
        return Fail("M2e-23 gap (showParentHint) not cleared", j);
    if (gapBlock.find("MessageLayer.tjs") != std::string::npos && gapLine == 1987)
        return Fail("the chain is still stuck at the MessageLayer.tjs showParentHint line", j);

    // M2e-24 硬判据：run 44214822084 的下一缺口是 MessageLayer.tjs:607 的
    // `lastMouseX = cursorX;`（clear() 记录鼠标位置，cursorY 紧随其后）。
    // 独立负向对照：NOCURSORX -> 必须复现 cursorX@MessageLayer.tjs:607
    // 注意判据用带引号的完整成员名：裸 "cursor" 是 M2e-17 已关闭的缺口，
    // 而 "cursorX" 包含 "cursor" 子串，裸匹配会把新缺口误判成旧缺口。
    if (fatal.find("\"cursorX\"") != std::string::npos ||
        inner.find("\"cursorX\"") != std::string::npos)
        return Fail("M2e-24 gap (cursorX) not cleared", j);
    if (gapBlock.find("MessageLayer.tjs") != std::string::npos && gapLine == 607)
        return Fail("the chain is still stuck at the MessageLayer.tjs cursorX line", j);

    // M2e-25 硬判据：run 44216212216 的下一缺口是 BGM.tjs:114 的
    // `class KAGMIDISoundBuffer extends MIDISoundBuffer, KAGSoundBuffer`
    // —— 首次跳出视觉类，进入音频类。独立负向对照：
    //   NOMIDI -> 必须复现 MIDISoundBuffer@BGM.tjs:114
    if (fatal.find("\"MIDISoundBuffer\"") != std::string::npos ||
        inner.find("\"MIDISoundBuffer\"") != std::string::npos)
        return Fail("M2e-25 gap (MIDISoundBuffer) not cleared", j);
    if (gapBlock.find("BGM.tjs") != std::string::npos && gapLine == 114)
        return Fail("the chain is still stuck at the BGM.tjs MIDISoundBuffer line", j);

    // M2e-26 硬判据：run 44217840651 的下一缺口是 SE.tjs:4 的
    // `class SESoundBuffer extends WaveSoundBuffer`（同族音频类）。
    // 独立负向对照：NOWAVE -> 必须复现 WaveSoundBuffer@SE.tjs:4
    // 注意：判定用带引号全名 —— 裸 "WaveSoundBuffer" 会命中 KAGWaveSoundBuffer？
    // 不会（后者的引号在 KAG 前），但仍统一用带引号全名保持口径一致。
    if (fatal.find("\"WaveSoundBuffer\"") != std::string::npos ||
        inner.find("\"WaveSoundBuffer\"") != std::string::npos)
        return Fail("M2e-26 gap (WaveSoundBuffer) not cleared", j);
    if (gapBlock.find("SE.tjs") != std::string::npos && gapLine == 4)
        return Fail("the chain is still stuck at the SE.tjs WaveSoundBuffer line", j);

    // M2e-27 硬判据：run 44298320673 的下一缺口是 Movie.tjs:4 的
    // `class Movie extends VideoOverlay`（影片播放类，音频之后的下一领域）。
    // 独立负向对照：NOVIDEO -> 必须复现 VideoOverlay@Movie.tjs:4
    if (fatal.find("\"VideoOverlay\"") != std::string::npos ||
        inner.find("\"VideoOverlay\"") != std::string::npos)
        return Fail("M2e-27 gap (VideoOverlay) not cleared", j);
    if (gapBlock.find("Movie.tjs") != std::string::npos && gapLine == 4)
        return Fail("the chain is still stuck at the Movie.tjs VideoOverlay line", j);

    // M2e-28 硬判据：run 44299192628 的下一缺口是 MainWindow.tjs:438 的
    // `this.width + this.left`（窗口居中修正，439/441 行还会回写）。
    // 归属是 **Window** 的 left/top，不是 Layer 的（后者 M2e-2 已有）。
    // 独立负向对照：NOWINLEFT -> 必须复现 left@MainWindow.tjs:438
    if (fatal.find("\"left\"") != std::string::npos ||
        inner.find("\"left\"") != std::string::npos)
        return Fail("M2e-28 gap (Window.left) not cleared", j);
    if (gapBlock.find("MainWindow.tjs") != std::string::npos && gapLine == 438)
        return Fail("the chain is still stuck at the MainWindow.tjs Window.left line", j);

    // M2e-29 硬判据：run 44311767621 的下一缺口是 BGM.tjs:585 的
    // `dic.paused = buf1.paused;`（store() 存暂停位）。注意这是对**类实例**
    // 的属性访问 → 缺 member 会真抛错（不同于 M2e-28 的 this.xxx 代理返回 void）。
    // 独立负向对照：NOPAUSED -> 必须复现 paused@BGM.tjs:585
    if (fatal.find("\"paused\"") != std::string::npos ||
        inner.find("\"paused\"") != std::string::npos)
        return Fail("M2e-29 gap (paused) not cleared", j);
    if (gapBlock.find("BGM.tjs") != std::string::npos && gapLine == 585)
        return Fail("the chain is still stuck at the BGM.tjs paused line", j);

    // M2e-30 硬判据：run 44317583445 的下一缺口是 SE.tjs:221
    // `return super.pan \ 1000;`（property pan 的 getter 读基类成员）。
    // 独立负向对照：NOPAN -> 必须到不了后续脚本（见 RunCaseNoPan 里的口径说明）。
    if (fatal.find("\"pan\"") != std::string::npos ||
        inner.find("\"pan\"") != std::string::npos)
        return Fail("M2e-30 gap (pan) not cleared", j);
    if (gapBlock.find("SE.tjs") != std::string::npos && gapLine == 221)
        return Fail("the chain is still stuck at the SE.tjs pan line", j);

    // M2e-31 硬判据：run 44323307593 的下一缺口是 Movie.tjs:30
    // `dic.mode = mode;`（VideoOverlay 的播放模式，普通实例成员）。
    if (fatal.find("\"mode\"") != std::string::npos ||
        inner.find("\"mode\"") != std::string::npos)
        return Fail("M2e-31 gap (mode) not cleared", j);
    if (gapBlock.find("Movie.tjs") != std::string::npos && gapLine == 30)
        return Fail("the chain is still stuck at the Movie.tjs mode line", j);

    // M2e-32 硬判据：run 44326396559 的下一缺口是 MainWindow.tjs:1045 的
    // `Scripts.evalStorage(fn)`（读存档/系统变量数据，原生方法缺失），
    // 被官方 catch 包成「システム変数データを読み込めないか...」。
    // evalStorage 是 C++ 原生方法，不是 TJS 骨架 —— 负向对照用宿主开关
    // 关掉注册来复现（见 RunCaseNoEvalStorage）。这里要求缺口清除 +
    // 方法确实注册（evalStorageEnabled=true）。
    if (fatal.find("evalStorage") != std::string::npos ||
        inner.find("evalStorage") != std::string::npos ||
        fatal.find("システム変数データを読み込めないか") != std::string::npos)
        return Fail("M2e-32 gap (Scripts.evalStorage) not cleared", j);
    if (gapBlock.find("MainWindow.tjs") != std::string::npos && gapLine == 1045)
        return Fail("the chain is still stuck at the MainWindow.tjs evalStorage line", j);
    if (!Has(j, "\"evalStorageEnabled\":true"))
        return Fail("M2e-32: Scripts.evalStorage was not registered", j);

    // M2e-33 硬判据：run 44480536205 的下一缺口是 Movie.tjs:33
    // `dic.visible = visible;`（VideoOverlay 的可见标志，普通实例成员）。
    // 独立负向对照：NOVISIBLE -> 必须复现 visible@Movie.tjs:33。
    if (fatal.find("\"visible\"") != std::string::npos ||
        inner.find("\"visible\"") != std::string::npos)
        return Fail("M2e-33 gap (visible) not cleared", j);
    if (gapBlock.find("Movie.tjs") != std::string::npos && gapLine == 33)
        return Fail("the chain is still stuck at the Movie.tjs visible line", j);

    // M2e-34 硬判据：run 44484142123 的下一缺口是 Movie.tjs:34
    // `dic.loop = loop;`（VideoOverlay 的循环播放标志，普通实例成员）。
    // 独立负向对照：NOLOOP -> 必须复现 loop@Movie.tjs:34。
    if (fatal.find("\"loop\"") != std::string::npos ||
        inner.find("\"loop\"") != std::string::npos)
        return Fail("M2e-34 gap (loop) not cleared", j);
    if (gapBlock.find("Movie.tjs") != std::string::npos && gapLine == 34)
        return Fail("the chain is still stuck at the Movie.tjs loop line", j);

    // M2e-35 硬判据：run 44485784048 的下一缺口是 Movie.tjs:35
    // `dic.frame = frame;`（VideoOverlay 的当前帧号，普通实例成员）。
    // 独立负向对照：NOFRAME -> 必须复现 frame@Movie.tjs:35。
    if (fatal.find("\"frame\"") != std::string::npos ||
        inner.find("\"frame\"") != std::string::npos)
        return Fail("M2e-35 gap (frame) not cleared", j);
    if (gapBlock.find("Movie.tjs") != std::string::npos && gapLine == 35)
        return Fail("the chain is still stuck at the Movie.tjs frame line", j);

    // M2e-36 硬判据：run 44524894739 的下一缺口是 Movie.tjs:37
    // `if( numberOfAudioStream > 0 )`（音频流数量，普通实例成员）。
    // 独立负向对照：NOAUDIOSTREAM -> 必须复现 numberOfAudioStream@Movie.tjs:37。
    if (fatal.find("numberOfAudioStream") != std::string::npos ||
        inner.find("numberOfAudioStream") != std::string::npos)
        return Fail("M2e-36 gap (numberOfAudioStream) not cleared", j);
    if (gapBlock.find("Movie.tjs") != std::string::npos && gapLine == 37)
        return Fail("the chain is still stuck at the Movie.tjs numberOfAudioStream line", j);

    // M2e-37 硬判据：run 44526837834 的下一缺口是 Movie.tjs:44
    // `dic.segmentLoopStartFrame = segmentLoopStartFrame;`（分段循环起始帧）。
    // 独立负向对照：NOSEGSTART -> 必须复现 segmentLoopStartFrame@Movie.tjs:44。
    if (fatal.find("segmentLoopStartFrame") != std::string::npos ||
        inner.find("segmentLoopStartFrame") != std::string::npos)
        return Fail("M2e-37 gap (segmentLoopStartFrame) not cleared", j);
    if (gapBlock.find("Movie.tjs") != std::string::npos && gapLine == 44)
        return Fail("the chain is still stuck at the Movie.tjs segmentLoopStartFrame line", j);

    // M2e-38 硬判据：run 44528210242 的下一缺口是 Movie.tjs:45
    // `dic.segmentLoopEndFrame = segmentLoopEndFrame;`（分段循环结束帧）。
    // 独立负向对照：NOSEGEND -> 必须复现 segmentLoopEndFrame@Movie.tjs:45。
    if (fatal.find("segmentLoopEndFrame") != std::string::npos ||
        inner.find("segmentLoopEndFrame") != std::string::npos)
        return Fail("M2e-38 gap (segmentLoopEndFrame) not cleared", j);
    if (gapBlock.find("Movie.tjs") != std::string::npos && gapLine == 45)
        return Fail("the chain is still stuck at the Movie.tjs segmentLoopEndFrame line", j);

    // M2e-39 硬判据：run 44531943383 的下一缺口是 Movie.tjs:46
    // `dic.periodEventFrame = periodEventFrame;`（周期事件帧）。
    // 独立负向对照：NOPERIOD -> 必须复现 periodEventFrame@Movie.tjs:46。
    if (fatal.find("periodEventFrame") != std::string::npos ||
        inner.find("periodEventFrame") != std::string::npos)
        return Fail("M2e-39 gap (periodEventFrame) not cleared", j);
    if (gapBlock.find("Movie.tjs") != std::string::npos && gapLine == 46)
        return Fail("the chain is still stuck at the Movie.tjs periodEventFrame line", j);

    // M2e-40 硬判据：run 44545238491 的下一缺口是 Conductor.tjs:213
    // `resetInterrupt();`（startProcess() 里裸标识符调用的 KAGParser 原生方法）。
    // 独立负向对照：NORESINT -> 必须复现 resetInterrupt@Conductor.tjs:213。
    if (fatal.find("resetInterrupt") != std::string::npos ||
        inner.find("resetInterrupt") != std::string::npos)
        return Fail("M2e-40 gap (resetInterrupt) not cleared", j);
    if (gapBlock.find("Conductor.tjs") != std::string::npos && gapLine == 213)
        return Fail("the chain is still stuck at the Conductor.tjs resetInterrupt line", j);

    // M2e-41 硬判据：run 44679025740 的下一缺口是 Conductor.tjs:180
    // `"ファイル : " + curStorage + ...`（timerCallback catch 里的裸标识符读，
    // 属父类 KAGParser 的 scenario 文件名槽位）。
    // 独立负向对照：NOCURSTOR -> 必须复现 curStorage@Conductor.tjs:180。
    //
    // ⚠️ 判据只能落在**变量名**上，不能加行号断言：180 行里并列着
    // `curStorage` 和 `curLine`（`(curLine+1)`），而 TJS2 的字符串拼接是
    // 从左到右求值的 —— 补完 curStorage 后，下一缺口就是 **curLine@180**，
    // 行号仍是 180 但变量换了。若在这里按行号判失败，
    // 判据就会变成自我实现的假红（下一轮补 curLine 时必然撞上）。
    if (fatal.find("curStorage") != std::string::npos ||
        inner.find("curStorage") != std::string::npos)
        return Fail("M2e-41 gap (curStorage) not cleared", j);

    // M2e-42 硬判据：run 44683997999 的下一缺口是 Conductor.tjs:180
    // `(curLine+1)`（与 curStorage 同一行的第二个裸标识符读，
    // 属父类 KAGParser 的 scenario 行号槽位）。
    // 独立负向对照：NOCURLINE -> 必须复现 curLine@Conductor.tjs:180。
    //
    // ⚠️ 同样**不绑行号**：180 行已被 curStorage 用过一次，按行号判会让
    // 两个里程碑互相打架（补 curLine 时 fatal 仍在 180 行）。只按变量名判。
    if (fatal.find("curLine") != std::string::npos ||
        inner.find("curLine") != std::string::npos)
        return Fail("M2e-42 gap (curLine) not cleared", j);

    // M2e-43 硬判据：run 44684761294 的下一缺口是 Conductor.tjs:81
    // `obj = getNextTag();`（timerCallback 里裸标识符调用的 KAGParser 原生方法）。
    // 独立负向对照：NOGETNEXT -> 必须复现 getNextTag 缺口。
    //
    // ⚠️ fatal 形态变了：getNextTag 在 try 块内抛错 → 被 172 行 catch 捕获 →
    // catch 里拼错误信息（此时才用到 curStorage/curLine）→ 186 行
    // `throw new ConductorException(msg)` 重抛。所以 A 场景的 fatal 是
    // **多行 script exception 文本**（内含 `Member "getNextTag" does not exist`），
    // 且 fatalLine 报的是**重抛点 189**而非调用点 81。
    // ⇒ 判据按**变量名**命中（fatal 多行文本里含该成员名），不绑行号。
    if (fatal.find("getNextTag") != std::string::npos ||
        inner.find("getNextTag") != std::string::npos)
        return Fail("M2e-43 gap (getNextTag) not cleared", j);

    // 新缺口只记录，不预设下一缺口的名字 —— 避免判据变成自我实现的预言。
    std::printf("[m2h] A next gap: '%s' @ %s:%lld (after M2e-43)\n",
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
    // M2e-25：MIDISoundBuffer 骨架必须真的装进引擎（BGM.tjs:114 类定义需要它）。
    if (shim.find("M2E25_SOUND_SKELETON") == std::string::npos)
        return Fail("M2e-25 regression: Sound skeleton was not loaded", j);
    // M2e-27：VideoOverlay 骨架必须真的装进引擎（Movie.tjs:4 类定义需要它）。
    if (shim.find("M2E27_MOVIE_SKELETON") == std::string::npos)
        return Fail("M2e-27 regression: Movie skeleton was not loaded", j);

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
        // 带初值匹配：裸 "var cursor" 会被 M2e-24 的 var cursorX 子串命中
        // （run 44215568706 / 44215904431 两轮实测假红，子串误判第三次同类坑）。
        if (body.find("var cursor = crDefault") != std::string::npos)
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

// ---- 场景 NONAME：Layer 缺图层名成员 name 的负向对照（M2e-18）----------
// 精准对照：Layer 骨架保留（几何/命中面/图像面/绘制面/光标面都在），只删
// M2E18 段。必须复现 `Member "name" does not exist @ HistoryLayer.tjs:110`。
static int RunCaseNoName(const char *noname, const char *storage) {
    if (!Exists(std::string(noname) + "/system/Config.tjs"))
        return Fail("NONAME fixture missing Config.tjs", NULL);
    const std::string skel = std::string(noname) + "/system/GalBoxLayer.tjs";
    if (!Exists(skel))
        return Fail("NONAME fixture must keep the Layer skeleton", NULL);
    {
        std::ifstream f(skel.c_str());
        std::string body((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        if (body.find("M2E18_BEGIN") != std::string::npos)
            return Fail("NONAME fixture must have the name member removed", NULL);
        if (body.find("var name") != std::string::npos)
            return Fail("NONAME fixture still declares name", NULL);
        for (const char *keep : {"var cursor", "var face", "function fillRect",
                                 "var imageWidth", "var hitType", "class Layer"}) {
            if (body.find(keep) == std::string::npos) {
                std::printf("[m2h] NONAME fixture lost '%s'\n", keep);
                return Fail("NONAME fixture lost unrelated Layer members", NULL);
            }
        }
    }

    galbox::XP3Archive arc;
    char *j = RunChain(noname, storage, arc);
    if (!j) return Fail("cannot run the NONAME control", NULL);
    DumpChain("NONAME", j);

    if (StrField(j, "shim").find("M2E2_LAYER_SKELETON") == std::string::npos)
        return Fail("NONAME control lost the Layer skeleton tag", j);

    const std::string fatal = StrField(j, "fatal");
    const std::string inner = StrField(j, "lastScriptError");
    const std::string block = StrField(j, "fatalBlock");
    const long long line = IntField(j, "fatalLine");
    const bool gap = (fatal + " || " + inner).find("\"name\"") != std::string::npos;
    if (!gap || block.find("HistoryLayer.tjs") == std::string::npos || line != 110)
        return Fail("NONAME control did not reproduce the name gap", j);

    std::printf("[m2h] NONAME verdict: fatal=\"%s\" @ %s:%lld\n",
                fatal.c_str(), block.c_str(), line);
    GalBoxTJS2Free(j);
    return 0;
}

// ---- 场景 NOFOCUSABLE：Layer 缺焦点接收成员 focusable 的负向对照（M2e-19）--
// 精准对照：Layer 骨架保留（几何/命中面/图像面/绘制面/光标面/图层名都在），
// 只删 M2E19 段。必须复现 `Member "focusable" does not exist @ HistoryLayer.tjs:130`。
static int RunCaseNoFocusable(const char *nofocusable, const char *storage) {
    if (!Exists(std::string(nofocusable) + "/system/Config.tjs"))
        return Fail("NOFOCUSABLE fixture missing Config.tjs", NULL);
    const std::string skel = std::string(nofocusable) + "/system/GalBoxLayer.tjs";
    if (!Exists(skel))
        return Fail("NOFOCUSABLE fixture must keep the Layer skeleton", NULL);
    {
        std::ifstream f(skel.c_str());
        std::string body((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        if (body.find("M2E19_BEGIN") != std::string::npos)
            return Fail("NOFOCUSABLE fixture must have the focusable member removed", NULL);
        if (body.find("var focusable") != std::string::npos)
            return Fail("NOFOCUSABLE fixture still declares focusable", NULL);
        for (const char *keep : {"var name", "var cursor", "var face",
                                 "function fillRect", "var imageWidth",
                                 "var hitType", "class Layer"}) {
            if (body.find(keep) == std::string::npos) {
                std::printf("[m2h] NOFOCUSABLE fixture lost '%s'\n", keep);
                return Fail("NOFOCUSABLE fixture lost unrelated Layer members", NULL);
            }
        }
    }

    galbox::XP3Archive arc;
    char *j = RunChain(nofocusable, storage, arc);
    if (!j) return Fail("cannot run the NOFOCUSABLE control", NULL);
    DumpChain("NOFOCUSABLE", j);

    if (StrField(j, "shim").find("M2E2_LAYER_SKELETON") == std::string::npos)
        return Fail("NOFOCUSABLE control lost the Layer skeleton tag", j);

    const std::string fatal = StrField(j, "fatal");
    const std::string inner = StrField(j, "lastScriptError");
    const std::string block = StrField(j, "fatalBlock");
    const long long line = IntField(j, "fatalLine");
    const bool gap = (fatal + " || " + inner).find("\"focusable\"") != std::string::npos;
    if (!gap || block.find("HistoryLayer.tjs") == std::string::npos || line != 130)
        return Fail("NOFOCUSABLE control did not reproduce the focusable gap", j);

    std::printf("[m2h] NOFOCUSABLE verdict: fatal=\"%s\" @ %s:%lld\n",
                fatal.c_str(), block.c_str(), line);
    GalBoxTJS2Free(j);
    return 0;
}

// ---- 场景 NOSIZE：Layer 缺显示尺寸方法 setSize 的负向对照（M2e-20）--------
// 精准对照：Layer 骨架保留（几何/命中/图像/绘制/光标/名字/焦点面都在），
// 只删 M2E20 段。必须复现 `Member "setSize" does not exist @ AnimationLayer.tjs:367`。
static int RunCaseNoSize(const char *nosize, const char *storage) {
    if (!Exists(std::string(nosize) + "/system/Config.tjs"))
        return Fail("NOSIZE fixture missing Config.tjs", NULL);
    const std::string skel = std::string(nosize) + "/system/GalBoxLayer.tjs";
    if (!Exists(skel))
        return Fail("NOSIZE fixture must keep the Layer skeleton", NULL);
    {
        std::ifstream f(skel.c_str());
        std::string body((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        if (body.find("M2E20_BEGIN") != std::string::npos)
            return Fail("NOSIZE fixture must have the setSize method removed", NULL);
        // ⚠️ 必须带参数列表精确匹配 function setSize(w, h)：
        // 只搜 "function setSize" 会命中同文件的 function setSizeToImageSize
        // 子串，导致删对了的夹具被误判成"还留着"（run 44210836253 踩过）。
        if (body.find("function setSize(w, h)") != std::string::npos)
            return Fail("NOSIZE fixture still declares setSize", NULL);
        // setSizeToImageSize 是另一个成员，别一起删了。
        for (const char *keep : {"function setSizeToImageSize", "function setPos",
                                 "var focusable", "var name", "var cursor",
                                 "var face", "class Layer"}) {
            if (body.find(keep) == std::string::npos) {
                std::printf("[m2h] NOSIZE fixture lost '%s'\n", keep);
                return Fail("NOSIZE fixture lost unrelated Layer members", NULL);
            }
        }
    }

    galbox::XP3Archive arc;
    char *j = RunChain(nosize, storage, arc);
    if (!j) return Fail("cannot run the NOSIZE control", NULL);
    DumpChain("NOSIZE", j);

    if (StrField(j, "shim").find("M2E2_LAYER_SKELETON") == std::string::npos)
        return Fail("NOSIZE control lost the Layer skeleton tag", j);

    const std::string fatal = StrField(j, "fatal");
    const std::string inner = StrField(j, "lastScriptError");
    const std::string block = StrField(j, "fatalBlock");
    const long long line = IntField(j, "fatalLine");
    const bool gap = (fatal + " || " + inner).find("\"setSize\"") != std::string::npos;
    if (!gap || block.find("AnimationLayer.tjs") == std::string::npos || line != 367)
        return Fail("NOSIZE control did not reproduce the setSize gap", j);

    std::printf("[m2h] NOSIZE verdict: fatal=\"%s\" @ %s:%lld\n",
                fatal.c_str(), block.c_str(), line);
    GalBoxTJS2Free(j);
    return 0;
}

// ---- 场景 NOIMAGEMODIFIED：Layer 缺图像脏标记 imageModified 的负向对照（M2e-21）
// 精准对照：只删 M2E21 段。必须复现
// `Member "imageModified" does not exist @ MessageLayer.tjs:493`。
static int RunCaseNoImageModified(const char *noim, const char *storage) {
    if (!Exists(std::string(noim) + "/system/Config.tjs"))
        return Fail("NOIMAGEMODIFIED fixture missing Config.tjs", NULL);
    const std::string skel = std::string(noim) + "/system/GalBoxLayer.tjs";
    if (!Exists(skel))
        return Fail("NOIMAGEMODIFIED fixture must keep the Layer skeleton", NULL);
    {
        std::ifstream f(skel.c_str());
        std::string body((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        if (body.find("M2E21_BEGIN") != std::string::npos)
            return Fail("NOIMAGEMODIFIED fixture must have imageModified removed", NULL);
        if (body.find("var imageModified") != std::string::npos)
            return Fail("NOIMAGEMODIFIED fixture still declares imageModified", NULL);
        // setSize / focusable 是紧邻前两轮的成员，别一起删了。
        for (const char *keep : {"function setSize(w, h)", "var focusable",
                                 "var name", "var cursor", "var face",
                                 "function setSizeToImageSize", "class Layer"}) {
            if (body.find(keep) == std::string::npos) {
                std::printf("[m2h] NOIMAGEMODIFIED fixture lost '%s'\n", keep);
                return Fail("NOIMAGEMODIFIED fixture lost unrelated Layer members", NULL);
            }
        }
    }

    galbox::XP3Archive arc;
    char *j = RunChain(noim, storage, arc);
    if (!j) return Fail("cannot run the NOIMAGEMODIFIED control", NULL);
    DumpChain("NOIMAGEMODIFIED", j);

    if (StrField(j, "shim").find("M2E2_LAYER_SKELETON") == std::string::npos)
        return Fail("NOIMAGEMODIFIED control lost the Layer skeleton tag", j);

    const std::string fatal = StrField(j, "fatal");
    const std::string inner = StrField(j, "lastScriptError");
    const std::string block = StrField(j, "fatalBlock");
    const long long line = IntField(j, "fatalLine");
    const bool gap = (fatal + " || " + inner).find("\"imageModified\"") != std::string::npos;
    if (!gap || block.find("MessageLayer.tjs") == std::string::npos || line != 493)
        return Fail("NOIMAGEMODIFIED control did not reproduce the imageModified gap", j);

    std::printf("[m2h] NOIMAGEMODIFIED verdict: fatal=\"%s\" @ %s:%lld\n",
                fatal.c_str(), block.c_str(), line);
    GalBoxTJS2Free(j);
    return 0;
}

// ---- 场景 NOCOLORRECT：Layer 缺颜色矩形填充 colorRect 的负向对照（M2e-22）
// 精准对照：只删 M2E22 段。必须复现
// `Member "colorRect" does not exist @ MessageLayer.tjs:587`。
static int RunCaseNoColorRect(const char *nocr, const char *storage) {
    if (!Exists(std::string(nocr) + "/system/Config.tjs"))
        return Fail("NOCOLORRECT fixture missing Config.tjs", NULL);
    const std::string skel = std::string(nocr) + "/system/GalBoxLayer.tjs";
    if (!Exists(skel))
        return Fail("NOCOLORRECT fixture must keep the Layer skeleton", NULL);
    {
        std::ifstream f(skel.c_str());
        std::string body((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        if (body.find("M2E22_BEGIN") != std::string::npos)
            return Fail("NOCOLORRECT fixture must have colorRect removed", NULL);
        // 带完整参数列表匹配，防 fillRect 子串混淆（fillRect 少一个字母但
        // 名字互不包含，这里仍用完整签名 + 单独确认 fillRect 还在）。
        if (body.find("function colorRect") != std::string::npos)
            return Fail("NOCOLORRECT fixture still declares colorRect", NULL);
        // imageModified / setSize 是紧邻前两轮的成员，别一起删了。
        for (const char *keep : {"function fillRect", "function setSize(w, h)",
                                 "var imageModified", "var focusable",
                                 "var name", "var cursor", "var face",
                                 "function setSizeToImageSize", "class Layer"}) {
            if (body.find(keep) == std::string::npos) {
                std::printf("[m2h] NOCOLORRECT fixture lost '%s'\n", keep);
                return Fail("NOCOLORRECT fixture lost unrelated Layer members", NULL);
            }
        }
    }

    galbox::XP3Archive arc;
    char *j = RunChain(nocr, storage, arc);
    if (!j) return Fail("cannot run the NOCOLORRECT control", NULL);
    DumpChain("NOCOLORRECT", j);

    if (StrField(j, "shim").find("M2E2_LAYER_SKELETON") == std::string::npos)
        return Fail("NOCOLORRECT control lost the Layer skeleton tag", j);

    const std::string fatal = StrField(j, "fatal");
    const std::string inner = StrField(j, "lastScriptError");
    const std::string block = StrField(j, "fatalBlock");
    const long long line = IntField(j, "fatalLine");
    const bool gap = (fatal + " || " + inner).find("\"colorRect\"") != std::string::npos;
    if (!gap || block.find("MessageLayer.tjs") == std::string::npos || line != 587)
        return Fail("NOCOLORRECT control did not reproduce the colorRect gap", j);

    std::printf("[m2h] NOCOLORRECT verdict: fatal=\"%s\" @ %s:%lld\n",
                fatal.c_str(), block.c_str(), line);
    GalBoxTJS2Free(j);
    return 0;
}

// ---- 场景 NOSHOWPARENTHINT：Layer 缺父层提示开关的负向对照（M2e-23）
// 精准对照：只删 M2E23 段。必须复现
// `Member "showParentHint" does not exist @ MessageLayer.tjs:1987`。
static int RunCaseNoShowParentHint(const char *noph, const char *storage) {
    if (!Exists(std::string(noph) + "/system/Config.tjs"))
        return Fail("NOSHOWPARENTHINT fixture missing Config.tjs", NULL);
    const std::string skel = std::string(noph) + "/system/GalBoxLayer.tjs";
    if (!Exists(skel))
        return Fail("NOSHOWPARENTHINT fixture must keep the Layer skeleton", NULL);
    {
        std::ifstream f(skel.c_str());
        std::string body((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        if (body.find("M2E23_BEGIN") != std::string::npos)
            return Fail("NOSHOWPARENTHINT fixture must have showParentHint removed", NULL);
        if (body.find("var showParentHint") != std::string::npos)
            return Fail("NOSHOWPARENTHINT fixture still declares showParentHint", NULL);
        // colorRect 是紧邻上一轮的成员，别一起删了。
        for (const char *keep : {"function colorRect", "function fillRect",
                                 "function setSize(w, h)", "var imageModified",
                                 "var focusable", "var name", "var cursor",
                                 "var face", "function setSizeToImageSize",
                                 "class Layer"}) {
            if (body.find(keep) == std::string::npos) {
                std::printf("[m2h] NOSHOWPARENTHINT fixture lost '%s'\n", keep);
                return Fail("NOSHOWPARENTHINT fixture lost unrelated Layer members", NULL);
            }
        }
    }

    galbox::XP3Archive arc;
    char *j = RunChain(noph, storage, arc);
    if (!j) return Fail("cannot run the NOSHOWPARENTHINT control", NULL);
    DumpChain("NOSHOWPARENTHINT", j);

    if (StrField(j, "shim").find("M2E2_LAYER_SKELETON") == std::string::npos)
        return Fail("NOSHOWPARENTHINT control lost the Layer skeleton tag", j);

    const std::string fatal = StrField(j, "fatal");
    const std::string inner = StrField(j, "lastScriptError");
    const std::string block = StrField(j, "fatalBlock");
    const long long line = IntField(j, "fatalLine");
    const bool gap = (fatal + " || " + inner).find("\"showParentHint\"") != std::string::npos;
    if (!gap || block.find("MessageLayer.tjs") == std::string::npos || line != 1987)
        return Fail("NOSHOWPARENTHINT control did not reproduce the showParentHint gap", j);

    std::printf("[m2h] NOSHOWPARENTHINT verdict: fatal=\"%s\" @ %s:%lld\n",
                fatal.c_str(), block.c_str(), line);
    GalBoxTJS2Free(j);
    return 0;
}

// ---- 场景 NOCURSORX：Layer 缺层内鼠标坐标 cursorX/cursorY 的负向对照（M2e-24）
// 精准对照：只删 M2E24 段（cursorX/cursorY 成对声明、一起删）。必须复现
// `Member "cursorX" does not exist @ MessageLayer.tjs:607`。
static int RunCaseNoCursorX(const char *nocx, const char *storage) {
    if (!Exists(std::string(nocx) + "/system/Config.tjs"))
        return Fail("NOCURSORX fixture missing Config.tjs", NULL);
    const std::string skel = std::string(nocx) + "/system/GalBoxLayer.tjs";
    if (!Exists(skel))
        return Fail("NOCURSORX fixture must keep the Layer skeleton", NULL);
    {
        std::ifstream f(skel.c_str());
        std::string body((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        if (body.find("M2E24_BEGIN") != std::string::npos)
            return Fail("NOCURSORX fixture must have cursorX/cursorY removed", NULL);
        if (body.find("var cursorX") != std::string::npos ||
            body.find("var cursorY") != std::string::npos)
            return Fail("NOCURSORX fixture still declares cursorX/cursorY", NULL);
        // M2e-17 的 var cursor 是不同成员；带初值匹配防与 cursorX 混淆。
        for (const char *keep : {"var cursor = crDefault", "var showParentHint",
                                 "function colorRect", "function fillRect",
                                 "function setSize(w, h)", "var imageModified",
                                 "var focusable", "var name", "var face",
                                 "function setSizeToImageSize", "class Layer"}) {
            if (body.find(keep) == std::string::npos) {
                std::printf("[m2h] NOCURSORX fixture lost '%s'\n", keep);
                return Fail("NOCURSORX fixture lost unrelated Layer members", NULL);
            }
        }
    }

    galbox::XP3Archive arc;
    char *j = RunChain(nocx, storage, arc);
    if (!j) return Fail("cannot run the NOCURSORX control", NULL);
    DumpChain("NOCURSORX", j);

    if (StrField(j, "shim").find("M2E2_LAYER_SKELETON") == std::string::npos)
        return Fail("NOCURSORX control lost the Layer skeleton tag", j);

    const std::string fatal = StrField(j, "fatal");
    const std::string inner = StrField(j, "lastScriptError");
    const std::string block = StrField(j, "fatalBlock");
    const long long line = IntField(j, "fatalLine");
    const bool gap = (fatal + " || " + inner).find("\"cursorX\"") != std::string::npos;
    if (!gap || block.find("MessageLayer.tjs") == std::string::npos || line != 607)
        return Fail("NOCURSORX control did not reproduce the cursorX gap", j);

    std::printf("[m2h] NOCURSORX verdict: fatal=\"%s\" @ %s:%lld\n",
                fatal.c_str(), block.c_str(), line);
    GalBoxTJS2Free(j);
    return 0;
}

// ---- 场景 NOMIDI：缺 MIDISoundBuffer 骨架的负向对照（M2e-25）
// 文件级删除（同 M2e-7/11/12 模式）：夹具不放 GalBoxSound.tjs。
// 必须复现 `Member "MIDISoundBuffer" does not exist @ BGM.tjs:114`。
static int RunCaseNoMidi(const char *nomidi, const char *storage) {
    if (!Exists(std::string(nomidi) + "/system/Config.tjs"))
        return Fail("NOMIDI fixture missing Config.tjs", NULL);
    if (Exists(std::string(nomidi) + "/system/GalBoxSound.tjs"))
        return Fail("NOMIDI fixture must not contain GalBoxSound.tjs", NULL);

    galbox::XP3Archive arc;
    char *j = RunChain(nomidi, storage, arc);
    if (!j) return Fail("cannot run the NOMIDI control", NULL);
    DumpChain("NOMIDI", j);

    // 骨架文件被删 ⇒ shim 里不许出现 M2E25 tag（tag 只在 ExecScript 成功后写入）。
    if (StrField(j, "shim").find("M2E25_SOUND_SKELETON") != std::string::npos)
        return Fail("NOMIDI control unexpectedly loaded the Sound skeleton", j);
    // 但 Layer 骨架必须还在（删的只是 Sound）。
    if (StrField(j, "shim").find("M2E2_LAYER_SKELETON") == std::string::npos)
        return Fail("NOMIDI control lost the Layer skeleton tag", j);

    const std::string fatal = StrField(j, "fatal");
    const std::string inner = StrField(j, "lastScriptError");
    const std::string block = StrField(j, "fatalBlock");
    const long long line = IntField(j, "fatalLine");
    const bool gap = (fatal + " || " + inner).find("\"MIDISoundBuffer\"") != std::string::npos;
    if (!gap || block.find("BGM.tjs") == std::string::npos || line != 114)
        return Fail("NOMIDI control did not reproduce the MIDISoundBuffer gap", j);

    std::printf("[m2h] NOMIDI verdict: fatal=\"%s\" @ %s:%lld\n",
                fatal.c_str(), block.c_str(), line);
    GalBoxTJS2Free(j);
    return 0;
}

// ---- 场景 NOWAVE：缺 WaveSoundBuffer 类的负向对照（M2e-26）
// 段级删除（文件保留）：只把 GalBoxSound.tjs 里 M2E26 段删掉，
// MIDISoundBuffer 仍在（否则测的是 M2e-25 那个缺口）。
// 必须复现 `Member "WaveSoundBuffer" does not exist @ SE.tjs:4`。
static int RunCaseNoWave(const char *nowave, const char *storage) {
    if (!Exists(std::string(nowave) + "/system/Config.tjs"))
        return Fail("NOWAVE fixture missing Config.tjs", NULL);
    const std::string snd = std::string(nowave) + "/system/GalBoxSound.tjs";
    if (!Exists(snd))
        return Fail("NOWAVE fixture must keep GalBoxSound.tjs", NULL);
    {
        std::ifstream f(snd.c_str());
        std::string body((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        if (body.find("M2E26_BEGIN") != std::string::npos)
            return Fail("NOWAVE fixture must have WaveSoundBuffer removed", NULL);
        if (body.find("class WaveSoundBuffer") != std::string::npos)
            return Fail("NOWAVE fixture still declares WaveSoundBuffer", NULL);
        // MIDISoundBuffer 是上一轮的成员，别一起删了（否则测的不是本轮缺口）。
        if (body.find("class MIDISoundBuffer") == std::string::npos)
            return Fail("NOWAVE fixture lost MIDISoundBuffer", NULL);
    }

    galbox::XP3Archive arc;
    char *j = RunChain(nowave, storage, arc);
    if (!j) return Fail("cannot run the NOWAVE control", NULL);
    DumpChain("NOWAVE", j);

    // 骨架文件仍在、shim tag 仍应装上 —— 唯一差异是 WaveSoundBuffer 类没了。
    if (StrField(j, "shim").find("M2E25_SOUND_SKELETON") == std::string::npos)
        return Fail("NOWAVE control lost the Sound skeleton tag", j);

    const std::string fatal = StrField(j, "fatal");
    const std::string inner = StrField(j, "lastScriptError");
    const std::string block = StrField(j, "fatalBlock");
    const long long line = IntField(j, "fatalLine");
    const bool gap = (fatal + " || " + inner).find("\"WaveSoundBuffer\"") != std::string::npos;
    if (!gap || block.find("SE.tjs") == std::string::npos || line != 4)
        return Fail("NOWAVE control did not reproduce the WaveSoundBuffer gap", j);

    std::printf("[m2h] NOWAVE verdict: fatal=\"%s\" @ %s:%lld\n",
                fatal.c_str(), block.c_str(), line);
    GalBoxTJS2Free(j);
    return 0;
}

// ---- 场景 NOVIDEO：缺 VideoOverlay 骨架的负向对照（M2e-27）
// 文件级删除（同 M2e-25 模式）：夹具不放 GalBoxMovie.tjs。
// 必须复现 `Member "VideoOverlay" does not exist @ Movie.tjs:4`。
static int RunCaseNoVideo(const char *novideo, const char *storage) {
    if (!Exists(std::string(novideo) + "/system/Config.tjs"))
        return Fail("NOVIDEO fixture missing Config.tjs", NULL);
    if (Exists(std::string(novideo) + "/system/GalBoxMovie.tjs"))
        return Fail("NOVIDEO fixture must not contain GalBoxMovie.tjs", NULL);

    galbox::XP3Archive arc;
    char *j = RunChain(novideo, storage, arc);
    if (!j) return Fail("cannot run the NOVIDEO control", NULL);
    DumpChain("NOVIDEO", j);

    // 骨架文件被删 ⇒ shim 里不许出现 M2E27 tag；但 Sound/Layer 骨架必须还在。
    if (StrField(j, "shim").find("M2E27_MOVIE_SKELETON") != std::string::npos)
        return Fail("NOVIDEO control unexpectedly loaded the Movie skeleton", j);
    if (StrField(j, "shim").find("M2E25_SOUND_SKELETON") == std::string::npos ||
        StrField(j, "shim").find("M2E2_LAYER_SKELETON") == std::string::npos)
        return Fail("NOVIDEO control lost unrelated skeletons", j);

    const std::string fatal = StrField(j, "fatal");
    const std::string inner = StrField(j, "lastScriptError");
    const std::string block = StrField(j, "fatalBlock");
    const long long line = IntField(j, "fatalLine");
    const bool gap = (fatal + " || " + inner).find("\"VideoOverlay\"") != std::string::npos;
    if (!gap || block.find("Movie.tjs") == std::string::npos || line != 4)
        return Fail("NOVIDEO control did not reproduce the VideoOverlay gap", j);

    std::printf("[m2h] NOVIDEO verdict: fatal=\"%s\" @ %s:%lld\n",
                fatal.c_str(), block.c_str(), line);
    GalBoxTJS2Free(j);
    return 0;
}

// ---- 场景 NOWINLEFT：Window 骨架缺 left/top 的负向对照（M2e-28）
// 段级删除（GalBoxWindow.tjs 文件保留）：只删 M2E28 段。
// 必须复现 `Member "left" does not exist @ MainWindow.tjs:438`。
// 注意：Layer 骨架的 var left/top（M2e-2）**仍在**，所以这恰恰证明了
// “Layer 有 left”不能替代“Window 有 left”—— 归属没搞错。
static int RunCaseNoWinLeft(const char *nowinl, const char *storage) {
    if (!Exists(std::string(nowinl) + "/system/Config.tjs"))
        return Fail("NOWINLEFT fixture missing Config.tjs", NULL);
    const std::string win = std::string(nowinl) + "/system/GalBoxWindow.tjs";
    if (!Exists(win))
        return Fail("NOWINLEFT fixture must keep GalBoxWindow.tjs", NULL);
    {
        std::ifstream f(win.c_str());
        std::string body((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        if (body.find("M2E28_BEGIN") != std::string::npos)
            return Fail("NOWINLEFT fixture must have Window.left/top removed", NULL);
        if (body.find("var left = 0;") != std::string::npos ||
            body.find("var top = 0;") != std::string::npos)
            return Fail("NOWINLEFT fixture still declares Window.left/top", NULL);
        // M2e-8 的 style 段等必须还在（否则测的是别的缺口）。
        for (const char *keep : {"M2E8_STYLE_BEGIN", "var borderStyle",
                                 "var width = 640", "var height = 480",
                                 "class Window"}) {
            if (body.find(keep) == std::string::npos) {
                std::printf("[m2h] NOWINLEFT fixture lost '%s'\n", keep);
                return Fail("NOWINLEFT fixture lost unrelated Window members", NULL);
            }
        }
    }

    galbox::XP3Archive arc;
    char *j = RunChain(nowinl, storage, arc);
    if (!j) return Fail("cannot run the NOWINLEFT control", NULL);
    DumpChain("NOWINLEFT", j);

    if (StrField(j, "shim").find("M2E3_WINDOW_SKELETON") == std::string::npos)
        return Fail("NOWINLEFT control lost the Window skeleton tag", j);

    const std::string fatal = StrField(j, "fatal");
    const std::string block = StrField(j, "fatalBlock");
    const long long line = IntField(j, "fatalLine");

    // ⚠️ 判据口径（run 44314647942 实测校准）：不在对照里写死“A 的停点行号”——
    // A 的停点每个里程碑都在前移，写死就会“随机过期”。改用**稳定的深度判据**：
    //   有 left/top（A）→ 链能推进到深层官方脚本（SE.tjs / BGM.tjs 一类）；
    //   无 left/top → 链在 MainWindow.tjs 内部就死（void 分支跳过）。
    // 这与“有/无该成员”这个唯一变量绑定，且不随里程碑漂移。
    if (Has(j, "\"ok\":true"))
        return Fail("NOWINLEFT control unexpectedly completed the startup chain", j);
    if (block.find("MainWindow.tjs") == std::string::npos)
        return Fail("NOWINLEFT control still reached the deep scripts — "
                    "Window.left/top would not be the variable under test", j);

    std::printf("[m2h] NOWINLEFT verdict: without Window.left/top the chain dies inside "
                "MainWindow.tjs: fatal=\"%s\" @ %s:%lld (A with left/top reaches deep scripts)\n",
                fatal.c_str(), block.c_str(), line);
    GalBoxTJS2Free(j);
    return 0;
}

// ---- 场景 NOPAUSED：音频缓冲区缺 paused 位的负向对照（M2e-29）
// 段级删除两处（M2E29A 在 MIDISoundBuffer、M2E29B 在 WaveSoundBuffer），文件保留。
// 必须复现 `Member "paused" does not exist @ BGM.tjs:585`。
// 两处都删是因为 buf1 可能是任一类型；只删一处会“看运气”通过。
static int RunCaseNoPaused(const char *nopause, const char *storage) {
    if (!Exists(std::string(nopause) + "/system/Config.tjs"))
        return Fail("NOPAUSED fixture missing Config.tjs", NULL);
    const std::string snd = std::string(nopause) + "/system/GalBoxSound.tjs";
    if (!Exists(snd))
        return Fail("NOPAUSED fixture must keep GalBoxSound.tjs", NULL);
    {
        std::ifstream f(snd.c_str());
        std::string body((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        if (body.find("M2E29A_BEGIN") != std::string::npos ||
            body.find("M2E29B_BEGIN") != std::string::npos)
            return Fail("NOPAUSED fixture must have both paused members removed", NULL);
        if (body.find("var paused") != std::string::npos)
            return Fail("NOPAUSED fixture still declares paused", NULL);
        // 两个音频类本身必须还在（否则测的是 M2e-25/26 的缺口）。
        for (const char *keep : {"class MIDISoundBuffer", "class WaveSoundBuffer",
                                 "var owner"}) {
            if (body.find(keep) == std::string::npos) {
                std::printf("[m2h] NOPAUSED fixture lost '%s'\n", keep);
                return Fail("NOPAUSED fixture lost unrelated Sound members", NULL);
            }
        }
    }

    galbox::XP3Archive arc;
    char *j = RunChain(nopause, storage, arc);
    if (!j) return Fail("cannot run the NOPAUSED control", NULL);
    DumpChain("NOPAUSED", j);

    if (StrField(j, "shim").find("M2E25_SOUND_SKELETON") == std::string::npos)
        return Fail("NOPAUSED control lost the Sound skeleton tag", j);

    const std::string fatal = StrField(j, "fatal");
    const std::string inner = StrField(j, "lastScriptError");
    const std::string block = StrField(j, "fatalBlock");
    const long long line = IntField(j, "fatalLine");
    // M2e-32 修正（run 44479583807 实测）：evalStorage 补上后链能跑到底，
    // 旧的「深度差异」判据失效——缺 paused 时 fatal 仍**精确**停在
    // `Member "paused" does not exist` @ BGM.tjs:585（实测），只是链尾跑得更深。
    // 旧注释说「读缺失成员返回 void、不会在 585 报错」是过时观察：
    // `dic.paused = buf1.paused` 是对**类实例**读属性，缺了会真抛错并写进 fatal。
    // ⇒ 改回直接验证变量唯一：fatal 必须精确复现 paused@BGM.tjs:585。
    const bool reproduced =
        (fatal.find("\"paused\"") != std::string::npos ||
         inner.find("\"paused\"") != std::string::npos) &&
        block.find("BGM.tjs") != std::string::npos && line == 585;
    if (!reproduced)
        return Fail("NOPAUSED control did not reproduce paused@BGM.tjs:585 — "
                    "paused would not be the variable under test", j);

    std::printf("[m2h] NOPAUSED verdict: without paused the chain stops at \"%s\" @ %s:%lld "
                "(exact paused@BGM.tjs:585 reproduction)\n", fatal.c_str(), block.c_str(), line);
    GalBoxTJS2Free(j);
    return 0;
}

// ---- 场景 NOPAN：音频缓冲区缺 pan 的负向对照（M2e-30）
// 段级删除（WaveSoundBuffer 里 M2E30 段），文件/两个类本体保留。
// 口径：按 M2e-28/29 的实测教训（读缺失成员返回 void、分支静默跳过），
// **不写死**期望行号，而用“到不了后续脚本”的判据：
//   有 pan → 链能过 SE.tjs 继续；无 pan → 死在 SE.tjs 或更浅的 MainWindow.tjs。
static int RunCaseNoPan(const char *nopan, const char *storage) {
    if (!Exists(std::string(nopan) + "/system/Config.tjs"))
        return Fail("NOPAN fixture missing Config.tjs", NULL);
    const std::string snd = std::string(nopan) + "/system/GalBoxSound.tjs";
    if (!Exists(snd))
        return Fail("NOPAN fixture must keep GalBoxSound.tjs", NULL);
    {
        std::ifstream f(snd.c_str());
        std::string body((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        if (body.find("M2E30_BEGIN") != std::string::npos)
            return Fail("NOPAN fixture must have pan removed", NULL);
        if (body.find("property pan") != std::string::npos ||
            body.find("var panValue") != std::string::npos)
            return Fail("NOPAN fixture still declares pan/panValue", NULL);
        for (const char *keep : {"class MIDISoundBuffer", "class WaveSoundBuffer",
                                 "var paused = false;", "var owner"}) {
            if (body.find(keep) == std::string::npos) {
                std::printf("[m2h] NOPAN fixture lost '%s'\n", keep);
                return Fail("NOPAN fixture lost unrelated Sound members", NULL);
            }
        }
    }

    galbox::XP3Archive arc;
    char *j = RunChain(nopan, storage, arc);
    if (!j) return Fail("cannot run the NOPAN control", NULL);
    DumpChain("NOPAN", j);

    if (StrField(j, "shim").find("M2E25_SOUND_SKELETON") == std::string::npos)
        return Fail("NOPAN control lost the Sound skeleton tag", j);

    const std::string fatal = StrField(j, "fatal");
    const std::string block = StrField(j, "fatalBlock");
    const long long line = IntField(j, "fatalLine");
    if (Has(j, "\"ok\":true"))
        return Fail("NOPAN control unexpectedly completed the startup chain", j);
    const bool shallow = block.find("SE.tjs") != std::string::npos ||
                         block.find("MainWindow.tjs") != std::string::npos;
    if (!shallow)
        return Fail("NOPAN control advanced past SE.tjs — pan would not be the variable under test", j);

    std::printf("[m2h] NOPAN verdict: without pan the chain stops at \"%s\" @ %s:%lld "
                "(A with pan advances past SE.tjs)\n", fatal.c_str(), block.c_str(), line);
    GalBoxTJS2Free(j);
    return 0;
}

// ---- 场景 NOMODE：VideoOverlay 缺播放模式 mode 的负向对照（M2e-31）
// 段级删除（GalBoxMovie.tjs 的 M2E31 段）。判据同前三个：不写死行号，
// 只看「没推进到后续官方脚本」（Movie.tjs 或更浅的 MainWindow.tjs）。
static int RunCaseNoMode(const char *nomode, const char *storage) {
    if (!Exists(std::string(nomode) + "/system/Config.tjs"))
        return Fail("NOMODE fixture missing Config.tjs", NULL);
    const std::string mvf = std::string(nomode) + "/system/GalBoxMovie.tjs";
    if (!Exists(mvf))
        return Fail("NOMODE fixture must keep GalBoxMovie.tjs", NULL);
    {
        std::ifstream f(mvf.c_str());
        std::string body((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        if (body.find("M2E31_BEGIN") != std::string::npos)
            return Fail("NOMODE fixture must have mode removed", NULL);
        if (body.find("var mode = vomLayer;") != std::string::npos)
            return Fail("NOMODE fixture still declares mode", NULL);
        if (body.find("class VideoOverlay") == std::string::npos)
            return Fail("NOMODE fixture lost class VideoOverlay", NULL);
    }

    galbox::XP3Archive arc;
    char *j = RunChain(nomode, storage, arc);
    if (!j) return Fail("cannot run the NOMODE control", NULL);
    DumpChain("NOMODE", j);

    if (StrField(j, "shim").find("M2E27_MOVIE_SKELETON") == std::string::npos)
        return Fail("NOMODE control lost the Movie skeleton tag", j);

    const std::string fatal = StrField(j, "fatal");
    const std::string block = StrField(j, "fatalBlock");
    const long long line = IntField(j, "fatalLine");
    if (Has(j, "\"ok\":true"))
        return Fail("NOMODE control unexpectedly completed the startup chain", j);
    const bool shallow = block.find("Movie.tjs") != std::string::npos ||
                         block.find("MainWindow.tjs") != std::string::npos;
    if (!shallow)
        return Fail("NOMODE control advanced past Movie.tjs — mode would not be the variable under test", j);

    std::printf("[m2h] NOMODE verdict: without mode the chain stops at \"%s\" @ %s:%lld "
                "(A with mode advances past Movie.tjs)\n", fatal.c_str(), block.c_str(), line);
    GalBoxTJS2Free(j);
    return 0;
}

// ---- 场景 NOVISIBLE：VideoOverlay 缺可见标志 visible 的负向对照（M2e-33）
// 段级删除（GalBoxMovie.tjs 的 M2E33 段）。判据：链没跑到后续脚本，
// fatal 必须精确复现 `Member "visible" does not exist` @ Movie.tjs:33 ——
// M2e-32 已证实 fatal 记的是第一次捕获的异常，链跑通后仍精确保留。
static int RunCaseNoVisible(const char *novisible, const char *storage) {
    if (!Exists(std::string(novisible) + "/system/Config.tjs"))
        return Fail("NOVISIBLE fixture missing Config.tjs", NULL);
    const std::string mvf = std::string(novisible) + "/system/GalBoxMovie.tjs";
    if (!Exists(mvf))
        return Fail("NOVISIBLE fixture must keep GalBoxMovie.tjs", NULL);
    {
        std::ifstream f(mvf.c_str());
        std::string body((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        if (body.find("M2E33_BEGIN") != std::string::npos)
            return Fail("NOVISIBLE fixture must have visible removed", NULL);
        if (body.find("var visible = true;") != std::string::npos)
            return Fail("NOVISIBLE fixture still declares visible", NULL);
        if (body.find("class VideoOverlay") == std::string::npos)
            return Fail("NOVISIBLE fixture lost class VideoOverlay", NULL);
        // 只有 visible 缺失，mode 必须还在 —— 保证变量唯一。
        if (body.find("var mode = vomLayer;") == std::string::npos)
            return Fail("NOVISIBLE fixture lost mode too (variable not unique)", NULL);
    }

    galbox::XP3Archive arc;
    char *j = RunChain(novisible, storage, arc);
    if (!j) return Fail("cannot run the NOVISIBLE control", NULL);
    DumpChain("NOVISIBLE", j);

    if (StrField(j, "shim").find("M2E27_MOVIE_SKELETON") == std::string::npos)
        return Fail("NOVISIBLE control lost the Movie skeleton tag", j);

    const std::string fatal = StrField(j, "fatal");
    const std::string block = StrField(j, "fatalBlock");
    const long long line = IntField(j, "fatalLine");
    if (Has(j, "\"ok\":true"))
        return Fail("NOVISIBLE control unexpectedly completed the startup chain", j);
    // 精确复现 visible@Movie.tjs:33（fatal 记首次捕获的异常，链跑通也不丢）。
    if (fatal.find("\"visible\"") == std::string::npos)
        return Fail("NOVISIBLE control did not reproduce the visible gap — "
                    "visible would not be the variable under test", j);
    if (block.find("Movie.tjs") == std::string::npos)
        return Fail("NOVISIBLE control's visible gap is not in Movie.tjs", j);

    std::printf("[m2h] NOVISIBLE verdict: without visible the chain stops at \"%s\" @ %s:%lld "
                "(A with visible advances past Movie.tjs:33)\n", fatal.c_str(), block.c_str(), line);
    GalBoxTJS2Free(j);
    return 0;
}

// ---- 场景 NOLOOP：VideoOverlay 缺循环播放标志 loop 的负向对照（M2e-34）
// 段级删除（GalBoxMovie.tjs 的 M2E34 段）。判据：fatal 必须精确复现
// `Member "loop" does not exist` @ Movie.tjs:34（fatal 记首次捕获异常，链跑通也不丢）。
static int RunCaseNoLoop(const char *noloop, const char *storage) {
    if (!Exists(std::string(noloop) + "/system/Config.tjs"))
        return Fail("NOLOOP fixture missing Config.tjs", NULL);
    const std::string mvf = std::string(noloop) + "/system/GalBoxMovie.tjs";
    if (!Exists(mvf))
        return Fail("NOLOOP fixture must keep GalBoxMovie.tjs", NULL);
    {
        std::ifstream f(mvf.c_str());
        std::string body((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        if (body.find("M2E34_BEGIN") != std::string::npos)
            return Fail("NOLOOP fixture must have loop removed", NULL);
        if (body.find("var loop = false;") != std::string::npos)
            return Fail("NOLOOP fixture still declares loop", NULL);
        if (body.find("class VideoOverlay") == std::string::npos)
            return Fail("NOLOOP fixture lost class VideoOverlay", NULL);
        // 只有 loop 缺失，visible 必须还在 —— 保证变量唯一。
        if (body.find("var visible = true;") == std::string::npos)
            return Fail("NOLOOP fixture lost visible too (variable not unique)", NULL);
    }

    galbox::XP3Archive arc;
    char *j = RunChain(noloop, storage, arc);
    if (!j) return Fail("cannot run the NOLOOP control", NULL);
    DumpChain("NOLOOP", j);

    if (StrField(j, "shim").find("M2E27_MOVIE_SKELETON") == std::string::npos)
        return Fail("NOLOOP control lost the Movie skeleton tag", j);

    const std::string fatal = StrField(j, "fatal");
    const std::string block = StrField(j, "fatalBlock");
    const long long line = IntField(j, "fatalLine");
    if (Has(j, "\"ok\":true"))
        return Fail("NOLOOP control unexpectedly completed the startup chain", j);
    // 精确复现 loop@Movie.tjs:34。注意 "loop" 是子串敏感的：别的成员名
    // （segmentLoopStartFrame 等）带引号后不会命中 "\"loop\""。
    if (fatal.find("\"loop\"") == std::string::npos)
        return Fail("NOLOOP control did not reproduce the loop gap — "
                    "loop would not be the variable under test", j);
    if (block.find("Movie.tjs") == std::string::npos)
        return Fail("NOLOOP control's loop gap is not in Movie.tjs", j);

    std::printf("[m2h] NOLOOP verdict: without loop the chain stops at \"%s\" @ %s:%lld "
                "(A with loop advances past Movie.tjs:34)\n", fatal.c_str(), block.c_str(), line);
    GalBoxTJS2Free(j);
    return 0;
}

// ---- 场景 NOFRAME：VideoOverlay 缺当前帧号 frame 的负向对照（M2e-35）
// 段级删除（GalBoxMovie.tjs 的 M2E35 段）。判据：fatal 必须精确复现
// `Member "frame" does not exist` @ Movie.tjs:35（fatal 记首次捕获异常，链跑通也不丢）。
static int RunCaseNoFrame(const char *noframe, const char *storage) {
    if (!Exists(std::string(noframe) + "/system/Config.tjs"))
        return Fail("NOFRAME fixture missing Config.tjs", NULL);
    const std::string mvf = std::string(noframe) + "/system/GalBoxMovie.tjs";
    if (!Exists(mvf))
        return Fail("NOFRAME fixture must keep GalBoxMovie.tjs", NULL);
    {
        std::ifstream f(mvf.c_str());
        std::string body((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        if (body.find("M2E35_BEGIN") != std::string::npos)
            return Fail("NOFRAME fixture must have frame removed", NULL);
        if (body.find("var frame = 0;") != std::string::npos)
            return Fail("NOFRAME fixture still declares frame", NULL);
        if (body.find("class VideoOverlay") == std::string::npos)
            return Fail("NOFRAME fixture lost class VideoOverlay", NULL);
        // 只有 frame 缺失，loop 必须还在 —— 保证变量唯一。
        if (body.find("var loop = false;") == std::string::npos)
            return Fail("NOFRAME fixture lost loop too (variable not unique)", NULL);
    }

    galbox::XP3Archive arc;
    char *j = RunChain(noframe, storage, arc);
    if (!j) return Fail("cannot run the NOFRAME control", NULL);
    DumpChain("NOFRAME", j);

    if (StrField(j, "shim").find("M2E27_MOVIE_SKELETON") == std::string::npos)
        return Fail("NOFRAME control lost the Movie skeleton tag", j);

    const std::string fatal = StrField(j, "fatal");
    const std::string block = StrField(j, "fatalBlock");
    const long long line = IntField(j, "fatalLine");
    if (Has(j, "\"ok\":true"))
        return Fail("NOFRAME control unexpectedly completed the startup chain", j);
    // 精确复现 frame@Movie.tjs:35。带引号的 "frame" 不会误匹配
    // segmentLoopStartFrame 等含 frame 子串的成员名。
    if (fatal.find("\"frame\"") == std::string::npos)
        return Fail("NOFRAME control did not reproduce the frame gap — "
                    "frame would not be the variable under test", j);
    if (block.find("Movie.tjs") == std::string::npos)
        return Fail("NOFRAME control's frame gap is not in Movie.tjs", j);

    std::printf("[m2h] NOFRAME verdict: without frame the chain stops at \"%s\" @ %s:%lld "
                "(A with frame advances past Movie.tjs:35)\n", fatal.c_str(), block.c_str(), line);
    GalBoxTJS2Free(j);
    return 0;
}

// ---- 场景 NOAUDIOSTREAM：VideoOverlay 缺音频流数量 numberOfAudioStream 的负向对照（M2e-36）
// 段级删除（GalBoxMovie.tjs 的 M2E36 段）。判据：fatal 必须精确复现
// `Member "numberOfAudioStream" does not exist` @ Movie.tjs:37。
// 注意这是 if() 条件里的裸标识符读（前几个成员都在赋值右侧）——是否同样抛
// Member 错由 CI 实测确认；若实测走 void 静默跳过，再按实测口径调整判据。
static int RunCaseNoAudioStream(const char *noaudio, const char *storage) {
    if (!Exists(std::string(noaudio) + "/system/Config.tjs"))
        return Fail("NOAUDIOSTREAM fixture missing Config.tjs", NULL);
    const std::string mvf = std::string(noaudio) + "/system/GalBoxMovie.tjs";
    if (!Exists(mvf))
        return Fail("NOAUDIOSTREAM fixture must keep GalBoxMovie.tjs", NULL);
    {
        std::ifstream f(mvf.c_str());
        std::string body((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        if (body.find("M2E36_BEGIN") != std::string::npos)
            return Fail("NOAUDIOSTREAM fixture must have numberOfAudioStream removed", NULL);
        if (body.find("var numberOfAudioStream = 0;") != std::string::npos)
            return Fail("NOAUDIOSTREAM fixture still declares numberOfAudioStream", NULL);
        if (body.find("class VideoOverlay") == std::string::npos)
            return Fail("NOAUDIOSTREAM fixture lost class VideoOverlay", NULL);
        // 只有 numberOfAudioStream 缺失，frame 必须还在 —— 保证变量唯一。
        if (body.find("var frame = 0;") == std::string::npos)
            return Fail("NOAUDIOSTREAM fixture lost frame too (variable not unique)", NULL);
    }

    galbox::XP3Archive arc;
    char *j = RunChain(noaudio, storage, arc);
    if (!j) return Fail("cannot run the NOAUDIOSTREAM control", NULL);
    DumpChain("NOAUDIOSTREAM", j);

    if (StrField(j, "shim").find("M2E27_MOVIE_SKELETON") == std::string::npos)
        return Fail("NOAUDIOSTREAM control lost the Movie skeleton tag", j);

    const std::string fatal = StrField(j, "fatal");
    const std::string block = StrField(j, "fatalBlock");
    const long long line = IntField(j, "fatalLine");
    if (Has(j, "\"ok\":true"))
        return Fail("NOAUDIOSTREAM control unexpectedly completed the startup chain", j);
    // 精确复现 numberOfAudioStream@Movie.tjs:37。
    if (fatal.find("numberOfAudioStream") == std::string::npos)
        return Fail("NOAUDIOSTREAM control did not reproduce the numberOfAudioStream gap — "
                    "numberOfAudioStream would not be the variable under test", j);
    if (block.find("Movie.tjs") == std::string::npos)
        return Fail("NOAUDIOSTREAM control's gap is not in Movie.tjs", j);

    std::printf("[m2h] NOAUDIOSTREAM verdict: without numberOfAudioStream the chain stops at \"%s\" @ %s:%lld "
                "(A with numberOfAudioStream advances past Movie.tjs:37)\n",
                fatal.c_str(), block.c_str(), line);
    GalBoxTJS2Free(j);
    return 0;
}

// ---- 场景 NOSEGSTART：VideoOverlay 缺分段循环起始帧 segmentLoopStartFrame 的负向对照（M2e-37）
// 段级删除（GalBoxMovie.tjs 的 M2E37 段）。判据：fatal 必须精确复现
// `Member "segmentLoopStartFrame" does not exist` @ Movie.tjs:44。
static int RunCaseNoSegStart(const char *noseg, const char *storage) {
    if (!Exists(std::string(noseg) + "/system/Config.tjs"))
        return Fail("NOSEGSTART fixture missing Config.tjs", NULL);
    const std::string mvf = std::string(noseg) + "/system/GalBoxMovie.tjs";
    if (!Exists(mvf))
        return Fail("NOSEGSTART fixture must keep GalBoxMovie.tjs", NULL);
    {
        std::ifstream f(mvf.c_str());
        std::string body((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        if (body.find("M2E37_BEGIN") != std::string::npos)
            return Fail("NOSEGSTART fixture must have segmentLoopStartFrame removed", NULL);
        if (body.find("var segmentLoopStartFrame = 0;") != std::string::npos)
            return Fail("NOSEGSTART fixture still declares segmentLoopStartFrame", NULL);
        if (body.find("class VideoOverlay") == std::string::npos)
            return Fail("NOSEGSTART fixture lost class VideoOverlay", NULL);
        // 只有 segmentLoopStartFrame 缺失，numberOfAudioStream 必须还在 —— 变量唯一。
        if (body.find("var numberOfAudioStream = 0;") == std::string::npos)
            return Fail("NOSEGSTART fixture lost numberOfAudioStream too (variable not unique)", NULL);
    }

    galbox::XP3Archive arc;
    char *j = RunChain(noseg, storage, arc);
    if (!j) return Fail("cannot run the NOSEGSTART control", NULL);
    DumpChain("NOSEGSTART", j);

    if (StrField(j, "shim").find("M2E27_MOVIE_SKELETON") == std::string::npos)
        return Fail("NOSEGSTART control lost the Movie skeleton tag", j);

    const std::string fatal = StrField(j, "fatal");
    const std::string block = StrField(j, "fatalBlock");
    const long long line = IntField(j, "fatalLine");
    if (Has(j, "\"ok\":true"))
        return Fail("NOSEGSTART control unexpectedly completed the startup chain", j);
    if (fatal.find("segmentLoopStartFrame") == std::string::npos)
        return Fail("NOSEGSTART control did not reproduce the segmentLoopStartFrame gap — "
                    "segmentLoopStartFrame would not be the variable under test", j);
    if (block.find("Movie.tjs") == std::string::npos)
        return Fail("NOSEGSTART control's gap is not in Movie.tjs", j);

    std::printf("[m2h] NOSEGSTART verdict: without segmentLoopStartFrame the chain stops at \"%s\" @ %s:%lld "
                "(A with segmentLoopStartFrame advances past Movie.tjs:44)\n",
                fatal.c_str(), block.c_str(), line);
    GalBoxTJS2Free(j);
    return 0;
}

// ---- 场景 NOSEGEND：VideoOverlay 缺分段循环结束帧 segmentLoopEndFrame 的负向对照（M2e-38）
// 段级删除（GalBoxMovie.tjs 的 M2E38 段）。判据：fatal 必须精确复现
// `Member "segmentLoopEndFrame" does not exist` @ Movie.tjs:45。
static int RunCaseNoSegEnd(const char *nosegend, const char *storage) {
    if (!Exists(std::string(nosegend) + "/system/Config.tjs"))
        return Fail("NOSEGEND fixture missing Config.tjs", NULL);
    const std::string mvf = std::string(nosegend) + "/system/GalBoxMovie.tjs";
    if (!Exists(mvf))
        return Fail("NOSEGEND fixture must keep GalBoxMovie.tjs", NULL);
    {
        std::ifstream f(mvf.c_str());
        std::string body((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        if (body.find("M2E38_BEGIN") != std::string::npos)
            return Fail("NOSEGEND fixture must have segmentLoopEndFrame removed", NULL);
        if (body.find("var segmentLoopEndFrame = 0;") != std::string::npos)
            return Fail("NOSEGEND fixture still declares segmentLoopEndFrame", NULL);
        if (body.find("class VideoOverlay") == std::string::npos)
            return Fail("NOSEGEND fixture lost class VideoOverlay", NULL);
        // 只有 segmentLoopEndFrame 缺失，segmentLoopStartFrame 必须还在 —— 变量唯一。
        if (body.find("var segmentLoopStartFrame = 0;") == std::string::npos)
            return Fail("NOSEGEND fixture lost segmentLoopStartFrame too (variable not unique)", NULL);
    }

    galbox::XP3Archive arc;
    char *j = RunChain(nosegend, storage, arc);
    if (!j) return Fail("cannot run the NOSEGEND control", NULL);
    DumpChain("NOSEGEND", j);

    if (StrField(j, "shim").find("M2E27_MOVIE_SKELETON") == std::string::npos)
        return Fail("NOSEGEND control lost the Movie skeleton tag", j);

    const std::string fatal = StrField(j, "fatal");
    const std::string block = StrField(j, "fatalBlock");
    const long long line = IntField(j, "fatalLine");
    if (Has(j, "\"ok\":true"))
        return Fail("NOSEGEND control unexpectedly completed the startup chain", j);
    if (fatal.find("segmentLoopEndFrame") == std::string::npos)
        return Fail("NOSEGEND control did not reproduce the segmentLoopEndFrame gap — "
                    "segmentLoopEndFrame would not be the variable under test", j);
    if (block.find("Movie.tjs") == std::string::npos)
        return Fail("NOSEGEND control's gap is not in Movie.tjs", j);

    std::printf("[m2h] NOSEGEND verdict: without segmentLoopEndFrame the chain stops at \"%s\" @ %s:%lld "
                "(A with segmentLoopEndFrame advances past Movie.tjs:45)\n",
                fatal.c_str(), block.c_str(), line);
    GalBoxTJS2Free(j);
    return 0;
}

// ---- 场景 NOPERIOD：VideoOverlay 缺周期事件帧 periodEventFrame 的负向对照（M2e-39）
// 段级删除（GalBoxMovie.tjs 的 M2E39 段）。判据：fatal 必须精确复现
// `Member "periodEventFrame" does not exist` @ Movie.tjs:46。
static int RunCaseNoPeriod(const char *noperiod, const char *storage) {
    if (!Exists(std::string(noperiod) + "/system/Config.tjs"))
        return Fail("NOPERIOD fixture missing Config.tjs", NULL);
    const std::string mvf = std::string(noperiod) + "/system/GalBoxMovie.tjs";
    if (!Exists(mvf))
        return Fail("NOPERIOD fixture must keep GalBoxMovie.tjs", NULL);
    {
        std::ifstream f(mvf.c_str());
        std::string body((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        if (body.find("M2E39_BEGIN") != std::string::npos)
            return Fail("NOPERIOD fixture must have periodEventFrame removed", NULL);
        if (body.find("var periodEventFrame = 0;") != std::string::npos)
            return Fail("NOPERIOD fixture still declares periodEventFrame", NULL);
        if (body.find("class VideoOverlay") == std::string::npos)
            return Fail("NOPERIOD fixture lost class VideoOverlay", NULL);
        // 只有 periodEventFrame 缺失，segmentLoopEndFrame 必须还在 —— 变量唯一。
        if (body.find("var segmentLoopEndFrame = 0;") == std::string::npos)
            return Fail("NOPERIOD fixture lost segmentLoopEndFrame too (variable not unique)", NULL);
    }

    galbox::XP3Archive arc;
    char *j = RunChain(noperiod, storage, arc);
    if (!j) return Fail("cannot run the NOPERIOD control", NULL);
    DumpChain("NOPERIOD", j);

    if (StrField(j, "shim").find("M2E27_MOVIE_SKELETON") == std::string::npos)
        return Fail("NOPERIOD control lost the Movie skeleton tag", j);

    const std::string fatal = StrField(j, "fatal");
    const std::string block = StrField(j, "fatalBlock");
    const long long line = IntField(j, "fatalLine");
    if (Has(j, "\"ok\":true"))
        return Fail("NOPERIOD control unexpectedly completed the startup chain", j);
    if (fatal.find("periodEventFrame") == std::string::npos)
        return Fail("NOPERIOD control did not reproduce the periodEventFrame gap — "
                    "periodEventFrame would not be the variable under test", j);
    if (block.find("Movie.tjs") == std::string::npos)
        return Fail("NOPERIOD control's gap is not in Movie.tjs", j);

    std::printf("[m2h] NOPERIOD verdict: without periodEventFrame the chain stops at \"%s\" @ %s:%lld "
                "(A with periodEventFrame advances past Movie.tjs:46)\n",
                fatal.c_str(), block.c_str(), line);
    GalBoxTJS2Free(j);
    return 0;
}

// ---- 场景 NORESINT：KAGParser 骨架缺 resetInterrupt() 的负向对照（M2e-40）
// 骨架文件保留（M2E11 tag 仍应出现），只把 M2E40_BEGIN/END 之间的
// `function resetInterrupt() {}` 整段删掉。判据：fatal 必须精确复现
// `Member "resetInterrupt" does not exist` @ Conductor.tjs:213。
static int RunCaseNoResInt(const char *noresint, const char *storage) {
    if (!Exists(std::string(noresint) + "/system/Config.tjs"))
        return Fail("NORESINT fixture missing Config.tjs", NULL);
    const std::string skel = std::string(noresint) + "/system/GalBoxKAGParser.tjs";
    if (!Exists(skel))
        return Fail("NORESINT fixture must keep the KAGParser skeleton", NULL);
    {
        std::ifstream f(skel.c_str());
        std::string body((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        if (body.find("M2E40_BEGIN") != std::string::npos)
            return Fail("NORESINT fixture must have resetInterrupt removed", NULL);
        if (body.find("function resetInterrupt()") != std::string::npos)
            return Fail("NORESINT fixture still declares resetInterrupt", NULL);
        if (body.find("class KAGParser") == std::string::npos)
            return Fail("NORESINT fixture lost the KAGParser class itself", NULL);
        // 只有 resetInterrupt 缺失，其他 KAGParser 方法必须还在 —— 变量唯一。
        if (body.find("function goToLabel()") == std::string::npos)
            return Fail("NORESINT fixture lost goToLabel too (variable not unique)", NULL);
    }

    galbox::XP3Archive arc;
    char *j = RunChain(noresint, storage, arc);
    if (!j) return Fail("cannot run the NORESINT control", NULL);
    DumpChain("NORESINT", j);

    if (StrField(j, "shim").find("M2E11_KAGPARSER_SKELETON") == std::string::npos)
        return Fail("NORESINT control lost the KAGParser skeleton tag", j);

    const std::string fatal = StrField(j, "fatal");
    const std::string block = StrField(j, "fatalBlock");
    const long long line = IntField(j, "fatalLine");
    if (Has(j, "\"ok\":true"))
        return Fail("NORESINT control unexpectedly completed the startup chain", j);
    if (fatal.find("resetInterrupt") == std::string::npos)
        return Fail("NORESINT control did not reproduce the resetInterrupt gap — "
                    "resetInterrupt would not be the variable under test", j);
    if (block.find("Conductor.tjs") == std::string::npos || line != 213)
        return Fail("NORESINT control's gap is not the resetInterrupt call @ Conductor.tjs:213", j);

    std::printf("[m2h] NORESINT verdict: without resetInterrupt the chain stops at \"%s\" @ %s:%lld "
                "(A with resetInterrupt advances past Conductor.tjs:213)\n",
                fatal.c_str(), block.c_str(), line);
    GalBoxTJS2Free(j);
    return 0;
}

// ---- 场景 NOCURSTOR：KAGParser 骨架缺 curStorage 成员的负向对照（M2e-41）
// 骨架文件保留（M2E11 tag 仍应出现），只把 M2E41_BEGIN/END 之间的
// `var curStorage = "";` 整段删掉。判据：fatal 必须精确复现
// `Member "curStorage" does not exist` @ Conductor.tjs:180。
static int RunCaseNoCurStorage(const char *nocurstor, const char *storage) {
    if (!Exists(std::string(nocurstor) + "/system/Config.tjs"))
        return Fail("NOCURSTOR fixture missing Config.tjs", NULL);
    const std::string skel = std::string(nocurstor) + "/system/GalBoxKAGParser.tjs";
    if (!Exists(skel))
        return Fail("NOCURSTOR fixture must keep the KAGParser skeleton", NULL);
    {
        std::ifstream f(skel.c_str());
        std::string body((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        if (body.find("M2E41_BEGIN") != std::string::npos)
            return Fail("NOCURSTOR fixture must have curStorage removed", NULL);
        // 查声明形态而非裸字样：M2E41 段内的注释也提到 curStorage，
        // 但整段会被 drop_marked 一并删掉；将来头部注释若提到它也不该假红。
        if (body.find("var curStorage") != std::string::npos)
            return Fail("NOCURSTOR fixture still declares curStorage", NULL);
        // 🔴 getNextTag 必须**同时**缺失：curStorage 只在官方 Conductor.tjs:180
        // 的 catch 块里被读，而 catch 块只有在 try 块抛异常时才进入（唯一异常源
        // 是 81 行的 getNextTag()）。M2e-43 补上 getNextTag 后链条走
        // 「シナリオ終了」分支就不进 catch 块 ⇒ 只删 curStorage 反而链条畅通
        // ⇒ 对照失效。额外删掉它，强制回到历史路径；被测变量仍是 curStorage。
        if (body.find("function getNextTag") != std::string::npos)
            return Fail("NOCURSTOR fixture must also drop getNextTag "
                        "(otherwise the chain never enters the catch block and "
                        "curStorage is never read — the control would be vacuous)", NULL);
        if (body.find("class KAGParser") == std::string::npos)
            return Fail("NOCURSTOR fixture lost the KAGParser class itself", NULL);
        // 只有 curStorage 缺失，其他 KAGParser 成员/方法必须还在 —— 变量唯一。
        if (body.find("function goToLabel()") == std::string::npos)
            return Fail("NOCURSTOR fixture lost goToLabel too (variable not unique)", NULL);
        if (body.find("function resetInterrupt()") == std::string::npos)
            return Fail("NOCURSTOR fixture lost resetInterrupt too (variable not unique)", NULL);
    }

    galbox::XP3Archive arc;
    char *j = RunChain(nocurstor, storage, arc);
    if (!j) return Fail("cannot run the NOCURSTOR control", NULL);
    DumpChain("NOCURSTOR", j);

    if (StrField(j, "shim").find("M2E11_KAGPARSER_SKELETON") == std::string::npos)
        return Fail("NOCURSTOR control lost the KAGParser skeleton tag", j);

    const std::string fatal = StrField(j, "fatal");
    const std::string block = StrField(j, "fatalBlock");
    const long long line = IntField(j, "fatalLine");
    if (Has(j, "\"ok\":true"))
        return Fail("NOCURSTOR control unexpectedly completed the startup chain", j);
    if (fatal.find("curStorage") == std::string::npos)
        return Fail("NOCURSTOR control did not reproduce the curStorage gap — "
                    "curStorage would not be the variable under test", j);
    if (block.find("Conductor.tjs") == std::string::npos || line != 180)
        return Fail("NOCURSTOR control's gap is not the curStorage read @ Conductor.tjs:180", j);

    std::printf("[m2h] NOCURSTOR verdict: without curStorage the chain stops at \"%s\" @ %s:%lld "
                "(A with curStorage advances past Conductor.tjs:180)\n",
                fatal.c_str(), block.c_str(), line);
    GalBoxTJS2Free(j);
    return 0;
}

// ---- 场景 NOCURLINE：KAGParser 骨架缺 curLine 成员的负向对照（M2e-42）
// 骨架文件保留（M2E11 tag 仍应出现），只把 M2E42_BEGIN/END 之间的
// `var curLine = 0;` 整段删掉。判据：fatal 必须精确复现
// `Member "curLine" does not exist` @ Conductor.tjs:180。
// ⚠️ curStorage 与 curLine 在官方脚本里是**同一行**（180）的两个裸标识符读，
// TJS2 拼接从左到右求值 ⇒ 本对照必须确认 fatal 里出现的是 curLine
// 而不是 curStorage（否则说明删错了段、变量不唯一）。
static int RunCaseNoCurLine(const char *nocurline, const char *storage) {
    if (!Exists(std::string(nocurline) + "/system/Config.tjs"))
        return Fail("NOCURLINE fixture missing Config.tjs", NULL);
    const std::string skel = std::string(nocurline) + "/system/GalBoxKAGParser.tjs";
    if (!Exists(skel))
        return Fail("NOCURLINE fixture must keep the KAGParser skeleton", NULL);
    {
        std::ifstream f(skel.c_str());
        std::string body((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        if (body.find("M2E42_BEGIN") != std::string::npos)
            return Fail("NOCURLINE fixture must have curLine removed", NULL);
        // 查声明形态而非裸字样：M2E42 段内的注释也提到 curLine，
        // 但整段会被 drop_marked 一并删掉。
        if (body.find("var curLine") != std::string::npos)
            return Fail("NOCURLINE fixture still declares curLine", NULL);
        // 🔴 getNextTag 必须**同时**缺失：curLine 与 curStorage 都只在官方
        // Conductor.tjs:180 的 catch 块里被读，而 catch 块只有在 try 块抛异常时
        // 才进入（唯一异常源是 81 行的 getNextTag()）。M2e-43 补上 getNextTag
        // 后链条走「シナリオ終了」分支就不进 catch 块 ⇒ 对照失效。
        if (body.find("function getNextTag") != std::string::npos)
            return Fail("NOCURLINE fixture must also drop getNextTag "
                        "(otherwise the chain never enters the catch block and "
                        "curLine is never read — the control would be vacuous)", NULL);
        if (body.find("class KAGParser") == std::string::npos)
            return Fail("NOCURLINE fixture lost the KAGParser class itself", NULL);
        // 只有 curLine 缺失，其他 KAGParser 成员/方法必须还在 —— 变量唯一。
        if (body.find("function goToLabel()") == std::string::npos)
            return Fail("NOCURLINE fixture lost goToLabel too (variable not unique)", NULL);
        if (body.find("function resetInterrupt()") == std::string::npos)
            return Fail("NOCURLINE fixture lost resetInterrupt too (variable not unique)", NULL);
        if (body.find("var curStorage") == std::string::npos)
            return Fail("NOCURLINE fixture lost curStorage too (variable not unique)", NULL);
    }

    galbox::XP3Archive arc;
    char *j = RunChain(nocurline, storage, arc);
    if (!j) return Fail("cannot run the NOCURLINE control", NULL);
    DumpChain("NOCURLINE", j);

    if (StrField(j, "shim").find("M2E11_KAGPARSER_SKELETON") == std::string::npos)
        return Fail("NOCURLINE control lost the KAGParser skeleton tag", j);

    const std::string fatal = StrField(j, "fatal");
    const std::string block = StrField(j, "fatalBlock");
    const long long line = IntField(j, "fatalLine");
    if (Has(j, "\"ok\":true"))
        return Fail("NOCURLINE control unexpectedly completed the startup chain", j);
    if (fatal.find("curLine") == std::string::npos)
        return Fail("NOCURLINE control did not reproduce the curLine gap — "
                    "curLine would not be the variable under test", j);
    // curStorage 与 curLine 同行：fatal 里若出现 curStorage，说明先撞上的是
    // 另一个变量 —— 段删错了，或 curStorage 也没补上。
    if (fatal.find("curStorage") != std::string::npos)
        return Fail("NOCURLINE control hit curStorage instead of curLine "
                    "(same line 180, evaluation order is left to right)", j);
    if (block.find("Conductor.tjs") == std::string::npos || line != 180)
        return Fail("NOCURLINE control's gap is not the curLine read @ Conductor.tjs:180", j);

    std::printf("[m2h] NOCURLINE verdict: without curLine the chain stops at \"%s\" @ %s:%lld "
                "(A with curLine advances past Conductor.tjs:180)\n",
                fatal.c_str(), block.c_str(), line);
    GalBoxTJS2Free(j);
    return 0;
}

// ---- 场景 NOGETNEXT：KAGParser 骨架缺 getNextTag() 方法的负向对照（M2e-43）
// 骨架文件保留（M2E11 tag 仍应出现），只把 M2E43_BEGIN/END 之间的
// `function getNextTag() { return void; }` 整段删掉。判据：fatal 必须
// 复现 `Member "getNextTag" does not exist`。
//
// ⚠️ fatal 形态与前几个对照不同：getNextTag 在 try 块内（Conductor.tjs:81）
// 抛错 → 被 172 行 catch 捕获 → catch 里拼错误信息 → 186 行
// `throw new ConductorException(msg)` 重抛。于是 fatal 是**多行 script
// exception 文本**，fatalLine 报的是**重抛点 189**而不是调用点 81。
// ⇒ 判据按**变量名**命中（fatal 文本里含成员名），不绑行号；
//   也不绑 fatalBlock 的具体文件行号。
static int RunCaseNoGetNext(const char *nogetnext, const char *storage) {
    if (!Exists(std::string(nogetnext) + "/system/Config.tjs"))
        return Fail("NOGETNEXT fixture missing Config.tjs", NULL);
    const std::string skel = std::string(nogetnext) + "/system/GalBoxKAGParser.tjs";
    if (!Exists(skel))
        return Fail("NOGETNEXT fixture must keep the KAGParser skeleton", NULL);
    {
        std::ifstream f(skel.c_str());
        std::string body((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        if (body.find("M2E43_BEGIN") != std::string::npos)
            return Fail("NOGETNEXT fixture must have getNextTag removed", NULL);
        // 查声明形态而非裸字样：M2E43 段内的注释也提到 getNextTag，
        // 但整段会被 drop_marked 一并删掉。
        if (body.find("function getNextTag") != std::string::npos)
            return Fail("NOGETNEXT fixture still declares getNextTag", NULL);
        if (body.find("class KAGParser") == std::string::npos)
            return Fail("NOGETNEXT fixture lost the KAGParser class itself", NULL);
        // 只有 getNextTag 缺失，其他 KAGParser 成员/方法必须还在 —— 变量唯一。
        if (body.find("function goToLabel()") == std::string::npos)
            return Fail("NOGETNEXT fixture lost goToLabel too (variable not unique)", NULL);
        if (body.find("function resetInterrupt()") == std::string::npos)
            return Fail("NOGETNEXT fixture lost resetInterrupt too (variable not unique)", NULL);
        if (body.find("var curStorage") == std::string::npos)
            return Fail("NOGETNEXT fixture lost curStorage too (variable not unique)", NULL);
        if (body.find("var curLine") == std::string::npos)
            return Fail("NOGETNEXT fixture lost curLine too (variable not unique)", NULL);
    }

    galbox::XP3Archive arc;
    char *j = RunChain(nogetnext, storage, arc);
    if (!j) return Fail("cannot run the NOGETNEXT control", NULL);
    DumpChain("NOGETNEXT", j);

    if (StrField(j, "shim").find("M2E11_KAGPARSER_SKELETON") == std::string::npos)
        return Fail("NOGETNEXT control lost the KAGParser skeleton tag", j);

    const std::string fatal = StrField(j, "fatal");
    if (Has(j, "\"ok\":true"))
        return Fail("NOGETNEXT control unexpectedly completed the startup chain", j);
    if (fatal.find("getNextTag") == std::string::npos)
        return Fail("NOGETNEXT control did not reproduce the getNextTag gap — "
                    "getNextTag would not be the variable under test", j);

    std::printf("[m2h] NOGETNEXT verdict: without getNextTag the chain stops at \"%s\" "
                "(A with getNextTag advances past Conductor.tjs:81)\n",
                fatal.c_str());
    GalBoxTJS2Free(j);
    return 0;
}

// ---- 场景 NOTAG44：M2e-44 one-shot 分发分支缺失的负向对照 --------------
// 删掉 M2E44 两个段（getNextTag 体内的 one-shot 分支 + 状态槽位声明）后，
// getNextTag 退回 M2e-43 的恒 void 版本 ⇒ 链条仍干净跑通（所以 ok 仍 true），
// 但 onTag 分发链路从未执行 ⇒ lastTagName 保持官方初始值 '' ⇒ probe 取 0。
// A（有 M2E44）probe=444、本对照 probe=0，唯一变量是那一段分发分支 ——
// 这证明 A 判据里的 probe==444 不是恒真，而是真的由 M2E44 段驱动的。
// 夹具检查查声明形态（var gp44Left / 字典字面量），不查裸字样：
// M2E44 段内的注释也提到 gp44Left，drop_marked 删整段后字样一并消失。
static int RunCaseNoTag44(const char *notag44, const char *storage) {
    if (!Exists(std::string(notag44) + "/system/Config.tjs"))
        return Fail("NOTAG44 fixture missing Config.tjs", NULL);
    const std::string skel = std::string(notag44) + "/system/GalBoxKAGParser.tjs";
    if (!Exists(skel))
        return Fail("NOTAG44 fixture must keep the KAGParser skeleton", NULL);
    {
        std::ifstream f(skel.c_str());
        std::string body((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
        if (body.find("M2E44_BEGIN") != std::string::npos)
            return Fail("NOTAG44 fixture must have the M2E44 sections removed", NULL);
        if (body.find("var gp44Left") != std::string::npos)
            return Fail("NOTAG44 fixture still declares the one-shot slot", NULL);
        if (body.find("%[\"tagname\"") != std::string::npos)
            return Fail("NOTAG44 fixture still builds the tag object", NULL);
        // getNextTag 本体（M2e-43 的恒 void 版本）必须还在 —— 删的只是 one-shot。
        if (body.find("function getNextTag") == std::string::npos)
            return Fail("NOTAG44 fixture lost getNextTag itself", NULL);
        // 变量唯一：其他 KAGParser 成员/方法必须还在。
        if (body.find("var curStorage") == std::string::npos)
            return Fail("NOTAG44 fixture lost curStorage too (variable not unique)", NULL);
        if (body.find("var curLine") == std::string::npos)
            return Fail("NOTAG44 fixture lost curLine too (variable not unique)", NULL);
        if (body.find("function goToLabel()") == std::string::npos)
            return Fail("NOTAG44 fixture lost goToLabel too (variable not unique)", NULL);
        if (body.find("function resetInterrupt()") == std::string::npos)
            return Fail("NOTAG44 fixture lost resetInterrupt too (variable not unique)", NULL);
    }

    galbox::XP3Archive arc;
    char *j = RunChain(notag44, storage, arc, kTagPathProbe);
    if (!j) return Fail("cannot run the NOTAG44 control", NULL);
    DumpChain("NOTAG44", j);

    if (StrField(j, "shim").find("M2E11_KAGPARSER_SKELETON") == std::string::npos)
        return Fail("NOTAG44 control lost the KAGParser skeleton tag", j);

    if (!Has(j, "\"ok\":true"))
        return Fail("NOTAG44 control unexpectedly failed the startup chain", j);
    if (StrField(j, "fatal").size() != 0)
        return Fail("NOTAG44 control raised a script-level exception", j);
    if (IntField(j, "probeValue") != 0)
        return Fail("NOTAG44 control ran the tag dispatch path without the "
                    "M2E44 branch — the control is not a negative contrast", j);

    std::printf("[m2h] NOTAG44 verdict: without the one-shot branch the chain is still "
                "clean but lastTagName stays empty (probe=0); with M2E44 the A case "
                "sees probe=444\n");
    GalBoxTJS2Free(j);
    return 0;
}

// ---- 场景 NOEVALSTORAGE：Scripts.evalStorage 原生方法缺失的负向对照（M2e-32）
// 与前面所有 NOxxx 不同：evalStorage 是 C++ 原生方法，不在 TJS 骨架文件里，
// 没法用 drop_marked/rm 夹具复现。改用宿主开关在**注册时**不挂载该方法 ——
// 引擎调用 Scripts.evalStorage() 时真抛 `Member "evalStorage" does not exist`，
// 与 run 44326396559 的历史缺口一字不差。夹具与 A 完全相同，唯一变量是开关。
// 判据（不写死行号）：ok 必须 false，且停在 MainWindow.tjs 的浅层系统变量加载处
// （带旧缺口的正文），诊断里 evalStorageEnabled 必须为 false。
static int RunCaseNoEvalStorage(const char *game, const char *storage) {
    if (!Exists(std::string(game) + "/system/Config.tjs"))
        return Fail("NOEVALSTORAGE fixture missing Config.tjs", NULL);

    GalBoxTJS2SetEvalStorageEnabled(0);  // 唯一变量：注册时不挂 evalStorage

    galbox::XP3Archive arc;
    char *j = RunChain(game, storage, arc);
    GalBoxTJS2SetEvalStorageEnabled(1);  // 恢复（独立进程里也只是好习惯）

    if (!j) return Fail("cannot run the NOEVALSTORAGE control", NULL);
    DumpChain("NOEVALSTORAGE", j);

    if (!Has(j, "\"evalStorageEnabled\":false"))
        return Fail("NOEVALSTORAGE control: evalStorage switch did not take effect", j);
    if (Has(j, "\"ok\":true"))
        return Fail("NOEVALSTORAGE control unexpectedly completed the startup chain", j);
    const std::string fatal = StrField(j, "fatal");
    const std::string inner = StrField(j, "lastScriptError");
    const std::string block = StrField(j, "fatalBlock");
    const long long line = IntField(j, "fatalLine");
    const bool reproduced =
        fatal.find("evalStorage") != std::string::npos ||
        inner.find("evalStorage") != std::string::npos ||
        fatal.find("システム変数データを読み込めないか") != std::string::npos;
    if (!reproduced)
        return Fail("NOEVALSTORAGE control did not reproduce the evalStorage gap", j);
    if (block.find("MainWindow.tjs") == std::string::npos)
        return Fail("NOEVALSTORAGE control stopped outside MainWindow.tjs — "
                    "evalStorage would not be the variable under test", j);

    std::printf("[m2h] NOEVALSTORAGE verdict: without the native evalStorage method "
                "the chain stops at \"%s\" @ %s:%lld "
                "(A with evalStorage advances past MainWindow.tjs:1045)\n",
                fatal.c_str(), block.c_str(), line);
    GalBoxTJS2Free(j);
    return 0;
}

// 打开任意容器（目录 / .xp3）跑一次官方链，返回 JSON。
// dirMode / entryCount 必须在 close() 之前取（close 会清掉它们）。
static char *RunArchive(const char *path, const char *storage,
                        galbox::XP3Archive &arc, bool *opened,
                        bool *dirMode, size_t *entryCount,
                        const char *probe = NULL) {
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
    char *j = GalBoxTJS2ExecScript("startup.tjs", probe);
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
    char *j = RunArchive(xp3, st.c_str(), arc, &opened, &dirMode, &cntXp3,
                         kTagPathProbe);
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
        "M2E25_SOUND_SKELETON",
        "M2E27_MOVIE_SKELETON",
    };
    for (size_t i = 0; i < sizeof(kMustHave) / sizeof(kMustHave[0]); ++i) {
        if (shim.find(kMustHave[i]) == std::string::npos) {
            std::printf("[m2h] XP3 missing shim tag: %s\n", kMustHave[i]);
            return Fail("packed xp3 did not load a skeleton that the fixture has", j);
        }
    }

    // 停点必须与 A 场景同口径（目录侧的等价结论由 A 在独立进程里断言）：
    // M2e-24 之后设备容器里也不许再出现 debugLevel / hitType / hitThreshold /
    // setImageSize / setSizeToImageSize / face / fillRect / cursor / name /
    // focusable / setSize / imageModified / colorRect / showParentHint /
    // cursorX+cursorY。判据非恒真 —— 十五个对应负向对照（NODEBUGLEVEL /
    // NOHITTYPE / NOHITTHRESHOLD / NOIMAGESIZE / NOSIZETOIMAGESIZE /
    // NODRAWFACE / NOFILLRECT / NOCURSOR / NONAME / NOFOCUSABLE / NOSIZE /
    // NOIMAGEMODIFIED / NOCOLORRECT / NOSHOWPARENTHINT / NOCURSORX）在
    // 独立进程里分别证明了「删掉成员 → 缺口复现」。
    const std::string fatal = StrField(j, "fatal");
    const std::string inner = StrField(j, "lastScriptError");
    for (const char *closed : {"debugLevel", "\"hitType\"", "\"hitThreshold\"",
                               "\"setImageSize\"", "\"setSizeToImageSize\"",
                               "\"face\"", "\"fillRect\"", "\"cursor\"",
                               "\"name\"", "\"focusable\"", "\"setSize\"",
                               "\"imageModified\"", "\"colorRect\"",
                               "\"showParentHint\"", "\"cursorX\"",
                               "\"MIDISoundBuffer\"", "\"WaveSoundBuffer\"",
                               "\"VideoOverlay\"", "\"left\"", "\"paused\"",
                               "\"pan\"", "\"mode\"", "evalStorage", "\"visible\"",
                               "\"loop\"", "\"frame\"", "numberOfAudioStream",
                               "segmentLoopStartFrame", "segmentLoopEndFrame",
                               "periodEventFrame", "resetInterrupt", "curStorage", "curLine",
                               "getNextTag", "gp44Left"}) {
        if (fatal.find(closed) != std::string::npos ||
            inner.find(closed) != std::string::npos) {
            std::printf("[m2h] XP3 still stuck at closed gap: %s\n", closed);
            return Fail("packed xp3 is still stuck at a gap this milestone closed", j);
        }
    }

    // M2e-44：包内链路与 A 场景同口径 —— onTag 分发必须真跑过（probe 444）。
    if (IntField(j, "probeValue") != 444)
        return Fail("packed xp3 did not run the tag dispatch path "
                    "(mainConductor.lastTagName != \"clickskip\")", j);

    std::printf("[m2h] XP3 verdict: real .xp3 ran the same chain "
                "(%lld scripts, debugLevel/hitType/hitThreshold/"
                "setImageSize/setSizeToImageSize/face/fillRect/cursor/name/"
                "focusable/setSize/imageModified/colorRect/showParentHint/"
                "cursorX/MIDISoundBuffer/WaveSoundBuffer/VideoOverlay/left/paused/pan/mode/"
                "evalStorage/visible/loop/frame/numberOfAudioStream/segmentLoopStartFrame/"
                "segmentLoopEndFrame/periodEventFrame/resetInterrupt/curStorage/curLine/getNextTag/gp44Left all cleared in the device container)\n", scripts);
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
    const char *noname   = (argc > 20) ? argv[20] : "";
    const char *nofocus  = (argc > 21) ? argv[21] : "";
    const char *nosize   = (argc > 22) ? argv[22] : "";
    const char *noim     = (argc > 23) ? argv[23] : "";
    const char *nocr     = (argc > 24) ? argv[24] : "";
    const char *noph     = (argc > 25) ? argv[25] : "";
    const char *nocx     = (argc > 26) ? argv[26] : "";
    const char *nomidi   = (argc > 27) ? argv[27] : "";
    const char *nowave   = (argc > 28) ? argv[28] : "";
    const char *novideo  = (argc > 29) ? argv[29] : "";
    const char *nowinl   = (argc > 30) ? argv[30] : "";
    const char *nopause  = (argc > 31) ? argv[31] : "";
    const char *nopan    = (argc > 32) ? argv[32] : "";
    const char *nomode   = (argc > 33) ? argv[33] : "";
    const char *novisible = (argc > 34) ? argv[34] : "";
    const char *noloop    = (argc > 35) ? argv[35] : "";
    const char *noframe   = (argc > 36) ? argv[36] : "";
    const char *noaudio   = (argc > 37) ? argv[37] : "";
    const char *noseg     = (argc > 38) ? argv[38] : "";
    const char *nosegend  = (argc > 39) ? argv[39] : "";
    const char *noperiod  = (argc > 40) ? argv[40] : "";
    const char *noresint  = (argc > 41) ? argv[41] : "";
    const char *nocurstor = (argc > 42) ? argv[42] : "";
    const char *nocurline = (argc > 43) ? argv[43] : "";
    const char *nogetnext = (argc > 44) ? argv[44] : "";
    const char *notag44   = (argc > 45) ? argv[45] : "";

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
    if (std::strcmp(which, "NONAME") == 0) {
        if (RunCaseNoName(noname, storage) != 0) return 3;
        std::printf("M2H_RESULT=OK\n");
        return 0;
    }
    if (std::strcmp(which, "NOFOCUSABLE") == 0) {
        if (RunCaseNoFocusable(nofocus, storage) != 0) return 3;
        std::printf("M2H_RESULT=OK\n");
        return 0;
    }
    if (std::strcmp(which, "NOSIZE") == 0) {
        if (RunCaseNoSize(nosize, storage) != 0) return 3;
        std::printf("M2H_RESULT=OK\n");
        return 0;
    }
    if (std::strcmp(which, "NOIMAGEMODIFIED") == 0) {
        if (RunCaseNoImageModified(noim, storage) != 0) return 3;
        std::printf("M2H_RESULT=OK\n");
        return 0;
    }
    if (std::strcmp(which, "NOCOLORRECT") == 0) {
        if (RunCaseNoColorRect(nocr, storage) != 0) return 3;
        std::printf("M2H_RESULT=OK\n");
        return 0;
    }
    if (std::strcmp(which, "NOSHOWPARENTHINT") == 0) {
        if (RunCaseNoShowParentHint(noph, storage) != 0) return 3;
        std::printf("M2H_RESULT=OK\n");
        return 0;
    }
    if (std::strcmp(which, "NOCURSORX") == 0) {
        if (RunCaseNoCursorX(nocx, storage) != 0) return 3;
        std::printf("M2H_RESULT=OK\n");
        return 0;
    }
    if (std::strcmp(which, "NOMIDI") == 0) {
        if (RunCaseNoMidi(nomidi, storage) != 0) return 3;
        std::printf("M2H_RESULT=OK\n");
        return 0;
    }
    if (std::strcmp(which, "NOWAVE") == 0) {
        if (RunCaseNoWave(nowave, storage) != 0) return 3;
        std::printf("M2H_RESULT=OK\n");
        return 0;
    }
    if (std::strcmp(which, "NOVIDEO") == 0) {
        if (RunCaseNoVideo(novideo, storage) != 0) return 3;
        std::printf("M2H_RESULT=OK\n");
        return 0;
    }
    if (std::strcmp(which, "NOWINLEFT") == 0) {
        if (RunCaseNoWinLeft(nowinl, storage) != 0) return 3;
        std::printf("M2H_RESULT=OK\n");
        return 0;
    }
    if (std::strcmp(which, "NOPAUSED") == 0) {
        if (RunCaseNoPaused(nopause, storage) != 0) return 3;
        std::printf("M2H_RESULT=OK\n");
        return 0;
    }
    if (std::strcmp(which, "NOPAN") == 0) {
        if (RunCaseNoPan(nopan, storage) != 0) return 3;
        std::printf("M2H_RESULT=OK\n");
        return 0;
    }
    if (std::strcmp(which, "NOMODE") == 0) {
        if (RunCaseNoMode(nomode, storage) != 0) return 3;
        std::printf("M2H_RESULT=OK\n");
        return 0;
    }
    if (std::strcmp(which, "NOEVALSTORAGE") == 0) {
        // 夹具与 A 完全相同；变量是宿主开关，不是文件，故复用 game 路径。
        if (RunCaseNoEvalStorage(game, storage) != 0) return 3;
        std::printf("M2H_RESULT=OK\n");
        return 0;
    }
    if (std::strcmp(which, "NOVISIBLE") == 0) {
        if (RunCaseNoVisible(novisible, storage) != 0) return 3;
        std::printf("M2H_RESULT=OK\n");
        return 0;
    }
    if (std::strcmp(which, "NOLOOP") == 0) {
        if (RunCaseNoLoop(noloop, storage) != 0) return 3;
        std::printf("M2H_RESULT=OK\n");
        return 0;
    }
    if (std::strcmp(which, "NOFRAME") == 0) {
        if (RunCaseNoFrame(noframe, storage) != 0) return 3;
        std::printf("M2H_RESULT=OK\n");
        return 0;
    }
    if (std::strcmp(which, "NOAUDIOSTREAM") == 0) {
        if (RunCaseNoAudioStream(noaudio, storage) != 0) return 3;
        std::printf("M2H_RESULT=OK\n");
        return 0;
    }
    if (std::strcmp(which, "NOSEGSTART") == 0) {
        if (RunCaseNoSegStart(noseg, storage) != 0) return 3;
        std::printf("M2H_RESULT=OK\n");
        return 0;
    }
    if (std::strcmp(which, "NOSEGEND") == 0) {
        if (RunCaseNoSegEnd(nosegend, storage) != 0) return 3;
        std::printf("M2H_RESULT=OK\n");
        return 0;
    }
    if (std::strcmp(which, "NOPERIOD") == 0) {
        if (RunCaseNoPeriod(noperiod, storage) != 0) return 3;
        std::printf("M2H_RESULT=OK\n");
        return 0;
    }
    if (std::strcmp(which, "NORESINT") == 0) {
        if (RunCaseNoResInt(noresint, storage) != 0) return 3;
        std::printf("M2H_RESULT=OK\n");
        return 0;
    }
    if (std::strcmp(which, "NOCURSTOR") == 0) {
        if (RunCaseNoCurStorage(nocurstor, storage) != 0) return 3;
        std::printf("M2H_RESULT=OK\n");
        return 0;
    }
    if (std::strcmp(which, "NOCURLINE") == 0) {
        if (RunCaseNoCurLine(nocurline, storage) != 0) return 3;
        std::printf("M2H_RESULT=OK\n");
        return 0;
    }
    if (std::strcmp(which, "NOGETNEXT") == 0) {
        if (RunCaseNoGetNext(nogetnext, storage) != 0) return 3;
        std::printf("M2H_RESULT=OK\n");
        return 0;
    }
    if (std::strcmp(which, "NOTAG44") == 0) {
        if (RunCaseNoTag44(notag44, storage) != 0) return 3;
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
                "NOFILLRECT / NOCURSOR / NONAME / NOFOCUSABLE / NOSIZE / "
                "NOIMAGEMODIFIED / NOCOLORRECT / NOSHOWPARENTHINT / "
                "NOCURSORX / NOMIDI / NOWAVE / NOVIDEO / NOWINLEFT / NOPAUSED / "
                "NOPAN / NOMODE / NOEVALSTORAGE / NOVISIBLE / NOLOOP / NOFRAME / "
                "NOAUDIOSTREAM / NOSEGSTART / NOSEGEND / NOPERIOD / NORESINT / NOCURSTOR / NOCURLINE / NOGETNEXT / NOTAG44 / XP3 / LEAK / LEAK2)\n", which);
    return 2;
}
