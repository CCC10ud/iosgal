// test_m2h.cpp — Milestone 2e-5 自测：夹具换成官方 Config.tjs，并查同进程状态泄漏
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
// 三个场景：
//   A) 官方 Config.tjs 的完整启动链（第一次跑）：
//      硬判据 —— **旧的 KAGWindow_config 缺口必须消失**，且加载脚本数不倒退
//      （>=15）。
//   A2) **紧接着再跑一次**同样的链 —— 只打印诊断，不参与判定。
//      run #40 出现过「同进程内第二次执行会破坏官方启动链」（autoPathCount=0、
//      trace 只 1 项、appLock=false，诊断自相矛盾），run #41 的隔离实验把
//      B 调到最前跑时一切正常 ⇒ 确认是状态泄漏，但根因未定。
//      A2 就是这次定位用的探针：如果 A2 的 autoPathCount 也是 0，泄漏必现。
//   B) 负向对照（同一套夹具，但 Config.tjs 换回探针版）：
//      必须复现 run #45 的 `Member "KAGWindow_config" does not exist` ——
//      没有这一条，A 场景就是恒真式自证。
//
// 全部通过打印 M2H_RESULT=OK。
#include "HostBridge.h"
#include "XP3Archive.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
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

int main(int argc, char **argv) {
    const char *game   = (argc > 1) ? argv[1] : "native/TJS2/build/m2h_game";
    const char *ctrl   = (argc > 2) ? argv[2] : "native/TJS2/build/m2h_ctrl";
    const char *storage = (argc > 3) ? argv[3] : ".";

    if (!Exists(std::string(game) + "/startup.tjs"))
        return Fail("M2h fixture missing", "startup.tjs");
    if (!Exists(std::string(game) + "/system/Config.tjs"))
        return Fail("M2h fixture missing Config.tjs", NULL);
    if (!Exists(std::string(ctrl) + "/system/Config.tjs"))
        return Fail("M2h negative-control fixture missing", NULL);

    // ---- 场景 A：官方 Config.tjs，完整启动链（第一次跑） ------------------
    {
        galbox::XP3Archive arc;
        if (!OpenDir(game, arc)) return Fail("cannot open fixture A", NULL);
        if (!GalBoxTJS2Install(&arc, storage, ""))
            return Fail("Install(A) returned 0", NULL);

        char *j = GalBoxTJS2ExecScript("startup.tjs", NULL);
        std::printf("[m2h] A (official Config.tjs) -> %s\n", j ? j : "(null)");
        if (!j) return Fail("ExecScript(A) returned NULL", NULL);
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

        // 硬判据 2：加载脚本数不许比 run #45 的 15 个倒退。
        const int scripts = CountTrace(StrField(j, "scriptTrace"));
        if (scripts < 15) {
            std::printf("[m2h] scripts in trace = %d\n", scripts);
            return Fail("the official script chain got shorter than in M2e-4", j);
        }

        // 硬判据 3：官方 Config.tjs 必须真被读到（不是探针版）。
        if (StrField(j, "scriptTrace").find("Config.tjs") == std::string::npos)
            return Fail("Config.tjs was never read", j);

        // 回归
        if (!Has(j, "\"appLock\":true"))
            return Fail("M2d regression: createAppLock no longer reached", j);
        if (!Has(j, "\"M2E4_OFFICIAL_CONSTS\""))
            return Fail("M2e-4 regression: official constants missing", j);

        GalBoxTJS2Free(j);
        GalBoxTJS2Uninstall();
        arc.close();
    }

    // ---- 场景 A2：紧接着再跑一次同样的链（诊断，不参与判定） ---------------
    // run #40 的现象：同进程内第二次执行会破坏官方启动链
    // （autoPathCount=0 / trace 只 1 项 / appLock=false）。
    {
        galbox::XP3Archive arc;
        if (!OpenDir(game, arc)) return Fail("cannot open fixture A2", NULL);
        if (!GalBoxTJS2Install(&arc, storage, ""))
            return Fail("Install(A2) returned 0", NULL);

        char *j = GalBoxTJS2ExecScript("startup.tjs", NULL);
        std::printf("[m2h] A2 (the same chain, second time in one process) -> %s\n",
                    j ? j : "(null)");
        if (j) {
            DumpChain("A2", j);
            const long long apc = IntField(j, "autoPathCount");
            const int scripts = CountTrace(StrField(j, "scriptTrace"));
            std::printf("[m2h] A2 verdict: %s\n",
                        (apc <= 0 || scripts < 15)
                            ? "LEAK REPRODUCED (second run is broken)"
                            : "no leak (second run is fine)");
            GalBoxTJS2Free(j);
        }
        GalBoxTJS2Uninstall();
        arc.close();
    }

    // ---- 场景 B：负向对照（Config.tjs 换回探针版） ------------------------
    {
        galbox::XP3Archive arc;
        if (!OpenDir(ctrl, arc)) return Fail("cannot open negative control", NULL);
        if (!GalBoxTJS2Install(&arc, storage, ""))
            return Fail("Install(B) returned 0", NULL);

        char *j = GalBoxTJS2ExecScript("startup.tjs", NULL);
        std::printf("[m2h] B (negative control, probe Config.tjs) -> %s\n",
                    j ? j : "(null)");
        if (!j) return Fail("ExecScript(B) returned NULL", NULL);
        DumpChain("B", j);

        const std::string fatal = StrField(j, "fatal");
        const std::string inner = StrField(j, "lastScriptError");
        if (fatal.find("KAGWindow_config") == std::string::npos &&
            inner.find("KAGWindow_config") == std::string::npos) {
            std::printf("[m2h] fatal=\"%s\" inner=\"%s\"\n",
                        fatal.c_str(), inner.c_str());
            return Fail("negative control did not reproduce the KAGWindow_config gap", j);
        }
        std::printf("[m2h] negative control OK: the gap really comes from Config.tjs\n");

        GalBoxTJS2Free(j);
        GalBoxTJS2Uninstall();
        arc.close();
    }

    std::printf("M2H_RESULT=OK\n");
    return 0;
}
