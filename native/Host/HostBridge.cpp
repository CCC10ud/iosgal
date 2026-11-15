// HostBridge.cpp — Milestone 1c / 2a / 2b：把官方 TJS2 引擎接进 iOS app 的适配层
//
// M1b 只证明 iOS 链接器能解析 TJS2 符号；M1c 要求：
//   1) 打开游戏包时安装宿主（4 个流工厂桥到 XP3Archive/Documents/bundle）；
//   2) 构造 tTJS 引擎、求值表达式；
//   3) 通过 TJS2 脚本 Array.load / Array.save 真正回调宿主读写；
//   4) 把计数与结果以 JSON 送回 Swift UI。
//
// M2a 再往前一步：注册官方启动链的第一个原生静态类 `Scripts`，实现
// `Scripts.execStorage(path)` —— 官方 startup.tjs 唯一做的事就是调它。
// 这让 TJS2 能从游戏包读 .tjs 脚本并递归执行，而不只是 eval 表达式。
//
// M2b 补官方启动链的第二个原生类 `Storages`：Initialize.tjs 开头连调 13 次
// addAutoPath("system/") 等，之后所有 execStorage 都传短文件名。没有 autoPath
// 解析就找不到任何系统脚本。同时给 execStorage 加存在性检查 —— 宿主流工厂
// 对缺失文件返回「空流」（为兼容官方 Array.load），若不检查，缺失脚本会被
// 静默当空脚本执行，把「加载失败」伪装成「加载成功」。
//
// 由 build_tjs2_test.sh（macOS host，真跑自测）与 build_tjs2_ios.sh
// （iphoneos arm64，编 .o 供 app 链接）用同一套 flags 编译。

#include "HostBridge.h"

#include "tjsCommHead.h"
#include "tjs.h"
#include "tjsNative.h"
#include "tjsErrorDefs.h"
#include "HostInterface.hpp"
#include "XP3Archive.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// 官方 TJS_BEGIN_NATIVE_METHOD_DECL 等宏展开时使用未限定的 tjs_error /
// tTJSVariant / TJSCreateNativeClassMethod，因此需要在本编译单元引入 TJS 命名空间。
using namespace TJS;

namespace {

using TJS::tTJS;
using TJS::tTJSVariant;

// JSON 里这个固定标记会被编进 app 二进制：CI 用 strings 检查它，
// 就能证明 HostBridge_ios.o 真的被链接进去了（而不是只链了 libtjs2.a）。
const char kBuildTag[] = "M1C_HOST_v1";

std::wstring Utf8ToWide(const char *s) {
    std::wstring out;
    if (!s) return out;
    const unsigned char *p = reinterpret_cast<const unsigned char *>(s);
    while (*p) {
        uint32_t cp = 0;
        int extra = 0;
        if (*p < 0x80) {
            cp = *p++;
        } else if ((*p & 0xE0) == 0xC0) {
            cp = (*p++ & 0x1F); extra = 1;
        } else if ((*p & 0xF0) == 0xE0) {
            cp = (*p++ & 0x0F); extra = 2;
        } else if ((*p & 0xF8) == 0xF0) {
            cp = (*p++ & 0x07); extra = 3;
        } else {
            cp = 0xFFFD; ++p;
        }
        for (int i = 0; i < extra && (*p & 0xC0) == 0x80; ++i)
            cp = (cp << 6) | (*p++ & 0x3F);
        out.push_back(static_cast<wchar_t>(cp));
    }
    return out;
}

std::string WideToUtf8(const wchar_t *s) {
    std::string out;
    if (!s) return out;
    while (*s) {
        uint32_t cp = static_cast<uint32_t>(*s++);
        if (cp <= 0x7F) {
            out.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

std::string JsonEscape(const char *s) {
    std::string out;
    if (!s) return out;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(s); *p; ++p) {
        char buf[8];
        switch (*p) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (*p < 0x20) { std::snprintf(buf, sizeof(buf), "\\u%04x", *p); out += buf; }
                else out += static_cast<char>(*p);
        }
    }
    return out;
}

char *DupString(const std::string &s) {
    char *p = static_cast<char *>(std::malloc(s.size() + 1));
    if (p) std::memcpy(p, s.c_str(), s.size() + 1);
    return p;
}

// 拼 JSON 时统一用这个缓冲区，避免引入 JSON 库。
std::string MakeResult(bool ok, const char *reason, long long evalValue,
                       const char *probe, unsigned readDelta, unsigned writeDelta) {
    char num[256];
    std::string s = "{\"ok\":";
    s += ok ? "true" : "false";
    std::snprintf(num, sizeof(num), "%d.%d.%d",
                  (int)TJS::TJSVersionMajor, (int)TJS::TJSVersionMinor,
                  (int)TJS::TJSVersionRelease);
    s += ",\"version\":\"" + std::string(num) + "\"";
    std::snprintf(num, sizeof(num), "%lld", evalValue);
    s += ",\"eval\":" + std::string(num);
    s += ",\"probe\":\"" + JsonEscape(probe ? probe : "") + "\"";
    s += ",\"hostReadDelta\":" + std::to_string(readDelta);
    s += ",\"hostWriteDelta\":" + std::to_string(writeDelta);
    s += ",\"build\":\"" + std::string(kBuildTag) + "\"";
    s += ",\"reason\":\"" + JsonEscape(reason ? reason : "") + "\"";
    s += "}";
    return s;
}

std::string MakeExecResult(bool ok, const char *reason, const char *entry,
                           const char *probe, long long probeValue,
                           unsigned readDelta, unsigned execCalls) {
    std::string s = "{\"ok\":";
    s += ok ? "true" : "false";
    s += ",\"entry\":\"" + JsonEscape(entry ? entry : "") + "\"";
    s += ",\"probe\":\"" + JsonEscape(probe ? probe : "") + "\"";
    s += ",\"probeValue\":" + std::to_string(probeValue);
    s += ",\"hostReadDelta\":" + std::to_string(readDelta);
    s += ",\"execStorageCalls\":" + std::to_string(execCalls);
    s += ",\"build\":\"" + std::string(kBuildTag) + "\"";
    s += ",\"kag\":\"M2A_SCRIPTS_EXEC_STORAGE\"";
    s += ",\"stage\":\"M2B_STORAGES_AUTOPATH\"";
    // 诊断必须能区分「真的按 autoPath 找到」与「碰巧被 basename 宽松回退命中」，
    // 也要能区分「Shift-JIS 正确解码」与「逐字节 latin-1 兜底但语法没报错」。
    s += ",\"autoPathCount\":" + std::to_string(galbox::host::AutoPathCount());
    s += ",\"autoPathHits\":" + std::to_string(galbox::host::AutoPathHitCount());
    s += ",\"shiftJisReads\":" + std::to_string(galbox::host::ShiftJisReadCount());
    s += ",\"lastEncoding\":\"" + JsonEscape(galbox::host::LastTextEncoding()) + "\"";
    s += ",\"lastResolvedPath\":\""
       + JsonEscape(galbox::host::LastResolvedPath().c_str()) + "\"";
    s += ",\"error\":\"" + JsonEscape(reason ? reason : "") + "\"";
    s += "}";
    return s;
}

// ---- M2a：官方启动链的第一个原生类 Scripts -------------------------------
// kag3/template/startup.tjs 的唯一有效语句是
//     Scripts.execStorage("system/Initialize.tjs");
// 官方实现在 kirikiri2/src/core/base/ScriptMgnIntf.cpp（绑 tp_stub / Windows）。
// 这里给最小 iOS 实现：从宿主读脚本 → 交给同一个 tTJS 递归执行。
tTJS *gActiveEngine = nullptr;
std::string gLastScriptError;
unsigned gExecStorageCalls = 0;

class tTJSNC_Scripts : public tTJSNativeClass {
public:
    tTJSNC_Scripts() : tTJSNativeClass(TJS_W("Scripts")) {
        TJS_BEGIN_NATIVE_MEMBERS(Scripts)
        TJS_DECL_EMPTY_FINALIZE_METHOD

        TJS_BEGIN_NATIVE_METHOD_DECL(execStorage)
        {
            if (numparams < 1) return TJS_E_BADPARAMCOUNT;
            ttstr name(*param[0]);
            ttstr mode;
            if (numparams >= 2 && param[1]->Type() != tvtVoid)
                mode = *param[1];

            // 宿主读流工厂对缺失文件返回「空流」（官方 Array.load 不做空指针
            // 检查）。execStorage 若不先判存在，缺失的 .tjs 会被当成空脚本
            // 静默执行，把加载失败伪装成成功。
            std::string nameU8 = WideToUtf8(name.c_str());
            if (!galbox::host::IsExistentStorage(nameU8)) {
                gLastScriptError = "Storages: script not found via autoPath: " + nameU8;
                return TJS_E_FAIL;
            }

            iTJSTextReadStream *stream = TJSCreateTextStreamForRead(name, mode);
            if (!stream) {
                gLastScriptError = "stream factory returned NULL for " +
                                   WideToUtf8(name.c_str());
                return TJS_E_FAIL;
            }
            tTJSString body;
            try {
                stream->Read(body, 0);
                stream->Destruct();
            } catch (...) {
                try { stream->Destruct(); } catch (...) {}
                gLastScriptError = "host read failed for " + WideToUtf8(name.c_str());
                return TJS_E_FAIL;
            }
            if (!gActiveEngine) {
                gLastScriptError = "no active TJS2 engine";
                return TJS_E_FAIL;
            }
            try {
                gActiveEngine->ExecScript(body.c_str(), result, NULL,
                                          name.c_str(), 0);
                ++gExecStorageCalls;
            } catch (...) {
                gLastScriptError = "exception in execStorage(" +
                                   WideToUtf8(name.c_str()) + ")";
                return TJS_E_FAIL;
            }
            return TJS_S_OK;
        }
        TJS_END_NATIVE_STATIC_METHOD_DECL(execStorage)

        // 官方 20 个系统脚本里 Scripts.eval 出现 31 次，是最常用的动态求值入口。
        TJS_BEGIN_NATIVE_METHOD_DECL(eval)
        {
            if (numparams < 1) return TJS_E_BADPARAMCOUNT;
            if (!gActiveEngine) {
                gLastScriptError = "no active TJS2 engine for Scripts.eval";
                return TJS_E_FAIL;
            }
            ttstr expr(*param[0]);
            tTJSVariant local;
            tTJSVariant *out = result ? result : &local;
            try {
                gActiveEngine->EvalExpression(expr.c_str(), out, NULL,
                                             TJS_W("Scripts.eval"), 0);
            } catch (...) {
                gLastScriptError = "Scripts.eval threw";
                return TJS_E_FAIL;
            }
            return TJS_S_OK;
        }
        TJS_END_NATIVE_STATIC_METHOD_DECL(eval)
        TJS_END_NATIVE_MEMBERS
    }
private:
    static tjs_uint32 ClassID;
};
tjs_uint32 tTJSNC_Scripts::ClassID = (tjs_uint32)-1;

// ---- M2b：官方启动链第二个原生类 Storages -------------------------------
// 官方 kag3/template/system/Initialize.tjs 的前 30 行就调用了 13 次
// addAutoPath；之后所有系统脚本都用短文件名取文件。这里实现最小可用子集：
//   addAutoPath / isExistentStorage / getPlacedPath / chopStorageExt
// 其余 selectFile / searchCD / extractStorage* 属文件对话框与 CD 搜索，iOS 无对应物。
class tTJSNC_Storages : public tTJSNativeClass {
public:
    tTJSNC_Storages() : tTJSNativeClass(TJS_W("Storages")) {
        TJS_BEGIN_NATIVE_MEMBERS(Storages)
        TJS_DECL_EMPTY_FINALIZE_METHOD

        TJS_BEGIN_NATIVE_METHOD_DECL(addAutoPath)
        {
            if (numparams < 1) return TJS_E_BADPARAMCOUNT;
            ttstr path(*param[0]);
            galbox::host::AddAutoPath(WideToUtf8(path.c_str()));
            return TJS_S_OK;
        }
        TJS_END_NATIVE_STATIC_METHOD_DECL(addAutoPath)

        TJS_BEGIN_NATIVE_METHOD_DECL(isExistentStorage)
        {
            if (numparams < 1) return TJS_E_BADPARAMCOUNT;
            ttstr name(*param[0]);
            bool found = galbox::host::IsExistentStorage(WideToUtf8(name.c_str()));
            if (result) *result = static_cast<tjs_int>(found ? 1 : 0);
            return TJS_S_OK;
        }
        TJS_END_NATIVE_STATIC_METHOD_DECL(isExistentStorage)

        TJS_BEGIN_NATIVE_METHOD_DECL(getPlacedPath)
        {
            if (numparams < 1) return TJS_E_BADPARAMCOUNT;
            ttstr name(*param[0]);
            std::string placed = galbox::host::GetPlacedPath(WideToUtf8(name.c_str()));
            if (result) *result = ttstr(Utf8ToWide(placed.c_str()).c_str());
            return TJS_S_OK;
        }
        TJS_END_NATIVE_STATIC_METHOD_DECL(getPlacedPath)

        TJS_BEGIN_NATIVE_METHOD_DECL(chopStorageExt)
        {
            if (numparams < 1) return TJS_E_BADPARAMCOUNT;
            ttstr name(*param[0]);
            std::string p = WideToUtf8(name.c_str());
            size_t slash = p.find_last_of("/\\");
            size_t dot = p.find_last_of('.');
            if (dot != std::string::npos &&
                (slash == std::string::npos || dot > slash))
                p.erase(dot);
            if (result) *result = ttstr(Utf8ToWide(p.c_str()).c_str());
            return TJS_S_OK;
        }
        TJS_END_NATIVE_STATIC_METHOD_DECL(chopStorageExt)
        TJS_END_NATIVE_MEMBERS
    }
private:
    static tjs_uint32 ClassID;
};
tjs_uint32 tTJSNC_Storages::ClassID = (tjs_uint32)-1;

void RegisterScriptsClass(tTJS *engine) {
    iTJSDispatch2 *global = engine->GetGlobalNoAddRef();
    if (!global) return;
    iTJSDispatch2 *dsp = new tTJSNC_Scripts();
    tTJSVariant val;
    val = tTJSVariant(dsp, NULL);
    dsp->Release();
    global->PropSet(TJS_MEMBERENSURE, TJS_W("Scripts"), NULL, &val, global);
}

void RegisterStoragesClass(tTJS *engine) {
    iTJSDispatch2 *global = engine->GetGlobalNoAddRef();
    if (!global) return;
    iTJSDispatch2 *dsp = new tTJSNC_Storages();
    tTJSVariant val;
    val = tTJSVariant(dsp, NULL);
    dsp->Release();
    global->PropSet(TJS_MEMBERENSURE, TJS_W("Storages"), NULL, &val, global);
}

}  // namespace

extern "C" int GalBoxTJS2Available(void) { return 1; }

extern "C" void GalBoxTJS2Free(char *p) { std::free(p); }

extern "C" int GalBoxTJS2Install(void *archive, const char *storageRoot,
                                 const char *bundleRoot) {
    if (!archive) return 0;
    galbox::host::Install(static_cast<galbox::XP3Archive *>(archive),
                          storageRoot ? storageRoot : "",
                          bundleRoot ? bundleRoot : "");
    return galbox::host::IsInstalled() ? 1 : 0;
}

extern "C" void GalBoxTJS2Uninstall(void) { galbox::host::Uninstall(); }

extern "C" char *GalBoxTJS2SmokeTest(const char *probeFile) {
    if (!galbox::host::IsInstalled())
        return DupString(MakeResult(false, "host not installed", -1, "", 0, 0));

    const unsigned readsBefore = galbox::host::ReadCallCount();
    const unsigned writesBefore = galbox::host::WriteCallCount();
    long long evalValue = -1;
    std::string reason;
    bool engineOk = false;

    tTJS *engine = NULL;
    try {
        engine = new tTJS();
        tTJSVariant result;

        // 每段单独包 try：失败时精确报出是 eval / load / save 哪一步，
        // 而不是笼统一句 "TJS2 exception"（run #17 就是被这句藏住了根因）。
        try {
            engine->EvalExpression(TJS_W("1+2*3"), &result, NULL,
                                   TJS_W("m1c-eval"), 0);
            evalValue = static_cast<long long>(result.AsInteger());
            engineOk = (evalValue == 7);
            if (!engineOk) reason = "eval(1+2*3) != 7";
        } catch (...) {
            reason = "eval threw";
        }

        // 经 TJS2 脚本触发宿主「读」：Array.load 会调 TJSCreateTextStreamForRead。
        if (reason.empty() && probeFile && *probeFile) {
            try {
                std::wstring script = L"var a = new Array(); a.load(\"";
                std::wstring name = Utf8ToWide(probeFile);
                for (size_t i = 0; i < name.size(); ++i) {
                    const wchar_t c = name[i];
                    if (c == L'"' || c == L'\\') script += L'\\';
                    if (c != L'\r' && c != L'\n') script += c;
                }
                script += L"\"); a.count;";
                tTJSVariant loaded;
                engine->ExecScript(script.c_str(), &loaded, NULL,
                                   TJS_W("m1c-load"), 0);
            } catch (...) {
                reason = "Array.load threw";
            }
        }

        // 经 TJS2 脚本触发宿主「写」：Array.save 会调 TJSCreateTextStreamForWrite。
        // 注意官方 Array 构造函数要求 numparams == 0（tjsArrayNI::Construct），
        // 写 `new Array("x","y")` 会抛 TJS_E_BADPARAMCOUNT —— run #17 的失败根因。
        // 正确写法是无参构造 + add()，这样写出的文件还非空，便于闭环核对。
        if (reason.empty()) {
            try {
                engine->ExecScript(
                    TJS_W("var b = new Array(); b.add(\"galbox\"); b.save(\"m1c_smoke.txt\");"),
                    NULL, NULL, TJS_W("m1c-save"), 0);
            } catch (...) {
                reason = "Array.save threw";
            }
        }
    } catch (...) {
        reason = "tTJS construction threw";
    }
    // 无论中途是否抛异常都要 Release：TJS2 的全局初始化有引用计数，
    // 漏掉会污染后续 Uninstall / 再次启动的状态。
    if (engine) {
        try { engine->Release(); }
        catch (...) { if (reason.empty()) reason = "tTJS::Release threw"; }
    }

    const unsigned readsAfter = galbox::host::ReadCallCount();
    const unsigned writesAfter = galbox::host::WriteCallCount();
    const unsigned readDelta = readsAfter - readsBefore;
    const unsigned writeDelta = writesAfter - writesBefore;

    bool ok = engineOk && writeDelta > 0;
    if (probeFile && *probeFile && readDelta == 0) {
        ok = false;
        reason = "Array.load did not call GalBox host";
    }
    if (!engineOk && reason.empty()) reason = "engine did not run";
    return DupString(MakeResult(ok, reason.c_str(), evalValue,
                                probeFile ? probeFile : "",
                                readDelta, writeDelta));
}

// M2a：执行游戏包内的 .tjs 入口脚本。
// 与 M1c 的 SmokeTest 不同，这里不是 eval 一段写死的表达式，而是
// ① 经宿主读流拿到脚本正文 → ② ExecScript → ③ 脚本里的
// Scripts.execStorage(...) 再经宿主读流加载下一个 .tjs 并递归执行。
// probeExpression 可选：执行完后求值，用来证明被加载脚本里的函数确实可用。
extern "C" char *GalBoxTJS2ExecScript(const char *entryName,
                                      const char *probeExpression) {
    if (!galbox::host::IsInstalled())
        return DupString(MakeExecResult(false, "host not installed",
                                        entryName, probeExpression, -1, 0, 0));
    if (!entryName || !*entryName)
        return DupString(MakeExecResult(false, "empty entry script name",
                                        "", probeExpression, -1, 0, 0));

    const unsigned readsBefore = galbox::host::ReadCallCount();
    // 每次执行入口脚本都视为一次全新启动：官方 Initialize.tjs 会重新
    // addAutoPath，不沿用上一次诊断运行的路径表。
    galbox::host::ClearAutoPaths();
    gActiveEngine = nullptr;
    gLastScriptError.clear();
    gExecStorageCalls = 0;
    long long probeValue = -1;
    bool probeRan = false;
    std::string reason;
    tTJS *engine = nullptr;

    try {
        engine = new tTJS();
        gActiveEngine = engine;
        RegisterScriptsClass(engine);
        RegisterStoragesClass(engine);

        std::wstring entryW = Utf8ToWide(entryName);
        ttstr entry(entryW.c_str());
        iTJSTextReadStream *st = TJSCreateTextStreamForRead(entry, ttstr());
        if (!st) {
            reason = "cannot open entry script";
        } else {
            tTJSString body;
            try {
                st->Read(body, 0);
                st->Destruct();
            } catch (...) {
                try { st->Destruct(); } catch (...) {}
                reason = "failed to read entry script";
            }
            if (reason.empty()) {
                try {
                    engine->ExecScript(body.c_str(), NULL, NULL,
                                       entry.c_str(), 0);
                } catch (...) {
                    reason = gLastScriptError.empty()
                           ? "exception while executing entry script"
                           : gLastScriptError;
                }
                if (!gLastScriptError.empty()) reason = gLastScriptError;
            }
        }

        if (reason.empty() && probeExpression && *probeExpression) {
            try {
                std::wstring expr = Utf8ToWide(probeExpression);
                tTJSVariant r;
                engine->EvalExpression(expr.c_str(), &r, NULL,
                                       TJS_W("m2a-probe"), 0);
                probeValue = static_cast<long long>(r.AsInteger());
                probeRan = true;
            } catch (...) {
                reason = "probe expression threw (loaded script functions unavailable?)";
            }
        }
    } catch (...) {
        reason = "tTJS construction threw";
    }
    if (engine) {
        try { engine->Release(); }
        catch (...) { if (reason.empty()) reason = "tTJS::Release threw"; }
    }
    gActiveEngine = nullptr;

    const unsigned readDelta = galbox::host::ReadCallCount() - readsBefore;
    bool ok = reason.empty();
    if (probeExpression && *probeExpression && !probeRan) ok = false;
    return DupString(MakeExecResult(ok, reason.c_str(), entryName,
                                    probeExpression, probeValue,
                                    readDelta, gExecStorageCalls));
}
