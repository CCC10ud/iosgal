// test_m2c.cpp — Milestone 2c 自测：官方 Initialize.tjs 原文真跑
//
// M2a 跑的是自写探针（只调 Utils.tjs）；M2b 跑的是自写探针（只测 autoPath）。
// 本轮第一次跑**官方 kag3/template/startup.tjs → system/Initialize.tjs 原文**，
// 一个字都不改，看它到底能走多远、停在哪一句、为什么停。
//
// 两个场景，覆盖两条真实结论：
//
//   A) 包里没有 Config.tjs
//      Initialize.tjs 会一路执行到
//          throw new Exception("Config.tjs が見つかりません。");
//      断言它走到了这里，即证明官方脚本里那一长串前置语句全都真的执行了：
//        - System.exceptionHandler = function(e){...}   （第一句，可写属性）
//        - System.exePath                              （读成字符串）
//        - Storages.addAutoPath(...) × 11
//        - Storages.isExistentStorage(...) × 12        （11 个 xp3 + patch2）
//        - Debug.notice(...) × 3 + System.osName/platformName/versionString
//        - System.getTickCount()
//
//   B) 包里有（探针版）Config.tjs
//      版本核对通过、跳过 UpdateConfig.tjs，执行推进到
//          System.createAppLock(System.exePath.replace(/[^A-Za-z]/g,'_'))
//      M2c 当时该行的正则字面量在 TJS_NO_REGEXP 下找不到全局 RegExp → 失败，
//      这就是「下一个阻塞点」的硬证据。**M2d 已把正则补上**，所以本文件里
//      那条「失败原因必须含 RegExp」的判据换成了「必须走到 createAppLock」
//      （appLock=true）——否则 M2c 的回归会因为「修好了」反而变红。
//      同时断言 dm(...)（`var dm = Debug.message` 取函数值再调用）确实生效。
//
// 两个场景都期望 ok=false —— 本轮**不假装** Initialize.tjs 跑通了。
// 成功跑完所有断言打印 M2C_RESULT=OK。
#include "HostBridge.h"
#include "XP3Archive.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

static int Fail(const char *why, const char *json) {
    std::printf("[m2c] FAIL: %s\n[m2c] json=%s\n", why, json ? json : "(null)");
    std::printf("M2C_RESULT=ERR\n");
    return 3;
}

static bool Has(const char *json, const char *needle) {
    return json && needle && std::strstr(json, needle) != NULL;
}

// 取扁平 JSON 里的非负整数字段，取不到返回 -1。
static long long IntField(const char *json, const char *key) {
    if (!json || !key) return -1;
    std::string pattern = std::string("\"") + key + "\":";
    const char *p = std::strstr(json, pattern.c_str());
    if (!p) return -1;
    return static_cast<long long>(std::atoll(p + pattern.size()));
}

// 取扁平 JSON 里的字符串字段（够用即可：不处理 \\u 转义）。
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
        std::printf("[m2c] open folder failed: %s (%s)\n", dir, err.c_str());
        return 0;
    }
    if (!arc.isDirectoryMode()) {
        std::printf("[m2c] %s is not a directory-mode archive\n", dir);
        return 0;
    }
    std::printf("[m2c] opened %s: entries=%zu dirMode=1\n", dir, arc.count());
    return 1;
}

int main(int argc, char **argv) {
    std::setbuf(stdout, NULL); // 崩溃诊断: 不丢已打印进度
    const char *gameA = (argc > 1) ? argv[1] : "native/TJS2/build/m2c_game";
    const char *gameB = (argc > 2) ? argv[2] : "native/TJS2/build/m2c_game_cfg";
    const char *storage = (argc > 3) ? argv[3] : ".";

    for (const char *f : {"startup.tjs", "system/Initialize.tjs"}) {
        if (!Exists(std::string(gameA) + "/" + f)) return Fail("M2c fixture A missing", f);
    }
    if (!Exists(std::string(gameB) + "/system/Config.tjs"))
        return Fail("M2c fixture B missing", "system/Config.tjs");

    // ---- 0) 未安装宿主时必须明确失败 ------------------------------------
    {
        char *j = GalBoxTJS2ExecScript("startup.tjs", NULL);
        if (!Has(j, "\"ok\":false") || !Has(j, "host not installed"))
            return Fail("uninstalled exec did not fail loudly", j);
        GalBoxTJS2Free(j);
    }

    // ---- 场景 A：官方 startup.tjs → Initialize.tjs（无 Config.tjs） -------
    {
        galbox::XP3Archive arc;
        if (!OpenDir(gameA, arc)) return Fail("cannot open fixture A", NULL);
        if (!GalBoxTJS2Install(&arc, storage, "")) return Fail("Install(A) returned 0", NULL);

        char *j = GalBoxTJS2ExecScript("startup.tjs", NULL);
        std::printf("[m2c] A (no Config) -> %s\n", j ? j : "(null)");
        if (!j) return Fail("ExecScript(A) returned NULL", NULL);

        if (!Has(j, "\"kag\":\"M2C_SYSTEM_DEBUG\""))
            return Fail("M2c build tag missing", j);
        // 本轮不假装跑通：官方 Initialize.tjs 必然停在 Config 检查处。
        if (!Has(j, "\"ok\":false"))
            return Fail("Initialize.tjs unexpectedly reported success", j);
        // 官方 startup.tjs 只有一句 Scripts.execStorage("system/Initialize.tjs")。
        // 这里必须区分「调用」与「跑完」：Initialize.tjs 内部会抛官方异常，
        // 所以 attempts=1 而 calls=0。若把两者混成一个计数，就看不出
        // 「脚本真被加载执行过、只是中途失败」与「压根没调」的区别（run #24 踩过）。
        if (IntField(j, "execStorageAttempts") != 1)
            return Fail("official startup.tjs did not attempt exactly one execStorage", j);
        if (IntField(j, "execStorageCalls") != 0)
            return Fail("execStorage counted a failed inner script as success", j);
        // Initialize.tjs 前 40 行 11 次 addAutoPath
        if (IntField(j, "autoPathCount") != 11)
            return Fail("official Initialize.tjs did not register 11 autoPaths", j);
        // 三行 Debug.notice（OS / KAG / Kirikiri）
        if (IntField(j, "debugNotices") != 3)
            return Fail("official Initialize.tjs did not emit 3 Debug.notice lines", j);
        if (IntField(j, "tickCalls") < 1)
            return Fail("System.getTickCount was never called", j);
        if (!Has(j, "\"exceptionHandlerSet\":true"))
            return Fail("System.exceptionHandler setter did not accept a function", j);
        // exePath 的语义是「游戏根目录 + 分隔符」（M2d 起由宿主真实上报，
        // 因为官方那句 createAppLock 正则需要一个有内容的输入）。
        if (StrField(j, "exePath") != std::string(storage) + "/")
            return Fail("System.exePath is not the game root", j);
        // 停在 Config 检查处 = 还没走到 createAppLock
        if (!Has(j, "\"appLock\":false"))
            return Fail("execution advanced past the Config check unexpectedly", j);
        if (!Has(j, "\"exitRequested\":false"))
            return Fail("System.exit was called on the throw path", j);
        // 异常定位：脚本名 + 行号 + 官方异常正文。
        // 注意 block 实测是调用点（startup.tjs:6 的 execStorage 那行），
        // 不是异常发源地 —— TJS2 把跨 execStorage 的异常按外层块上报。
        // 所以这里只要求「指向某个 .tjs 且行号有效」，不写死具体文件。
        if (StrField(j, "fatalBlock").find(".tjs") == std::string::npos)
            return Fail("fatalBlock does not point at a script", j);
        if (IntField(j, "fatalLine") <= 0)
            return Fail("fatalLine was not captured", j);
        if (StrField(j, "fatalException").find("Config.tjs") == std::string::npos)
            return Fail("official Exception message was lost", j);
        // 官方 startup.tjs 与 Initialize.tjs 都是 Shift-JIS
        if (!Has(j, "\"lastEncoding\":\"cp932\"") && !Has(j, "\"lastEncoding\":\"shift_jis\""))
            return Fail("official KAG3 scripts were not decoded as Shift-JIS", j);

        GalBoxTJS2Free(j);
        GalBoxTJS2Uninstall();
        arc.close();
    }

    // ---- 场景 B：加一个探针 Config.tjs，推到 createAppLock ----------------
    {
        galbox::XP3Archive arc;
        if (!OpenDir(gameB, arc)) return Fail("cannot open fixture B", NULL);
        if (!GalBoxTJS2Install(&arc, storage, "")) return Fail("Install(B) returned 0", NULL);

        char *j = GalBoxTJS2ExecScript("startup.tjs", NULL);
        std::printf("[m2c] B (probe Config) -> %s\n", j ? j : "(null)");
        if (!j) return Fail("ExecScript(B) returned NULL", NULL);

        if (!Has(j, "\"ok\":false"))
            return Fail("Initialize.tjs unexpectedly reported success", j);
        // startup.tjs + Initialize.tjs + Config.tjs -> 至少 2 次调用；
        // M2d 起还会继续加载 Utils.tjs，所以这里用下界而不是定值。
        // （原本写死 ==2，正则补上后必然变成 4 —— 断言比事实更脆弱就该改断言。）
        if (IntField(j, "execStorageAttempts") < 2)
            return Fail("Config.tjs was not attempted via Scripts.execStorage", j);
        if (IntField(j, "execStorageCalls") < 1)
            return Fail("the probe Config.tjs did not execute successfully", j);
        // ⚠️ 不能用 lastResolvedPath 判「Config.tjs 是否被解析」：它是**最后一次**
        // 解析结果。M2d 起 Initialize.tjs 会继续加载 Utils.tjs，于是它变成
        // system/Utils.tjs（run #30 实测，本断言因此红了一轮）。
        // 真正能证明 Config.tjs 被执行的是下面这条因果链：
        //   global.config_version 未定义 → KAGLoadScript("UpdateConfig.tjs")
        //   → 夹具里没有该文件 → 在 createAppLock 之前就失败。
        // 既然 appLock 变成了 true，就说明版本核对通过 = Config.tjs 真的执行了。
        if (IntField(j, "autoPathHits") < 2)
            return Fail("Config.tjs/Utils.tjs were not resolved through autoPath", j);
        if (StrField(j, "lastResolvedPath").find("system/") != 0)
            return Fail("lastResolvedPath is not a system/ script", j);
        // `var dm = Debug.message;` -> `dm(name + " を読み込みました")`
        // 取原生方法作为函数值再调用，这条路径 M2a/M2b 都没覆盖过。
        if (IntField(j, "debugMessages") < 1)
            return Fail("native method used as a function value (dm) did not run", j);
        // M2c 结束时这里曾断言「失败原因含 RegExp」——那是当轮的阻塞点。
        // M2d 补上正则后，正确的回归判据变成：**推过** createAppLock。
        // 若这里又变回 false，说明正则层回归了（M2d 的 test_m2d 会指出具体哪条）。
        if (!Has(j, "\"appLock\":true"))
            return Fail("execution did not get past createAppLock (regex regression?)", j);

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

    std::printf("M2C_RESULT=OK\n");
    return 0;
}
