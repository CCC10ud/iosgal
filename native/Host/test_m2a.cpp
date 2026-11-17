// test_m2a.cpp — Milestone 2a 自测：官方 KAG3 系统脚本真的被 TJS2 执行了吗
//
// 官方 kag3/template/startup.tjs 只做一件事：Scripts.execStorage(...)。
// 本自测用「目录模式归档」（Kirikiri 的 Data 文件夹形态）装一个游戏，
// 里面放：
//   startup.tjs            -> Scripts.execStorage("system/Utils.tjs");
//   system/Utils.tjs       -> 官方 KAG3 原文（UTF-8 + CRLF、日文注释、
//                             property 块、<-> 交换运算符、intrandom 函数）
//
// 判据不是「读计数 +1」这种自证，而是：加载完成后求值 intrandom(5,5)，
// 必须返回 5。Utils.tjs 没被真执行的话 intrandom 根本不存在，探针必抛异常。
// 同时检查 execStorage 被回调一次、宿主读流被走了两次（入口 + 被包含脚本）。
//
// 成功打印 M2A_RESULT=OK；build_tjs2_test.sh 检查该标记。
#include "HostBridge.h"
#include "XP3Archive.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

static int Fail(const char *why, const char *json) {
    std::printf("[m2a] FAIL: %s\n[m2a] json=%s\n", why, json ? json : "(null)");
    std::printf("M2A_RESULT=ERR\n");
    return 3;
}

static bool Has(const char *json, const char *needle) {
    return json && std::strstr(json, needle) != NULL;
}

static bool Exists(const std::string &path) {
    std::ifstream f(path.c_str(), std::ios::binary);
    return f.good();
}

int main(int argc, char **argv) {
    std::setbuf(stdout, NULL); // 崩溃诊断: 不丢已打印进度
    const char *gameDir = (argc > 1) ? argv[1] : "native/TJS2/build/m2a_game";
    const char *storage = (argc > 2) ? argv[2] : ".";

    if (!Exists(std::string(gameDir) + "/startup.tjs") ||
        !Exists(std::string(gameDir) + "/system/Utils.tjs")) {
        return Fail("vendored KAG3 scripts missing from the directory-mode game", NULL);
    }

    galbox::XP3Archive arc;
    std::string err;
    if (!arc.open(gameDir, &err)) {
        std::printf("[m2a] open directory-mode game failed: %s\n", err.c_str());
        return Fail("cannot open game folder", NULL);
    }
    std::printf("[m2a] game folder opened: entries=%zu dirMode=%d\n",
                arc.count(), arc.isDirectoryMode() ? 1 : 0);
    if (!arc.isDirectoryMode()) return Fail("fixture is not a directory-mode archive", NULL);

    // ---- 1) 失败路径：没安装宿主时必须明确失败 --------------------------
    char *j0 = GalBoxTJS2ExecScript("startup.tjs", "intrandom(5,5)");
    if (!Has(j0, "\"ok\":false") || !Has(j0, "host not installed"))
        return Fail("uninstalled exec did not fail loudly", j0);
    std::printf("[m2a] uninstalled -> %s\n", j0);
    GalBoxTJS2Free(j0);

    // ---- 2) 安装宿主并执行官方启动链第一段 ------------------------------
    if (!GalBoxTJS2Install(&arc, storage, ""))
        return Fail("Install returned 0", NULL);

    char *j1 = GalBoxTJS2ExecScript("startup.tjs", "intrandom(5,5)");
    std::printf("[m2a] installed  -> %s\n", j1 ? j1 : "(null)");
    if (!j1) return Fail("ExecScript returned NULL", NULL);
    if (!Has(j1, "\"ok\":true")) return Fail("startup.tjs did not run cleanly", j1);
    // 关键判据：官方 Utils.tjs 真的被加载并执行，intrandom(5,5) 才可能等于 5。
    if (!Has(j1, "\"probeValue\":5")) return Fail("official Utils.tjs was not executed", j1);
    if (!Has(j1, "\"execStorageCalls\":1"))
        return Fail("Scripts.execStorage was not called exactly once", j1);
    if (!Has(j1, "\"hostReadDelta\":2"))
        return Fail("entry + included script were not both read via host", j1);
    if (!Has(j1, "M2A_SCRIPTS_EXEC_STORAGE")) return Fail("M2a build tag missing", j1);
    GalBoxTJS2Free(j1);

    // ---- 3) 官方 startup.tjs 原文只作为参考：确认它指向 Initialize.tjs ----
    // （不执行：Initialize.tjs 依赖未移植的 Layer/Window/System 原生类，M2b 再补）

    // ---- 4) 收尾：卸载后必须回到失败态 ----------------------------------
    GalBoxTJS2Uninstall();
    char *j2 = GalBoxTJS2ExecScript("startup.tjs", NULL);
    if (!Has(j2, "\"ok\":false")) return Fail("Uninstall did not take effect", j2);
    std::printf("[m2a] uninstalled again -> %s\n", j2);
    GalBoxTJS2Free(j2);

    arc.close();
    std::printf("M2A_RESULT=OK\n");
    return 0;
}
