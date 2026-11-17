// test_m2e.cpp — Milestone 2e-1 自测：官方 KAG 系统脚本全量 vendoring
//
// run #32 把阻塞点钉死在
//   "Storages: script not found via autoPath: KAGLayer.tjs"
// 也就是「缺文件」。本轮把 kag3/template/system/ 的 28 个上游文件
// （20 个 .tjs + Config.~new + 7 个 png/asd）按 git blob 原始字节全量装进
// 游戏包，目的是把阻塞点**逼成缺原生类**：
//   KAGLayer.tjs 第 87 行 `class KAGLayer extends Layer` 里的 Layer 尚未实现。
//
// 本轮不实现 Layer。仍然期望 ok=false，不假装 KAG 跑通。
//
// 三个场景（都真跑 TJS2）：
//   A) 清单探针：按官方方式 addAutoPath("system/") 后用短名逐个查 20 个
//      .tjs，另加一个必然不存在的哨兵。global.m2eProbe = found*100 + bad，
//      期望恰好 2000 → 能区分「文件数不对」「存在性检查恒真」。
//      C++ 侧另外逐个校验 28 个文件的**精确字节数**（被转码会变长度），
//      构建脚本再用 sha256 逐文件对上游 blob 校验。
//   B) 官方 startup.tjs → Initialize.tjs 原文，全量系统脚本在位。
//      判据：scriptTrace 含 KAGLayer.tjs（文件真被读到并解码）、
//      scriptsRead>=4、shiftJisReads>=4、错误里不再有 "script not found"、
//      且报错块/正文指向 KAGLayer / Layer。
//   C) **负向对照**：故意只放旧的 4 个文件（M2d 夹具），断言 scriptTrace
//      不含 KAGLayer.tjs 且错误回到 "script not found"。
//      没有这一条，A/B 里的 scriptTrace 断言就是恒真式自证。
//
// 全部通过打印 M2E_RESULT=OK。
#include "HostBridge.h"
#include "XP3Archive.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

static int Fail(const char *why, const char *json) {
    std::printf("[m2e] FAIL: %s\n[m2e] json=%s\n", why, json ? json : "(null)");
    std::printf("M2E_RESULT=ERR\n");
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

static long FileSize(const std::string &path) {
    std::ifstream f(path.c_str(), std::ios::binary | std::ios::ate);
    if (!f.good()) return -1;
    return static_cast<long>(f.tellg());
}

// 上游 jeeb/kirikiri2 @ master, kag3/template/system/ 的全部 28 个 blob。
// 字节数来自 /git/trees/master?recursive=1；内容与 blob sha 由构建脚本
// 逐文件 sha256 校验。这里再校验一次长度，防「文件在但被转码/截断」。
struct ExpectedFile { const char *name; long bytes; };
static const ExpectedFile kUpstreamSystem[] = {
    {"AnimationLayer.tjs", 31464}, {"BGM.tjs", 15453},
    {"ButtonLayer.tjs", 7541},     {"CheckBoxLayer.tjs", 4856},
    {"Conductor.tjs", 11530},      {"DefaultMover.tjs", 7665},
    {"EditLayer.tjs", 16138},      {"GraphicLayer.tjs", 14982},
    {"HistoryLayer.tjs", 24415},   {"Initialize.tjs", 7246},
    {"KAGLayer.tjs", 13213},       {"MainWindow.tjs", 150598},
    {"Menus.tjs", 8074},           {"MessageLayer.tjs", 77527},
    {"Movie.tjs", 7072},           {"Plugin.tjs", 2475},
    {"SE.tjs", 6266},              {"UpdateConfig.tjs", 2973},
    {"Utils.tjs", 3658},           {"YesNoDialog.tjs", 4406},
    {"Config.~new", 25899},        {"LineBreak.asd", 616},
    {"LineBreak.png", 81},         {"LineBreak_a.png", 1972},
    {"PageBreak.asd", 616},        {"PageBreak.png", 81},
    {"PageBreak_a.png", 1768},     {"check.png", 144},
};
static const size_t kUpstreamCount =
    sizeof(kUpstreamSystem) / sizeof(kUpstreamSystem[0]);

static int VerifyVendoredBytes(const std::string &game) {
    long total = 0;
    for (size_t i = 0; i < kUpstreamCount; ++i) {
        const std::string path = game + "/system/" + kUpstreamSystem[i].name;
        const long n = FileSize(path);
        if (n != kUpstreamSystem[i].bytes) {
            std::printf("[m2e] vendored byte mismatch: %s got %ld want %ld\n",
                        path.c_str(), n, kUpstreamSystem[i].bytes);
            return 0;
        }
        total += n;
    }
    std::printf("[m2e] vendored system files=%zu total=%ld B (expect 448729 B)\n",
                kUpstreamCount, total);
    return total == 448729 ? 1 : 0;
}

static int OpenDir(const char *dir, galbox::XP3Archive &arc) {
    std::string err;
    if (!arc.open(dir, &err)) {
        std::printf("[m2e] open folder failed: %s (%s)\n", dir, err.c_str());
        return 0;
    }
    if (!arc.isDirectoryMode()) {
        std::printf("[m2e] %s is not a directory-mode archive\n", dir);
        return 0;
    }
    std::printf("[m2e] opened %s: entries=%zu dirMode=1\n", dir, arc.count());
    return 1;
}

int main(int argc, char **argv) {
    std::setbuf(stdout, NULL); // 崩溃诊断: 不丢已打印进度
    const char *game    = (argc > 1) ? argv[1] : "native/TJS2/build/m2e_game";
    const char *minGame = (argc > 2) ? argv[2] : "native/TJS2/build/m2e_min";
    const char *storage = (argc > 3) ? argv[3] : ".";

    if (!Exists(std::string(game) + "/m2e_manifest_probe.tjs"))
        return Fail("M2e fixture A missing", "m2e_manifest_probe.tjs");
    for (const char *f : {"startup.tjs", "system/Initialize.tjs",
                          "system/Utils.tjs", "system/Config.tjs"}) {
        if (!Exists(std::string(game) + "/" + f))
            return Fail("M2e fixture B missing", f);
    }
    if (!Exists(std::string(minGame) + "/startup.tjs"))
        return Fail("M2e negative-control fixture missing", "m2e_min/startup.tjs");
    if (!VerifyVendoredBytes(game))
        return Fail("vendored system files are not the exact upstream blobs", NULL);

    // ---- 0) 未安装宿主时必须明确失败 ------------------------------------
    {
        char *j = GalBoxTJS2ExecScript("startup.tjs", NULL);
        if (!Has(j, "\"ok\":false") || !Has(j, "host not installed"))
            return Fail("uninstalled exec did not fail loudly", j);
        GalBoxTJS2Free(j);
    }

    // ---- 场景 A：官方 20 个 .tjs 都在包里，且存在性检查不是恒真 ----------
    {
        galbox::XP3Archive arc;
        if (!OpenDir(game, arc)) return Fail("cannot open fixture A", NULL);
        if (!GalBoxTJS2Install(&arc, storage, ""))
            return Fail("Install(A) returned 0", NULL);

        char *j = GalBoxTJS2ExecScript(
            "m2e_manifest_probe.tjs",
            "(typeof m2eProbe == \"undefined\") ? -1 : m2eProbe");
        std::printf("[m2e] A (manifest probe) -> %s\n", j ? j : "(null)");
        if (!j) return Fail("ExecScript(A) returned NULL", NULL);

        if (!Has(j, "\"kagSystem\":\"M2E_SYSTEM_SCRIPTS_VENDORED\""))
            return Fail("M2e build tag missing", j);
        if (!Has(j, "\"ok\":true"))
            return Fail("manifest probe did not finish cleanly", j);
        const long long probe = IntField(j, "probeValue");
        if (probe != 2000) {
            std::printf("[m2e] m2eProbe=%lld (want 20 found * 100 + 0 false positives)\n",
                        probe);
            return Fail("not all 20 official .tjs are visible via autoPath, "
                        "or isExistentStorage is always-true", j);
        }
        // 探针没有调用 execStorage，所以读取轨迹必须为空。
        // 这条与场景 B 的「轨迹含 KAGLayer」配对，证明轨迹不是恒真计数器。
        if (IntField(j, "scriptsRead") != 0)
            return Fail("scriptTrace is not empty for a probe that loaded no script", j);
        if (StrField(j, "lastDebug").find("M2E_MISSING") != std::string::npos)
            return Fail("manifest probe reported a missing script", j);
        std::printf("[m2e] manifest: 20/20 official .tjs visible, "
                    "sentinel correctly absent\n");

        GalBoxTJS2Free(j);
        GalBoxTJS2Uninstall();
        arc.close();
    }

    // ---- 场景 B：官方启动链推过「缺文件」，撞上「缺原生类」 ---------------
    {
        galbox::XP3Archive arc;
        if (!OpenDir(game, arc)) return Fail("cannot open fixture B", NULL);
        if (!GalBoxTJS2Install(&arc, storage, ""))
            return Fail("Install(B) returned 0", NULL);

        char *j = GalBoxTJS2ExecScript("startup.tjs", NULL);
        std::printf("[m2e] B (full official system scripts) -> %s\n",
                    j ? j : "(null)");
        if (!j) return Fail("ExecScript(B) returned NULL", NULL);

        // 本轮不实现 Layer / Window，仍然不许假装跑通。
        if (!Has(j, "\"ok\":false"))
            return Fail("official startup unexpectedly reported success", j);

        const std::string trace  = StrField(j, "scriptTrace");
        const std::string inner  = StrField(j, "lastScriptError");
        const std::string err    = StrField(j, "error");
        const std::string fatal  = StrField(j, "fatal");
        const std::string exc    = StrField(j, "fatalException");
        const std::string block  = StrField(j, "fatalBlock");

        std::printf("[m2e]   scriptTrace     = \"%s\"\n", trace.c_str());
        std::printf("[m2e]   fatalBlock      = \"%s\" (line %lld)\n",
                    block.c_str(), IntField(j, "fatalLine"));
        std::printf("[m2e]   fatal           = \"%s\"\n", fatal.c_str());
        std::printf("[m2e]   fatalException  = \"%s\"\n", exc.c_str());
        std::printf("[m2e]   lastScriptError = \"%s\"\n", inner.c_str());
        std::printf("[m2e]   error           = \"%s\"\n", err.c_str());

        // 结论 1：KAGLayer.tjs 真实存在于包里、被读流打开并解码过。
        // M2d 时它连存在性检查都过不了，trace 里根本不会出现。
        if (trace.find("KAGLayer.tjs") == std::string::npos)
            return Fail("KAGLayer.tjs was never actually read from the package", j);
        if (IntField(j, "scriptsRead") < 4)
            return Fail("too few official scripts were read", j);
        if (IntField(j, "execStorageAttempts") < 4)
            return Fail("startup chain did not advance past M2d", j);
        // 结论 2：多个官方脚本走的是 Shift-JIS/CP932 解码链，不是 latin-1 兜底。
        if (IntField(j, "shiftJisReads") < 4)
            return Fail("Shift-JIS decode path did not run on the official scripts", j);

        // 结论 3：失败原因不再是「缺文件」。
        if (inner.find("script not found") != std::string::npos ||
            err.find("script not found") != std::string::npos ||
            fatal.find("script not found") != std::string::npos)
            return Fail("still blocked by a missing file (vendoring did not take)", j);

        // 结论 4：报错落在 KAGLayer.tjs 内部（缺 Layer 原生类），
        // 不是停在 Initialize.tjs 或 startup.tjs。两种消息格式都能接受：
        //   fatalBlock = system/KAGLayer.tjs，或正文里出现 Layer。
        const std::string all = fatal + " | " + exc + " | " + err;
        if (block.find("KAGLayer") == std::string::npos &&
            all.find("Layer") == std::string::npos) {
            std::printf("[m2e] blockerText=\"%s\"\n", all.c_str());
            return Fail("blocker is not inside KAGLayer.tjs / not the Layer class", j);
        }

        // 回归：M2d 的正则能力不能被这轮改动弄丢。
        if (!Has(j, "\"appLock\":true"))
            return Fail("M2d regression: createAppLock no longer reached", j);
        if (!Has(j, "\"regexp\":\"M2D_REGEXP_STDREGEX\""))
            return Fail("M2d build tag missing", j);

        GalBoxTJS2Free(j);
        GalBoxTJS2Uninstall();
        arc.close();
    }

    // ---- 场景 C：负向对照（只有旧的 4 个文件） ----------------------------
    {
        galbox::XP3Archive arc;
        if (!OpenDir(minGame, arc)) return Fail("cannot open negative control", NULL);
        if (!GalBoxTJS2Install(&arc, storage, ""))
            return Fail("Install(C) returned 0", NULL);

        char *j = GalBoxTJS2ExecScript("startup.tjs", NULL);
        std::printf("[m2e] C (negative control, no system scripts) -> %s\n",
                    j ? j : "(null)");
        if (!j) return Fail("ExecScript(C) returned NULL", NULL);

        const std::string trace = StrField(j, "scriptTrace");
        const std::string inner = StrField(j, "lastScriptError");
        const std::string err   = StrField(j, "error");
        if (trace.find("KAGLayer.tjs") != std::string::npos)
            return Fail("scriptTrace claims KAGLayer.tjs was read although "
                        "the file is absent", j);
        if (inner.find("script not found via autoPath: KAGLayer.tjs") ==
                std::string::npos &&
            err.find("script not found via autoPath: KAGLayer.tjs") ==
                std::string::npos)
            return Fail("negative control did not reproduce the M2d blocker", j);
        std::printf("[m2e] negative control OK: trace=\"%s\", blocker=\"%s\"\n",
                    trace.c_str(), inner.c_str());

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

    std::printf("M2E_RESULT=OK\n");
    return 0;
}
