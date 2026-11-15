// test_m1c.cpp — Milestone 1c 自测：app 即将调用的同一份 C 边界在 macOS 上真跑
//
// 关键：CI 无法在 iOS 设备上运行 app，所以「app 内逻辑正确」必须由同一份
// HostBridge.cpp 在 macOS host 上真跑来证明；iOS 那一侧只证明能编译 + 链接。
//
// 验证 4 件事：
//   1) 未安装宿主时 SmokeTest 明确返回 ok:false（失败路径不能假装通过）；
//   2) Install(&XP3Archive) 后返回 ok:true，eval(1+2*3)==7；
//   3) Array.load / Array.save 经 TJS2 脚本真的回调宿主，读/写计数各 +1；
//   4) Uninstall 后再跑回到 ok:false（状态可收尾，不是单向开关）。
//
// 成功打印 M1C_RESULT=OK；build_tjs2_test.sh 检查该标记。
#include "HostBridge.h"
#include "XP3Archive.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

static int Fail(const char *why, const char *json) {
    std::printf("[m1c] FAIL: %s\n[m1c] json=%s\n", why, json ? json : "(null)");
    std::printf("M1C_RESULT=ERR\n");
    return 3;
}

static bool Has(const char *json, const char *needle) {
    return json && std::strstr(json, needle) != NULL;
}

int main(int argc, char **argv) {
    const char *xp3 = (argc > 1) ? argv[1] : "dist/fixtures/demo-game.xp3";
    const char *storage = (argc > 2) ? argv[2] : ".";

    galbox::XP3Archive arc;
    std::string err;
    if (!arc.open(xp3, &err)) {
        std::printf("[m1c] open xp3 failed: %s\n", err.c_str());
        return Fail("cannot open fixture xp3", NULL);
    }
    std::printf("[m1c] fixture opened: entries=%zu\n", arc.count());

    // ---- 1) 失败路径：没安装宿主时必须明确失败 --------------------------
    char *j0 = GalBoxTJS2SmokeTest("scenario/first.ks");
    if (!Has(j0, "\"ok\":false") || !Has(j0, "host not installed"))
        return Fail("uninstalled smoke test did not fail loudly", j0);
    std::printf("[m1c] uninstalled -> %s\n", j0);
    GalBoxTJS2Free(j0);

    // ---- 2) 安装宿主 ----------------------------------------------------
    if (!GalBoxTJS2Install(&arc, storage, ""))
        return Fail("Install returned 0", NULL);
    if (!GalBoxTJS2Available()) return Fail("Available() returned 0", NULL);

    // ---- 3) 真跑：eval + TJS2 脚本回调宿主读写 --------------------------
    char *j1 = GalBoxTJS2SmokeTest("scenario/first.ks");
    std::printf("[m1c] installed  -> %s\n", j1 ? j1 : "(null)");
    if (!j1) return Fail("SmokeTest returned NULL", NULL);
    if (!Has(j1, "\"ok\":true")) return Fail("smoke test not ok", j1);
    if (!Has(j1, "\"eval\":7")) return Fail("eval did not return 7", j1);
    if (!Has(j1, "\"hostReadDelta\":1")) return Fail("Array.load did not hit host", j1);
    if (!Has(j1, "\"hostWriteDelta\":1")) return Fail("Array.save did not hit host", j1);
    if (!Has(j1, "M1C_HOST_v1")) return Fail("build tag missing", j1);

    // 闭环：宿主写计数 +1 只证明回调发生，不等于数据真的落盘。
    // 核对 Array.save 写出的内容，避免"计数自证"式的假通过。
    std::ifstream saved((std::string(storage) + "/m1c_smoke.txt").c_str(),
                        std::ios::binary);
    const std::string body((std::istreambuf_iterator<char>(saved)),
                           std::istreambuf_iterator<char>());
    std::printf("[m1c] saved file: %zu bytes, body=%s\n",
                body.size(), body.c_str());
    if (body.find("galbox") == std::string::npos)
        return Fail("Array.save did not persist content", j1);
    GalBoxTJS2Free(j1);

    // ---- 4) 收尾：卸载后必须回到失败态 ----------------------------------
    GalBoxTJS2Uninstall();
    char *j2 = GalBoxTJS2SmokeTest(NULL);
    if (!Has(j2, "\"ok\":false")) return Fail("Uninstall did not take effect", j2);
    std::printf("[m1c] uninstalled again -> %s\n", j2);
    GalBoxTJS2Free(j2);

    arc.close();
    std::printf("M1C_RESULT=OK\n");
    return 0;
}
