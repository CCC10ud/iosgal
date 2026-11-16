// test_m2g.cpp — Milestone 2e-3 自测：GalBox 的 Window 骨架能否被官方脚本继承
//
// run #36 把官方脚本加载数从 10 推到 15，`MainWindow.tjs` 进入 scriptTrace，
// 新阻塞点是 `Member "Window" does not exist` @ MainWindow.tjs:7
// （`class KAGWindow extends Window`）。本轮补一份纯 TJS2 的 `Window` 骨架，
// 顺带补上 `System.dataPath` 与 `Debug.logLocation`
// （KAGWindow 的类成员初始化与构造器各用一次，缺了就会挡住实例化）。
//
// TJS2 的 super 是**惰性**的：类语句本身不查父类，只有实例化时才查。
// 所以判据全部围绕「实例化时父类查找的结果」，不依赖「脚本被读完」。
//
// 三个场景（都真跑 TJS2）：
//   A) Window 骨架在场：实例化的结果**不得**是 `Member "Window" does not exist`
//      （成功更好：probeValue==1）。
//   B) 完整官方启动链：同样不得再出现 Window 缺失；加载脚本数不许比
//      M2e-2 的 15 个倒退。
//   C) **负向对照**：同一套夹具但**不放** Window 骨架 —— 必须复现 run #36 的
//      `Member "Window" does not exist` @ MainWindow.tjs。
//      没有这一条，A 场景就是恒真式自证。
//
// 全部通过打印 M2G_RESULT=OK。
#include "HostBridge.h"
#include "XP3Archive.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

static int Fail(const char *why, const char *json) {
    std::printf("[m2g] FAIL: %s\n[m2g] json=%s\n", why, json ? json : "(null)");
    std::printf("M2G_RESULT=ERR\n");
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
        std::printf("[m2g] open folder failed: %s (%s)\n", dir, err.c_str());
        return 0;
    }
    if (!arc.isDirectoryMode()) {
        std::printf("[m2g] %s is not a directory-mode archive\n", dir);
        return 0;
    }
    std::printf("[m2g] opened %s: entries=%zu dirMode=1\n", dir, arc.count());
    return 1;
}

static int CountTrace(const std::string &trace) {
    if (trace.empty()) return 0;
    int n = 1;
    for (size_t i = 0; i < trace.size(); ++i)
        if (trace[i] == ',') ++n;
    return n;
}

int main(int argc, char **argv) {
    const char *game   = (argc > 1) ? argv[1] : "native/TJS2/build/m2g_game";
    const char *noWin  = (argc > 2) ? argv[2] : "native/TJS2/build/m2g_nowindow";
    const char *storage = (argc > 3) ? argv[3] : ".";

    if (!Exists(std::string(game) + "/m2g_window_probe.tjs"))
        return Fail("M2g fixture A missing", "m2g_window_probe.tjs");
    if (!Exists(std::string(game) + "/system/GalBoxWindow.tjs"))
        return Fail("M2g fixture A missing the Window skeleton", "GalBoxWindow.tjs");
    if (!Exists(std::string(game) + "/system/MainWindow.tjs"))
        return Fail("M2g fixture A missing the official MainWindow.tjs", "MainWindow.tjs");
    if (!Exists(std::string(noWin) + "/m2g_window_probe.tjs"))
        return Fail("M2g negative-control fixture missing", "m2g_nowindow/probe");
    if (Exists(std::string(noWin) + "/system/GalBoxWindow.tjs"))
        return Fail("negative control unexpectedly contains the Window skeleton", NULL);
    if (!Exists(std::string(noWin) + "/system/GalBoxLayer.tjs"))
        return Fail("negative control must still contain the Layer skeleton", NULL);

    // ---- 场景 A：官方 KAGWindow 不再缺 Window 父类 ------------------------
    {
        galbox::XP3Archive arc;
        if (!OpenDir(game, arc)) return Fail("cannot open fixture A", NULL);
        if (!GalBoxTJS2Install(&arc, storage, ""))
            return Fail("Install(A) returned 0", NULL);

        char *j = GalBoxTJS2ExecScript(
            "m2g_window_probe.tjs",
            "(typeof m2gProbe == \"undefined\") ? "
            "((typeof m2gLoaded == \"undefined\") ? -2 : -1) : 1");
        std::printf("[m2g] A (inherit the Window skeleton) -> %s\n", j ? j : "(null)");
        if (!j) return Fail("ExecScript(A) returned NULL", NULL);

        if (!Has(j, "\"M2E3_WINDOW_SKELETON\""))
            return Fail("M2e-3 build tag missing", j);
        const std::string shim = StrField(j, "shim");
        if (shim.find("M2E3_WINDOW_SKELETON") == std::string::npos)
            return Fail("the Window skeleton was not loaded", j);

        const long long probe = IntField(j, "probeValue");
        if (probe == -2) {
            std::printf("[m2g] fatal=\"%s\" inner=\"%s\"\n",
                        StrField(j, "fatal").c_str(),
                        StrField(j, "lastScriptError").c_str());
            return Fail("the official MainWindow.tjs was never read", j);
        }
        // 核心结论：父类查找不再停在 `Window` 上。
        const std::string fatal = StrField(j, "fatal");
        const std::string inner = StrField(j, "lastScriptError");
        std::printf("[m2g]   fatal       = \"%s\"\n", fatal.c_str());
        std::printf("[m2g]   fatalBlock  = \"%s\" (line %lld)\n",
                    StrField(j, "fatalBlock").c_str(), IntField(j, "fatalLine"));
        std::printf("[m2g]   inner error = \"%s\"\n", inner.c_str());
        if (fatal.find("\"Window\"") != std::string::npos ||
            inner.find("\"Window\"") != std::string::npos) {
            return Fail("still blocked by the missing Window class", j);
        }
        if (probe == 1)
            std::printf("[m2g] new KAGWindow() succeeded outright\n");
        else
            std::printf("[m2g] new KAGWindow() hit the next gap (not Window)\n");

        // 官方 MainWindow.tjs 必须真的被读到（不是探针自己编的类）
        if (StrField(j, "scriptTrace").find("MainWindow.tjs") == std::string::npos)
            return Fail("the official MainWindow.tjs was never read", j);

        GalBoxTJS2Free(j);
        GalBoxTJS2Uninstall();
        arc.close();
    }

    // ---- 场景 B：完整官方启动链 -------------------------------------------
    {
        galbox::XP3Archive arc;
        if (!OpenDir(game, arc)) return Fail("cannot open fixture B", NULL);
        if (!GalBoxTJS2Install(&arc, storage, ""))
            return Fail("Install(B) returned 0", NULL);

        char *j = GalBoxTJS2ExecScript("startup.tjs", NULL);
        std::printf("[m2g] B (full official startup chain) -> %s\n", j ? j : "(null)");
        if (!j) return Fail("ExecScript(B) returned NULL", NULL);

        const std::string trace = StrField(j, "scriptTrace");
        const std::string fatal = StrField(j, "fatal");
        const std::string inner = StrField(j, "lastScriptError");
        std::printf("[m2g]   scriptTrace = \"%s\"\n", trace.c_str());
        std::printf("[m2g]   fatalBlock  = \"%s\" (line %lld)\n",
                    StrField(j, "fatalBlock").c_str(), IntField(j, "fatalLine"));
        std::printf("[m2g]   fatal       = \"%s\"\n", fatal.c_str());
        std::printf("[m2g]   inner error = \"%s\"\n", inner.c_str());

        if (!Has(j, "\"ok\":false"))
            return Fail("official startup unexpectedly reported success", j);
        if (fatal.find("\"Window\"") != std::string::npos ||
            inner.find("\"Window\"") != std::string::npos)
            return Fail("still blocked by the missing Window class", j);

        const int scripts = CountTrace(trace);
        if (scripts < 15) {
            std::printf("[m2g] scripts in trace = %d\n", scripts);
            return Fail("the official script chain got shorter than in M2e-2", j);
        }
        std::printf("[m2g] official scripts read: %d\n", scripts);

        // 回归
        if (!Has(j, "\"appLock\":true"))
            return Fail("M2d regression: createAppLock no longer reached", j);
        if (!Has(j, "\"M2E2_LAYER_SKELETON\""))
            return Fail("M2e-2 regression: Layer skeleton missing", j);

        GalBoxTJS2Free(j);
        GalBoxTJS2Uninstall();
        arc.close();
    }

    // ---- 场景 C：负向对照（Layer 骨架在，Window 骨架不在） -----------------
    {
        galbox::XP3Archive arc;
        if (!OpenDir(noWin, arc)) return Fail("cannot open negative control", NULL);
        if (!GalBoxTJS2Install(&arc, storage, ""))
            return Fail("Install(C) returned 0", NULL);

        char *j = GalBoxTJS2ExecScript(
            "m2g_window_probe.tjs",
            "(typeof m2gProbe == \"undefined\") ? "
            "((typeof m2gLoaded == \"undefined\") ? -2 : -1) : 1");
        std::printf("[m2g] C (negative control, no Window skeleton) -> %s\n",
                    j ? j : "(null)");
        if (!j) return Fail("ExecScript(C) returned NULL", NULL);

        if (StrField(j, "shim").find("M2E3_WINDOW_SKELETON") != std::string::npos)
            return Fail("negative control unexpectedly loaded the Window skeleton", j);
        if (IntField(j, "probeValue") != -1)
            return Fail("negative control unexpectedly instantiated KAGWindow", j);
        const std::string fatal = StrField(j, "fatal");
        const std::string block = StrField(j, "fatalBlock");
        if (fatal.find("\"Window\"") == std::string::npos ||
            block.find("MainWindow") == std::string::npos) {
            std::printf("[m2g] fatal=\"%s\" block=\"%s\"\n",
                        fatal.c_str(), block.c_str());
            return Fail("negative control did not reproduce the Window blocker", j);
        }
        std::printf("[m2g] negative control OK: fatal=\"%s\" @ %s\n",
                    fatal.c_str(), block.c_str());

        GalBoxTJS2Free(j);
        GalBoxTJS2Uninstall();
        arc.close();
    }

    // ---- 收尾：卸载后必须回到失败态 --------------------------------------
    {
        char *j = GalBoxTJS2ExecScript("startup.tjs", NULL);
        if (!Has(j, "\"ok\":false")) return Fail("Uninstall did not take effect", j);
        GalBoxTJS2Free(j);
    }

    std::printf("M2G_RESULT=OK\n");
    return 0;
}
