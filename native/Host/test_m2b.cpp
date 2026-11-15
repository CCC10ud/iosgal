// test_m2b.cpp — Milestone 2b 自测：官方启动链第二段
//
// 目标不是「跑通 KAG」，而是证明三件此前从未真验证过的事：
//   1) Storages.addAutoPath 真的按官方「后加优先」语义把短文件名
//      "Utils.tjs" 解析成 "system/Utils.tjs"（不是靠 basename 宽松回退蒙对）；
//   2) 宿主能解码并执行**官方原始 Shift-JIS 字节**的 Utils.tjs
//      （M2a 跑的是 GitHub 转码后的 UTF-8 副本，SJIS 路径从未跑过）；
//   3) 解码是 CP932/SHIFT_JIS 而不是「逐字节 latin-1 兜底但语法没报错」。
//
// 判据层层递进：
//   intrandom(5,5)==5           -> 官方脚本真的被执行
//   str2num("１２３")==123       -> 脚本内的全角日文字符字面量解码正确
//                                 （latin-1 兜底会让 case "１" 变成 2 字符而失配）
//   lastEncoding / shiftJisReads -> 解码链走了 SJIS 分支
//   autoPathHits / lastResolvedPath -> 短名确实经 autoPath 展开
//   错误 autoPath -> 必须失败      -> 解析严格、execStorage 不把缺失脚本当空脚本
//
// 成功打印 M2B_RESULT=OK；build_tjs2_test.sh 检查该标记。
#include "HostBridge.h"
#include "XP3Archive.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

static int Fail(const char *why, const char *json) {
    std::printf("[m2b] FAIL: %s\n[m2b] json=%s\n", why, json ? json : "(null)");
    std::printf("M2B_RESULT=ERR\n");
    return 3;
}

static bool Has(const char *json, const char *needle) {
    return json && std::strstr(json, needle) != NULL;
}

// 从扁平 JSON 里取一个非负整数字段，取不到返回 -1。
static long long IntField(const char *json, const char *key) {
    if (!json || !key) return -1;
    std::string pattern = std::string("\"") + key + "\":";
    const char *p = std::strstr(json, pattern.c_str());
    if (!p) return -1;
    return static_cast<long long>(std::atoll(p + pattern.size()));
}

static bool Exists(const std::string &path) {
    std::ifstream f(path.c_str(), std::ios::binary);
    return f.good();
}

int main(int argc, char **argv) {
    const char *gameDir = (argc > 1) ? argv[1] : "native/TJS2/build/m2b_game";
    const char *storage = (argc > 2) ? argv[2] : ".";

    for (const char *f : {"startup.tjs", "bad_startup.tjs", "system/Utils.tjs"}) {
        if (!Exists(std::string(gameDir) + "/" + f))
            return Fail("M2b fixture missing", f);
    }

    galbox::XP3Archive arc;
    std::string err;
    if (!arc.open(gameDir, &err)) {
        std::printf("[m2b] open directory-mode game failed: %s\n", err.c_str());
        return Fail("cannot open game folder", NULL);
    }
    std::printf("[m2b] game folder opened: entries=%zu dirMode=%d\n",
                arc.count(), arc.isDirectoryMode() ? 1 : 0);
    if (!arc.isDirectoryMode()) return Fail("fixture is not a directory-mode archive", NULL);

    // ---- 1) 失败路径：没安装宿主时必须明确失败 --------------------------
    char *j0 = GalBoxTJS2ExecScript("startup.tjs", NULL);
    if (!Has(j0, "\"ok\":false") || !Has(j0, "host not installed"))
        return Fail("uninstalled exec did not fail loudly", j0);
    std::printf("[m2b] uninstalled -> %s\n", j0);
    GalBoxTJS2Free(j0);

    if (!GalBoxTJS2Install(&arc, storage, ""))
        return Fail("Install returned 0", NULL);

    // ---- 2) 正确 autoPath + 官方原始 Shift-JIS 脚本 ----------------------
    char *j1 = GalBoxTJS2ExecScript("startup.tjs", "intrandom(5,5)");
    std::printf("[m2b] good autopath -> %s\n", j1 ? j1 : "(null)");
    if (!j1) return Fail("ExecScript returned NULL", NULL);
    if (!Has(j1, "\"ok\":true")) return Fail("startup probe did not run cleanly", j1);
    if (!Has(j1, "\"probeValue\":5"))
        return Fail("official Utils.tjs was not executed (intrandom missing)", j1);
    if (!Has(j1, "\"execStorageCalls\":1"))
        return Fail("Scripts.execStorage was not called exactly once", j1);
    if (IntField(j1, "autoPathCount") != 1)
        return Fail("Storages.addAutoPath was not registered", j1);
    if (IntField(j1, "autoPathHits") != 1)
        return Fail("short name was not resolved exactly once via autoPath", j1);
    if (!Has(j1, "\"lastResolvedPath\":\"system/Utils.tjs\""))
        return Fail("autoPath did not resolve to system/Utils.tjs", j1);
    if (IntField(j1, "shiftJisReads") < 1)
        return Fail("Shift-JIS decode path was never exercised", j1);
    if (!Has(j1, "\"lastEncoding\":\"cp932\"") &&
        !Has(j1, "\"lastEncoding\":\"shift_jis\""))
        return Fail("text fell back to latin-1 (or another unexpected encoding)", j1);
    if (!Has(j1, "\"stage\":\"M2B_STORAGES_AUTOPATH\""))
        return Fail("M2b build tag missing", j1);
    GalBoxTJS2Free(j1);

    // ---- 3) 字符串字面量解码质量 ----------------------------------------
    // 官方 Utils.tjs 的 str2num 靠 case "０".."９" 把全角数字转成半角。
    // 若日文被逐字节 latin-1 兜底，case 字面量会变成 2 个 wchar 而失配，
    // 结果不可能等于 123。这个判据比「没抛异常」硬得多。
    char *j2 = GalBoxTJS2ExecScript("startup.tjs", "str2num(\"１２３\")");
    std::printf("[m2b] unicode probe -> %s\n", j2 ? j2 : "(null)");
    if (!Has(j2, "\"ok\":true")) return Fail("unicode probe did not run cleanly", j2);
    if (!Has(j2, "\"probeValue\":123"))
        return Fail("full-width literals inside Utils.tjs were decoded wrong", j2);
    GalBoxTJS2Free(j2);

    // ---- 4) 错误 autoPath 必须失败 --------------------------------------
    // 宽松 basename 回退会让 "Utils.tjs" 碰巧命中 system/Utils.tjs；
    // execStorage 若不判存在会把缺失脚本当空脚本静默执行。两者任一存在，
    // 这个用例就会错误地通过。
    char *j3 = GalBoxTJS2ExecScript("bad_startup.tjs", NULL);
    std::printf("[m2b] bad autopath  -> %s\n", j3 ? j3 : "(null)");
    if (!Has(j3, "\"ok\":false"))
        return Fail("wrong autoPath silently succeeded (basename fallback?)", j3);
    if (!Has(j3, "script not found via autoPath"))
        return Fail("failure did not report the Storages resolution error", j3);
    GalBoxTJS2Free(j3);

    // ---- 5) 收尾：卸载后必须回到失败态 ----------------------------------
    GalBoxTJS2Uninstall();
    char *j4 = GalBoxTJS2ExecScript("startup.tjs", NULL);
    if (!Has(j4, "\"ok\":false")) return Fail("Uninstall did not take effect", j4);
    std::printf("[m2b] uninstalled again -> %s\n", j4);
    GalBoxTJS2Free(j4);

    arc.close();
    std::printf("M2B_RESULT=OK\n");
    return 0;
}
