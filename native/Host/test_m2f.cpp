// test_m2f.cpp — Milestone 2e-2 自测：GalBox 的 Layer 骨架能否被官方脚本继承
//
// run #34 把根因钉死在 `KAGLayer.tjs:87` 的 `Member "Layer" does not exist`。
// 本轮用一份**纯 TJS2 的 Layer 骨架**（native/KAG/src/system/GalBoxLayer.tjs，
// GalBox 自写、非官方）先顶上，让官方脚本类真正能实例化，把下一批缺口逼出来。
//
// 关键机制（TJS2 的 super 是**惰性**的）：`class KAGLayer extends Layer` 语句
// 本身不去查 `Layer`，引擎把它编译成 super proxy，只有实例化或访问父类成员
// 时才求值。所以「脚本被成功读完」≠「继承成立」，必须真的 new 一次。
//
// 三个场景（都真跑 TJS2）：
//   A) 骨架在场：官方 KAGLayer.tjs（Shift-JIS 原始字节）被读入，
//      `new KAGLayer(null,null)` 走通 -> global.m2fProbe = 1。
//      probeValue 必须是 1；-1 说明实例化抛了异常（global 未赋值）。
//   B) 完整官方启动链：startup.tjs -> Initialize.tjs 原文 + 全量 system 脚本。
//      断言：仍 ok=false（不假装跑通）、错误里**不再**有 Layer 缺失、
//      scriptTrace 不比 M2e-1 倒退（>=10 个脚本）。
//   C) **负向对照**：同一套夹具但**不放骨架** —— 必须复现 run #34 的
//      `Member "Layer" does not exist`。没有这一条，A 场景就是恒真式自证：
//      万一探针根本没执行，A 也可能"看起来"通过。
//
// 全部通过打印 M2F_RESULT=OK。
#include "HostBridge.h"
#include "XP3Archive.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

static int Fail(const char *why, const char *json) {
    std::printf("[m2f] FAIL: %s\n[m2f] json=%s\n", why, json ? json : "(null)");
    std::printf("M2F_RESULT=ERR\n");
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
        std::printf("[m2f] open folder failed: %s (%s)\n", dir, err.c_str());
        return 0;
    }
    if (!arc.isDirectoryMode()) {
        std::printf("[m2f] %s is not a directory-mode archive\n", dir);
        return 0;
    }
    std::printf("[m2f] opened %s: entries=%zu dirMode=1\n", dir, arc.count());
    return 1;
}

int main(int argc, char **argv) {
    std::setbuf(stdout, NULL); // 崩溃诊断: 不丢已打印进度
    const char *game    = (argc > 1) ? argv[1] : "native/TJS2/build/m2f_game";
    const char *noShim  = (argc > 2) ? argv[2] : "native/TJS2/build/m2f_noshim";
    const char *storage = (argc > 3) ? argv[3] : ".";

    if (!Exists(std::string(game) + "/m2f_layer_probe.tjs"))
        return Fail("M2f fixture A missing", "m2f_layer_probe.tjs");
    if (!Exists(std::string(game) + "/system/GalBoxLayer.tjs"))
        return Fail("M2f fixture A missing the Layer skeleton", "GalBoxLayer.tjs");
    if (!Exists(std::string(game) + "/system/KAGLayer.tjs"))
        return Fail("M2f fixture A missing the official KAGLayer.tjs", "KAGLayer.tjs");
    if (!Exists(std::string(noShim) + "/system/KAGLayer.tjs"))
        return Fail("M2f negative-control fixture missing", "m2f_noshim/KAGLayer.tjs");
    if (Exists(std::string(noShim) + "/system/GalBoxLayer.tjs"))
        return Fail("negative control unexpectedly contains the Layer skeleton", NULL);

    // ---- 场景 A：官方 KAGLayer 成功继承 GalBox 的 Layer 骨架 ---------------
    {
        galbox::XP3Archive arc;
        if (!OpenDir(game, arc)) return Fail("cannot open fixture A", NULL);
        if (!GalBoxTJS2Install(&arc, storage, ""))
            return Fail("Install(A) returned 0", NULL);

        char *j = GalBoxTJS2ExecScript(
            "m2f_layer_probe.tjs",
            "(typeof m2fProbe == \"undefined\") ? -1 : m2fProbe");
        std::printf("[m2f] A (inherit the Layer skeleton) -> %s\n", j ? j : "(null)");
        if (!j) return Fail("ExecScript(A) returned NULL", NULL);

        if (!Has(j, "\"layerSkeleton\":\"M2E2_LAYER_SKELETON\""))
            return Fail("M2e-2 build tag missing", j);
        if (StrField(j, "shim").find("M2E2_LAYER_SKELETON") == std::string::npos)
            return Fail("the Layer skeleton was not loaded", j);
        if (!Has(j, "\"ok\":true"))
            return Fail("layer probe did not finish cleanly", j);

        const long long probe = IntField(j, "probeValue");
        if (probe != 1) {
            std::printf("[m2f] fatal=\"%s\" block=\"%s\" line=%lld\n",
                        StrField(j, "fatal").c_str(),
                        StrField(j, "fatalBlock").c_str(),
                        IntField(j, "fatalLine"));
            return Fail("new KAGLayer(null,null) did not succeed "
                        "(script class did not inherit the Layer skeleton)", j);
        }
        // 官方 KAGLayer.tjs 必须真的被读到（不是探针自己编的类）
        if (StrField(j, "scriptTrace").find("KAGLayer.tjs") == std::string::npos)
            return Fail("the official KAGLayer.tjs was never read", j);
        std::printf("[m2f] official KAGLayer extends the GalBox Layer skeleton: OK\n");

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
        std::printf("[m2f] B (full official startup chain) -> %s\n", j ? j : "(null)");
        if (!j) return Fail("ExecScript(B) returned NULL", NULL);

        const std::string trace = StrField(j, "scriptTrace");
        const std::string fatal = StrField(j, "fatal");
        const std::string inner = StrField(j, "lastScriptError");
        std::printf("[m2f]   scriptTrace     = \"%s\"\n", trace.c_str());
        std::printf("[m2f]   fatalBlock      = \"%s\" (line %lld)\n",
                    StrField(j, "fatalBlock").c_str(), IntField(j, "fatalLine"));
        std::printf("[m2f]   fatal           = \"%s\"\n", fatal.c_str());
        std::printf("[m2f]   lastScriptError = \"%s\"\n", inner.c_str());

        // 本轮仍不实现真正的渲染 / 事件循环，不许假装跑通。
        if (!Has(j, "\"ok\":false"))
            return Fail("official startup unexpectedly reported success", j);

        // 核心结论：Layer 缺口已经消失。
        if (fatal.find("\"Layer\"") != std::string::npos ||
            inner.find("\"Layer\"") != std::string::npos) {
            std::printf("[m2f] the Layer skeleton did not take effect\n");
            return Fail("still blocked by the missing Layer class", j);
        }
        // 加载序列不许比 M2e-1（10 个脚本）倒退。
        int readCount = 0;
        for (size_t i = 0; i < trace.size(); ++i)
            if (trace[i] == ',') ++readCount;
        const int scripts = readCount + (trace.empty() ? 0 : 1);
        if (scripts < 10) {
            std::printf("[m2f] scripts in trace = %d\n", scripts);
            return Fail("the official script chain got shorter than in M2e-1", j);
        }
        std::printf("[m2f] official scripts read: %d\n", scripts);

        // 回归：M2d 的正则与 M2e-1 的 vendoring 不能被本轮弄坏。
        if (!Has(j, "\"appLock\":true"))
            return Fail("M2d regression: createAppLock no longer reached", j);
        if (!Has(j, "\"kagSystem\":\"M2E_SYSTEM_SCRIPTS_VENDORED\""))
            return Fail("M2e regression: vendored system scripts missing", j);

        GalBoxTJS2Free(j);
        GalBoxTJS2Uninstall();
        arc.close();
    }

    // ---- 场景 C：负向对照（同一套夹具，但不放 Layer 骨架） -----------------
    {
        galbox::XP3Archive arc;
        if (!OpenDir(noShim, arc)) return Fail("cannot open negative control", NULL);
        if (!GalBoxTJS2Install(&arc, storage, ""))
            return Fail("Install(C) returned 0", NULL);

        char *j = GalBoxTJS2ExecScript(
            "m2f_layer_probe.tjs",
            "(typeof m2fProbe == \"undefined\") ? -1 : m2fProbe");
        std::printf("[m2f] C (negative control, no Layer skeleton) -> %s\n",
                    j ? j : "(null)");
        if (!j) return Fail("ExecScript(C) returned NULL", NULL);

        // 状态串里现在还有官方常量表的 tag（它不在负向对照夹具里被去掉），
        // 所以这里只查 **Layer 骨架** 的 tag 有没有出现。
        if (StrField(j, "shim").find("M2E2_LAYER_SKELETON") != std::string::npos)
            return Fail("negative control unexpectedly loaded the Layer skeleton", j);
        if (!Has(j, "\"ok\":false"))
            return Fail("negative control unexpectedly succeeded", j);
        if (IntField(j, "probeValue") != -1)
            return Fail("negative control unexpectedly instantiated KAGLayer", j);
        const std::string fatal = StrField(j, "fatal");
        const std::string block = StrField(j, "fatalBlock");
        if (fatal.find("\"Layer\"") == std::string::npos ||
            block.find("KAGLayer") == std::string::npos) {
            std::printf("[m2f] fatal=\"%s\" block=\"%s\"\n",
                        fatal.c_str(), block.c_str());
            return Fail("negative control did not reproduce the Layer blocker", j);
        }
        std::printf("[m2f] negative control OK: fatal=\"%s\" @ %s\n",
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

    std::printf("M2F_RESULT=OK\n");
    return 0;
}
