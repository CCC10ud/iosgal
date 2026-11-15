// test_m0.cpp — Milestone 0 自测：验证官方 TJS2 引擎在 iOS/macOS(clang) 下
// 能编译、链接，并正确求值表达式 `1+1`。
//
// 只依赖 vendored 的核心源码 + libtjs2.a。不接宿主文件系统/渲染。
// 成功时输出 `M0_RESULT=2`，由 build_tjs2_test.sh 检查。
#include "tjsCommHead.h"
#include "tjs.h"

#include <cstdio>

int main()
{
    using namespace TJS;

    std::printf("[m0] TJS version %d.%d.%d\n",
                (int)TJSVersionMajor, (int)TJSVersionMinor, (int)TJSVersionRelease);

    try {
        // tTJS 构造会完成全部全局初始化（字符串堆、保留字表、全局对象、内建类）。
        tTJS *engine = new tTJS();

        tTJSVariant result;
        engine->EvalExpression(TJS_W("1+1"), &result, NULL, TJS_W("m0"), 0);

        tTVInteger v = result.AsInteger();
        std::printf("[m0] eval(1+1) = %lld (type=%d)\n",
                    (long long)v, (int)result.Type());

        engine->Release();
        std::printf("M0_RESULT=%lld\n", (long long)v);
        return (v == 2) ? 0 : 3;
    }
    catch (...) {
        std::printf("[m0] exception during eval\n");
        std::printf("M0_RESULT=ERR\n");
        return 4;
    }
}
