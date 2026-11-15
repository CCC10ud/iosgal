// test_m2d.cpp — Milestone 2d 自测：正则（RegExp 原生类）
//
// M2c 结束时把阻塞点钉死在 `Member "RegExp" does not exist`
// @ system/Initialize.tjs:164。本轮不去装 boost，而是把官方 tjsRegExp.cpp
// 用 std::regex 重写（见 PATCHES.md P7），并去掉官方 TJS_NO_REGEXP 开关，
// 于是 tjs.cpp 会注册全局 RegExp、tjsArray.cpp 的 regexp-split 分支恢复编译。
//
// 两个场景：
//
//   A) 正则语义探针（m2d_regex_probe.tjs，GalBox 自写）
//      逐条断言 replace / test / match / exec / split / 标志 / 内部状态 /
//      _compile 内部表示 / "." 吃换行 / 非 ASCII。
//      判据不是「脚本跑完没报错」，而是 probeValue = checks*100 + fails：
//        - probeValue < 0  → 探针根本没执行到结尾（global 未赋值），判失败
//        - probeValue %100 → 有 check 失败（fails>0），判失败
//        - checks < 20     → 断言条数不够，等于没验证，判失败
//      这样「0 失败」不可能靠「什么都没跑」凑出来。
//
//   B) 官方 startup.tjs → system/Initialize.tjs 原文 + 探针 Config.tjs
//      M2c 时它停在 createAppLock 那一行的正则；现在必须**走过去**：
//        - appLock=true，且 appLockKey == exePath 里非字母逐字符换成 '_'
//          （这个键值由脚本侧 String.replace(RegExp) 算出，是端到端证据）
//        - 继续执行到 `KAGLoadScript("KAGLayer.tjs")` —— 该文件不在夹具里，
//          所以必然停在这里。断言失败原因正是它，证明阻塞点已经从「引擎缺
//          正则」推进到「M2e 需要的系统脚本/原生类」。
//      仍然期望 ok=false：本轮不假装 KAG 跑通。
//
// 全部通过打印 M2D_RESULT=OK。
#include "HostBridge.h"
#include "XP3Archive.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

static int Fail(const char *why, const char *json) {
    std::printf("[m2d] FAIL: %s\n[m2d] json=%s\n", why, json ? json : "(null)");
    std::printf("M2D_RESULT=ERR\n");
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
        std::printf("[m2d] open folder failed: %s (%s)\n", dir, err.c_str());
        return 0;
    }
    if (!arc.isDirectoryMode()) {
        std::printf("[m2d] %s is not a directory-mode archive\n", dir);
        return 0;
    }
    std::printf("[m2d] opened %s: entries=%zu dirMode=1\n", dir, arc.count());
    return 1;
}

// 官方那句 System.exePath.replace(/[^A-Za-z]/g, '_') 的期望值。
// 逐字符判定（不是正则），因此与 RegExp 实现完全独立 —— 不是循环校验。
static std::string ExpectedLockKey(const std::string &exePath) {
    std::string out;
    for (size_t i = 0; i < exePath.size(); ++i) {
        const char c = exePath[i];
        const bool alpha = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        out += alpha ? c : '_';
    }
    return out;
}

int main(int argc, char **argv) {
    const char *regexGame = (argc > 1) ? argv[1] : "native/TJS2/build/m2d_regex";
    const char *initGame = (argc > 2) ? argv[2] : "native/TJS2/build/m2d_init";
    const char *storage = (argc > 3) ? argv[3] : ".";

    if (!Exists(std::string(regexGame) + "/regex_probe.tjs"))
        return Fail("M2d fixture A missing", "regex_probe.tjs");
    for (const char *f : {"startup.tjs", "system/Initialize.tjs",
                          "system/Utils.tjs", "system/Config.tjs"}) {
        if (!Exists(std::string(initGame) + "/" + f))
            return Fail("M2d fixture B missing", f);
    }

    // ---- 0) 未安装宿主时必须明确失败 ------------------------------------
    {
        char *j = GalBoxTJS2ExecScript("startup.tjs", NULL);
        if (!Has(j, "\"ok\":false") || !Has(j, "host not installed"))
            return Fail("uninstalled exec did not fail loudly", j);
        GalBoxTJS2Free(j);
    }

    // ---- 场景 A：正则语义探针 -------------------------------------------
    {
        galbox::XP3Archive arc;
        if (!OpenDir(regexGame, arc)) return Fail("cannot open fixture A", NULL);
        if (!GalBoxTJS2Install(&arc, storage, ""))
            return Fail("Install(A) returned 0", NULL);

        // 探针必须非空跑：undefined 时返回 -1，否则返回 checks*100+fails。
        char *j = GalBoxTJS2ExecScript(
            "regex_probe.tjs",
            "(typeof m2dChecks == \"undefined\") ? -1 : (m2dChecks * 100 + m2dFails)");
        std::printf("[m2d] A (regex probe) -> %s\n", j ? j : "(null)");
        if (!j) return Fail("ExecScript(A) returned NULL", NULL);

        if (!Has(j, "\"regexp\":\"M2D_REGEXP_STDREGEX\""))
            return Fail("M2d build tag missing", j);
        if (!Has(j, "\"ok\":true"))
            return Fail("regex probe script did not finish cleanly", j);

        const long long probe = IntField(j, "probeValue");
        if (probe < 0)
            return Fail("probe script never assigned its result globals", j);
        if (probe % 100 != 0) {
            std::printf("[m2d] probeValue=%lld (fails=%lld)\n", probe, probe % 100);
            return Fail("at least one RegExp semantic check failed", j);
        }
        if (probe / 100 < 20)
            return Fail("too few regex checks ran (verification too thin)", j);
        std::printf("[m2d] regex checks ran=%lld, fails=0\n", probe / 100);

        GalBoxTJS2Free(j);
        GalBoxTJS2Uninstall();
        arc.close();
    }

    // ---- 场景 B：官方 Initialize.tjs 必须推过 createAppLock 的正则 --------
    {
        galbox::XP3Archive arc;
        if (!OpenDir(initGame, arc)) return Fail("cannot open fixture B", NULL);
        if (!GalBoxTJS2Install(&arc, storage, ""))
            return Fail("Install(B) returned 0", NULL);

        char *j = GalBoxTJS2ExecScript("startup.tjs", NULL);
        std::printf("[m2d] B (official Initialize.tjs) -> %s\n", j ? j : "(null)");
        if (!j) return Fail("ExecScript(B) returned NULL", NULL);

        // 官方脚本还没跑完（KAGLayer.tjs 等系统脚本属 M2e/M3），不假装成功。
        if (!Has(j, "\"ok\":false"))
            return Fail("official Initialize.tjs unexpectedly reported success", j);

        // 关键结论 1：正则阻塞点已被推过 —— createAppLock 真的被调用并放行。
        if (!Has(j, "\"appLock\":true"))
            return Fail("Initialize.tjs still dies before createAppLock", j);

        // 关键结论 2：键值是脚本侧正则算出来的，且与独立实现一致。
        const std::string exePath = StrField(j, "exePath");
        const std::string key = StrField(j, "appLockKey");
        const std::string expected = ExpectedLockKey(exePath);
        if (exePath.empty())
            return Fail("System.exePath is empty; the regex had no real input", j);
        if (key != expected) {
            std::printf("[m2d] exePath=\"%s\"\n[m2d] appLockKey=\"%s\"\n"
                        "[m2d] expected  =\"%s\"\n",
                        exePath.c_str(), key.c_str(), expected.c_str());
            return Fail("appLockKey != exePath.replace(/[^A-Za-z]/g,'_')", j);
        }

        // 关键结论 3：执行推进到了下一个真实缺口（KAGLayer.tjs 不在夹具里）。
        // 内层原因由宿主记录（lastScriptError），不能被外层 "exception in execStorage" 覆盖。
        const std::string inner = StrField(j, "lastScriptError");
        const std::string err = StrField(j, "error");
        if (inner.find("KAGLayer.tjs") == std::string::npos &&
            err.find("KAGLayer.tjs") == std::string::npos) {
            std::printf("[m2d] error=\"%s\" lastScriptError=\"%s\"\n",
                        err.c_str(), inner.c_str());
            return Fail("did not stop at the next missing system script", j);
        }

        // 走到这里说明至少加载了 startup / Initialize / Config / Utils 四次。
        if (IntField(j, "execStorageAttempts") < 4)
            return Fail("Initialize.tjs did not keep loading system scripts", j);
        if (IntField(j, "execStorageCalls") < 2)
            return Fail("too few system scripts actually executed", j);
        if (IntField(j, "debugMessages") < 2)
            return Fail("KAGLoadScript's dm() messages did not run", j);
        // Config.tjs 仍必须是 Shift-JIS 解码链上的官方脚本（探针本身是 ASCII/UTF-8）
        if (StrField(j, "lastEncoding").empty())
            return Fail("text encoding diagnostics missing", j);

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

    std::printf("M2D_RESULT=OK\n");
    return 0;
}
