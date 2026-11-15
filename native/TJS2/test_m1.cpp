// test_m1.cpp — Milestone 1a 自测：官方 TJS2 的宿主文件线是否真接到了 GalBox。
//
// 验证 4 件事（在 Mac runner 上真跑，不是静态检查）：
//   1) 二进制流能从 XP3 归档内读出 scenario/first.ks，且字节数与索引一致；
//   2) 文本流能解码出 UTF-8 日文脚本，内容含 "*start" 与非 ASCII 字符；
//   3) TJS2 引擎执行 Array.load("scenario/first.ks") 时，确实回调到了
//      GalBox 宿主（ReadCallCount 增加）—— 这是 4 个函数指针接线的硬证据；
//   4) 写文本流能把数据落到 Documents，且 eval 表达式仍正常。
//
// 成功打印 M1_RESULT=OK；build_tjs2_test.sh 检查该标记。
#include "tjsCommHead.h"
#include "tjs.h"
#include "XP3Archive.hpp"
#include "HostInterface.hpp"

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static int Fail(const char* why) {
    std::printf("[m1] FAIL: %s\n", why);
    std::printf("M1_RESULT=ERR\n");
    return 3;
}

int main(int argc, char** argv)
{
    using namespace TJS;
    const char* xp3Path = (argc > 1) ? argv[1] : "dist/fixtures/demo-game.xp3";
    std::string storage = (argc > 2) ? argv[2] : ".";

    galbox::XP3Archive arc;
    std::string err;
    if (!arc.open(xp3Path, &err)) {
        std::printf("[m1] open xp3 failed: %s\n", err.c_str());
        return Fail("cannot open fixture xp3");
    }
    const galbox::XP3Entry* entry = arc.find("scenario/first.ks");
    if (!entry) return Fail("fixture has no scenario/first.ks");
    const unsigned long long expected = (unsigned long long)entry->orgSize;
    std::printf("[m1] xp3 opened: entries=%zu, first.ks orgSize=%llu\n",
                arc.count(), expected);

    galbox::host::Install(&arc, storage, "");
    if (!galbox::host::IsInstalled()) return Fail("Install() did not take effect");

    // ---- 1) 二进制流 ----------------------------------------------------
    tTJSBinaryStream* bs = TJSCreateBinaryStreamForRead(
        TJS_W("scenario/first.ks"), TJS_W(""));
    if (!bs) return Fail("binary stream factory returned NULL");
    const unsigned long long got = (unsigned long long)bs->GetSize();
    std::vector<unsigned char> head(8, 0);
    const tjs_uint nread = bs->Read(&head[0], 8);
    delete bs;
    std::printf("[m1] binary stream size=%llu read=%u head=%.6s\n",
                got, (unsigned)nread, reinterpret_cast<const char*>(head.data()));
    if (got != expected) return Fail("binary stream size != XP3 index orgSize");
    if (nread < 6 || std::memcmp(head.data(), "*start", 6) != 0)
        return Fail("binary stream content mismatch");

    // ---- 2) 文本流 ------------------------------------------------------
    iTJSTextReadStream* ts = TJSCreateTextStreamForRead(
        TJS_W("scenario/first.ks"), TJS_W(""));
    if (!ts) return Fail("text stream factory returned NULL");
    tTJSString content;
    const tjs_uint chars = ts->Read(content, 0);
    const tjs_char* cstr = content.c_str();
    const bool hasStart = cstr && std::wcsstr(cstr, L"*start") != NULL;
    bool hasNonAscii = false;
    if (cstr) {
        for (const tjs_char* p = cstr; *p; ++p) {
            if ((unsigned long)*p > 0x3000u) { hasNonAscii = true; break; }
        }
    }
    ts->Destruct();
    std::printf("[m1] text stream chars=%u hasStart=%d hasNonAscii=%d\n",
                (unsigned)chars, hasStart ? 1 : 0, hasNonAscii ? 1 : 0);
    if (!hasStart) return Fail("text stream did not contain *start");
    if (!hasNonAscii) return Fail("UTF-8 Japanese text was not decoded");

    // ---- 3) 写文本流 ----------------------------------------------------
    iTJSTextWriteStream* ws = TJSCreateTextStreamForWrite(
        TJS_W("m1_saved.txt"), TJS_W(""));
    if (!ws) return Fail("text write factory returned NULL");
    ws->Write(tTJSString(TJS_W("galbox-m1-ok")));
    ws->Destruct();
    std::ifstream saved((storage + "/m1_saved.txt").c_str(), std::ios::binary);
    const std::string savedBody((std::istreambuf_iterator<char>(saved)),
                                std::istreambuf_iterator<char>());
    std::printf("[m1] saved file bytes=%zu body=%s\n", savedBody.size(), savedBody.c_str());
    if (savedBody != "galbox-m1-ok") return Fail("text write did not persist to storage");

    // ---- 4) TJS2 引擎 + 真实宿主回调 ------------------------------------
    try {
        tTJS* engine = new tTJS();
        tTJSVariant result;

        const unsigned int before = galbox::host::ReadCallCount();
        engine->ExecScript(
            TJS_W("var a = new Array(); a.load(\"scenario/first.ks\"); a.count;"),
            &result, NULL, TJS_W("m1"), 0);
        const unsigned int after = galbox::host::ReadCallCount();
        std::printf("[m1] Array.load via TJS2: host read calls %u -> %u\n", before, after);
        if (after <= before)
            return Fail("TJS2 did NOT call GalBox host stream factory");

        engine->EvalExpression(TJS_W("1+2*3"), &result, NULL, TJS_W("m1"), 0);
        const long long v = (long long)result.AsInteger();
        std::printf("[m1] eval(1+2*3) = %lld\n", v);
        if (v != 7) return Fail("eval regression after host install");

        engine->Release();
    } catch (...) {
        return Fail("TJS2 engine threw while running Array.load/eval");
    }

    std::printf("[m1] host reads=%u writes=%u\n",
                galbox::host::ReadCallCount(), galbox::host::WriteCallCount());
    galbox::host::Uninstall();
    arc.close();
    std::printf("M1_RESULT=OK\n");
    return 0;
}
