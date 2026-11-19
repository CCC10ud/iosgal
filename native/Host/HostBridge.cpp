// HostBridge.cpp — Milestone 1c / 2a / 2b / 2c：把官方 TJS2 引擎接进 iOS app 的适配层
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
// M2c 补官方启动链第三段：`System` 与 `Debug` 两个原生静态类。
// 对着官方 kag3/template/system/Initialize.tjs 的真实语句一条条实现，而不是
// 按文档猜。这一轮的目标不是"跑通 KAG"，而是让**官方 Initialize.tjs 原文**
// 真正执行起来，并把执行停在哪一句、因为什么停下来如实报出来。
//
// 关键发现（决定了 M2c 的边界）：官方 `TJS_NO_REGEXP` 只影响 tjs.cpp:183 是否注册
// 全局 `RegExp` 类，词法器照常把 /[^A-Za-z]/g 解析成 REGEXP token，编译期不报错，
// 只有**执行到那一行**才会因找不到全局 RegExp 而失败。
//
// M2d 拆掉这个阻塞点：不再定义 TJS_NO_REGEXP，改用 GalBox 用 std::regex 重写的
// 正则层（native/TJS2/src/tjsRegExp.{h,cpp}，见 PATCHES.md P7）。于是
//   ① tjs.cpp 把全局 RegExp 类注册进 Global（`new RegExp()`、正则字面量都活）；
//   ② tjsArray.cpp 的「regexp 参与 split」分支恢复编译（`"a,b".split(/,/)`）；
//   ③ 官方 Initialize.tjs 那句 `System.exePath.replace(/[^A-Za-z]/g,'_')`
//      能真正算出键值 —— 本轮把它记进 JSON 的 appLockKey 当硬判据。
//
// M2e-1 把官方 kag3/template/system/ 的 28 个文件（20 个 .tjs + Config.~new +
// 7 个 png/asd）按 git blob 原始字节全量 vendoring。目的不是「跑通 KAG」，而是
// 把 run #32 的阻塞点（Storages: script not found via autoPath: KAGLayer.tjs）
// 从「缺文件」逼成「缺原生类」：脚本能被真实读到并解析，才会暴露
// `class KAGLayer extends Layer` 里 Layer 未实现这一真正的下一步工作。
// 为此新增 scriptTrace —— 只在**成功打开流并读到正文之后**追加短名，
// 所以「文件没找到」和「文件读到了但执行失败」在诊断里能分开，不会混为一谈。
//
// 由 build_tjs2_test.sh（macOS host，真跑自测）与 build_tjs2_ios.sh
// （iphoneos arm64，编 .o 供 app 链接）用同一套 flags 编译。

#include "HostBridge.h"

#include "tjsCommHead.h"
#include "tjs.h"
#include "tjsNative.h"
#include "tjsErrorDefs.h"
#include "tjsError.h"
#include "HostInterface.hpp"
#include "XP3Archive.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

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

// ---- M2c：System / Debug 的状态与诊断 -----------------------------------
// 这些状态全部是「一次 execScript 运行内」的观测值，由 GalBoxTJS2ExecScript
// 在开始前清零，结束后写进返回的 JSON。放在 MakeExecResult 之前定义，
// 是因为结果拼装要直接读它们（避免再加一堆函数参数）。
struct SystemState {
    std::string exePath;      // 官方语义：游戏根目录（带尾部斜杠）
    std::string title;
    std::string lastInform;
    std::string lastArgument;
    std::string appLockKey;   // createAppLock 收到的键值（M2d：由脚本侧正则算出）
    bool eventDisabled;
    bool exitRequested;
    bool appLockGranted;
    bool exceptionHandlerSet;
    unsigned tickCalls;
    unsigned informCalls;
    unsigned continuousHandlers;
    unsigned getKeyStateCalls;
    tTJSVariant exceptionHandler;   // 脚本赋进来的函数对象，由 tTJSVariant 管引用计数

    SystemState()
        : eventDisabled(false), exitRequested(false), appLockGranted(false),
          exceptionHandlerSet(false), tickCalls(0), informCalls(0),
          continuousHandlers(0), getKeyStateCalls(0) {}
};

struct DebugState {
    unsigned messageCalls;
    unsigned noticeCalls;
    unsigned logAsErrorCalls;
    std::string last;                 // 最后一条，UI 用
    std::string logLocation;          // 官方 Debug.logLocation（M2e-3 补）
    std::vector<std::string> lines;   // 最多 kDebugKeep 条，防止长跑吃内存

    DebugState() : messageCalls(0), noticeCalls(0), logAsErrorCalls(0) {}
};

const size_t kDebugKeep = 64;

SystemState gSys;
DebugState gDbg;

// 异常定位：TJS2 抛异常时把源码块名/行号/正文都带出来。
// M1c/M2a 早期只报一句 "exception while executing entry script"，
// 结果 run #17 的真根因（Array 构造参数错）被这句话藏了整整一轮。
std::string gFatalMessage;    // TJS 侧描述信息
std::string gFatalBlock;      // 出错的脚本名
long long gFatalLine = -1;    // 出错行号（1 起）
std::string gFatalException;  // throw new Exception("...") 里的正文
// 宿主侧记录的最内层失败原因（例："Storages: script not found via autoPath: X"）。
// 必须声明在 MakeExecResult 之前 —— 结果拼装要直接读它。
std::string gLastScriptError;

// execStorage 的两种计数：Attempts = 被脚本调用（进入方法即计），
// Calls = 内层脚本成功跑完。必须分开 —— 只有 Calls 的话，
// 「调用了但内层抛异常」与「压根没调用」无法区分（run #24 踩过）。
unsigned gExecStorageAttempts = 0;

// M2e：脚本读取轨迹。只记「流已成功打开且正文已读到」的短名；
// execStorage 的存在性检查失败不会写进来，因此 trace 里出现 KAGLayer.tjs
// 就等于「文件真在包里、真被解码了」，而不是「碰巧名字对上了」。
std::string gScriptTrace;
unsigned gScriptsRead = 0;

// M2e-2：GalBox 自写的 Layer 骨架（system/GalBoxLayer.tjs）加载状态。
// 取值："M2E2_LAYER_SKELETON"（已加载）/ "absent"（游戏包里没有这套骨架）/
//       "error: ..."（加载失败）。刻意**不**走 Scripts.execStorage ——
// 那会把 execStorageAttempts / scriptTrace / lastScriptError 全部污染，
// 让「官方脚本加载序列」这条判据失真（M2e-1 刚踩过 lastResolvedPath 的坑）。
std::string gShimStatus;

void ResetDiagnostics() {
    gSys = SystemState();
    // M2d：System.exePath 不再是空串，而是真实的游戏根目录（带尾部分隔符）。
    // 官方 Initialize.tjs 用它拼 autoPath（`System.exePath + "video/"`），
    // 也用它算二重起動锁的键值 —— 空串会让那句正则变成空操作的假验证。
    gSys.exePath = galbox::host::StorageRootPath();
    if (!gSys.exePath.empty() && gSys.exePath[gSys.exePath.size() - 1] != '/')
        gSys.exePath += '/';
    gDbg = DebugState();
    gFatalMessage.clear();
    gFatalBlock.clear();
    gFatalLine = -1;
    gFatalException.clear();
    gScriptTrace.clear();
    gScriptsRead = 0;
    gShimStatus.clear();
}

// 引擎销毁前清掉「持有 TJS 对象引用」的字段（exceptionHandler 里存着脚本
// 函数对象），但保留计数与布尔观测值 —— 这样 Release 之后还能拼出完整诊断。
void ClearEngineScopedDiagnostics() {
    gSys.exceptionHandler.Clear();
}

void PushDebugLine(const std::string &line, bool notice) {    if (notice) ++gDbg.noticeCalls; else ++gDbg.messageCalls;
    gDbg.last = line;
    gDbg.lines.push_back(line);
    if (gDbg.lines.size() > kDebugKeep) gDbg.lines.erase(gDbg.lines.begin());
}

// 必须在 catch 块内部调用（内部用 throw; 重新抛出当前异常来做分类）。
//
// 关键：**先捕获到局部变量，再按「不覆盖更具体信息」的原则提交**。
// 因为同一个失败会被捕获两次：execStorage 内部先抓到真正的异常，
// 之后原生方法返回 TJS_E_FAIL，TJS2 又把它转成一句笼统的
// "Unknown failure : FFFFFFFF" 抛给外层。若第二次无条件覆盖，
// 脚本名/行号会退化成调用点（run #26 实测 fatalBlock=startup.tjs:6，
// 真正抛错的 Initialize.tjs 行号被冲掉）。
void CaptureCurrentException() {
    std::string msg, block, exc;
    long long line = -1;
    try {
        throw;
    } catch (const TJS::eTJSScriptException &e) {
        msg = WideToUtf8(e.GetMessage().c_str());
        if (e.GetBlockName()) block = WideToUtf8(e.GetBlockName());
        line = static_cast<long long>(e.GetSourceLine());
        // throw new Exception("...") 的正文在异常对象的 message 属性里。
        tTJSVariant &val = const_cast<TJS::eTJSScriptException &>(e).GetValue();
        if (val.Type() == tvtObject) {
            iTJSDispatch2 *obj = val.AsObjectNoAddRef();
            if (obj) {
                tTJSVariant m;
                if (TJS_SUCCEEDED(obj->PropGet(0, TJS_W("message"), NULL, &m, obj)) &&
                    m.Type() == tvtString)
                    exc = WideToUtf8(ttstr(m).c_str());
            }
        }
    } catch (const TJS::eTJSScriptError &e) {
        msg = WideToUtf8(e.GetMessage().c_str());
        if (e.GetBlockName()) block = WideToUtf8(e.GetBlockName());
        line = static_cast<long long>(e.GetSourceLine());
    } catch (const TJS::eTJS &e) {
        msg = WideToUtf8(e.GetMessage().c_str());
    } catch (const TJS::eTJSSilent &) {
        msg = "silent exception unwind";
    } catch (...) {
        msg = "unknown C++ exception";
    }

    if (gFatalMessage.empty() && !msg.empty()) gFatalMessage = msg;
    if (gFatalBlock.empty() && !block.empty()) gFatalBlock = block;
    if (gFatalLine < 0 && line > 0) gFatalLine = line;
    if (gFatalException.empty() && !exc.empty()) gFatalException = exc;

    if (gFatalMessage.empty() && gFatalException.empty())
        gFatalMessage = "exception with empty message";
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
    s += ",\"execStorageAttempts\":" + std::to_string(gExecStorageAttempts);
    // M2e-32：evalStorage 是否被注册 + 成功求值次数。负向对照关掉注册后
    // enabled=false；正常路径 enabled=true 且 calls>=1（存档文件存在时）。
    s += ",\"evalStorageEnabled\":"
       + std::string(gEvalStorageEnabled ? "true" : "false");
    s += ",\"evalStorageCalls\":" + std::to_string(gEvalStorageCalls);
    s += ",\"build\":\"" + std::string(kBuildTag) + "\"";
    // 这三个固定标记都会被编进 app 二进制，CI 用 strings 逐个硬断言。
    // 历史标记不能因为升级而被替换掉 —— 否则「M2a/M2b 能力是否还在包里」
    // 就失去了可验证性（本轮差点把 M2A 标记覆盖成 M2C）。
    s += ",\"kag\":\"M2C_SYSTEM_DEBUG\"";
    s += ",\"storages\":\"M2B_STORAGES_AUTOPATH\"";
    s += ",\"scripts\":\"M2A_SCRIPTS_EXEC_STORAGE\"";
    // M2d：正则层换成 std::regex 重写版的固定标记
    s += ",\"regexp\":\"M2D_REGEXP_STDREGEX\"";
    // M2e：官方 KAG 系统脚本全量 vendoring 的固定标记（历史标记一律保留）
    s += ",\"kagSystem\":\"M2E_SYSTEM_SCRIPTS_VENDORED\"";
    // M2e-2：GalBox 的 Layer 骨架层（纯 TJS2 shim，替代官方 293KB 原生类）
    s += ",\"layerSkeleton\":\"M2E2_LAYER_SKELETON\"";
    // M2e-3：GalBox 的 Window 骨架层（同样纯 TJS2 shim）
    s += ",\"windowSkeleton\":\"M2E3_WINDOW_SKELETON\"";
    s += ",\"shim\":\"" + JsonEscape(gShimStatus.c_str()) + "\"";
    s += ",\"stage\":\"M2B_STORAGES_AUTOPATH\"";
    // 诊断必须能区分「真的按 autoPath 找到」与「碰巧被 basename 宽松回退命中」，
    // 也要能区分「Shift-JIS 正确解码」与「逐字节 latin-1 兜底但语法没报错」。
    s += ",\"autoPathCount\":" + std::to_string(galbox::host::AutoPathCount());
    s += ",\"autoPathHits\":" + std::to_string(galbox::host::AutoPathHitCount());
    s += ",\"shiftJisReads\":" + std::to_string(galbox::host::ShiftJisReadCount());
    s += ",\"lastEncoding\":\"" + JsonEscape(galbox::host::LastTextEncoding()) + "\"";
    s += ",\"lastResolvedPath\":\""
       + JsonEscape(galbox::host::LastResolvedPath().c_str()) + "\"";
    s += ",\"scriptTrace\":\"" + JsonEscape(gScriptTrace.c_str()) + "\"";
    s += ",\"scriptsRead\":" + std::to_string(gScriptsRead);

    // ---- M2c：System / Debug 观测值 ----
    s += ",\"exePath\":\"" + JsonEscape(gSys.exePath.c_str()) + "\"";
    s += ",\"tickCalls\":" + std::to_string(gSys.tickCalls);
    s += ",\"informCalls\":" + std::to_string(gSys.informCalls);
    s += ",\"appLock\":" + std::string(gSys.appLockGranted ? "true" : "false");
    s += ",\"appLockKey\":\"" + JsonEscape(gSys.appLockKey.c_str()) + "\"";
    s += ",\"exitRequested\":" + std::string(gSys.exitRequested ? "true" : "false");
    s += ",\"continuousHandlers\":" + std::to_string(gSys.continuousHandlers);
    s += ",\"exceptionHandlerSet\":"
       + std::string(gSys.exceptionHandlerSet ? "true" : "false");
    s += ",\"debugMessages\":" + std::to_string(gDbg.messageCalls);
    s += ",\"debugNotices\":" + std::to_string(gDbg.noticeCalls);
    s += ",\"logAsErrorCalls\":" + std::to_string(gDbg.logAsErrorCalls);
    s += ",\"lastDebug\":\"" + JsonEscape(gDbg.last.c_str()) + "\"";
    s += ",\"lastInformMsg\":\"" + JsonEscape(gSys.lastInform.c_str()) + "\"";

    // ---- 异常定位：停在哪一句、什么原因 ----
    s += ",\"fatal\":\"" + JsonEscape(gFatalMessage.c_str()) + "\"";
    s += ",\"fatalBlock\":\"" + JsonEscape(gFatalBlock.c_str()) + "\"";
    s += ",\"fatalLine\":" + std::to_string(gFatalLine);
    s += ",\"fatalException\":\"" + JsonEscape(gFatalException.c_str()) + "\"";
    // 宿主侧（最内层）失败原因单独一个字段：脚本自己 try/catch 吞掉的失败
    // 不该被当成整体失败，但也不能让它消失在诊断里。
    s += ",\"lastScriptError\":\"" + JsonEscape(gLastScriptError.c_str()) + "\"";

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
// gLastScriptError 已上移到诊断状态区（MakeExecResult 要读它）。
unsigned gExecStorageCalls = 0;      // 成功执行完的次数（gExecStorageAttempts 见上方）

// M2e-32：`Scripts.evalStorage(fn)` 是官方读取「存档/系统变量数据」的原生
// 入口（MainWindow.tjs:1020/1032/1957/2130）：读一个 storage 的正文，当
// TJS 表达式求值后返回 Dictionary。与 execStorage 的区别是后者只执行脚本、
// 不返回值。缺了它，官方 loadSystemVariables() 会抛
// `Member "evalStorage" does not exist`，被 catch 包成
// `システム変数データを読み込めないか、あるいは...` @ MainWindow.tjs:1045。
//
// 负向对照的「变量唯一」铁律：不能靠删 TJS 文件（这是 C++ 原生方法），
// 因此用一个进程级开关在**注册时**决定是否挂载 evalStorage。关掉时方法
// 真的不在 Scripts 类上，TJS 引擎会真抛 Member does not exist —— 与历史
// 缺口一字不差。默认 true（出包/app 侧永远带这个方法）。
bool gEvalStorageEnabled = true;
unsigned gEvalStorageCalls = 0;      // evalStorage 成功求值返回的次数

// 测试专用开关：负向对照场景（独立进程）在 ExecScript 前调用它关掉方法注册。
// app 侧不调用，因此设备包里 evalStorage 恒在。
extern "C" void GalBoxTJS2SetEvalStorageEnabled(int enabled) {
    gEvalStorageEnabled = (enabled != 0);
}

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
            // 尝试与成功要分开计数：内层脚本抛异常时 execStorage 会返回
            // TJS_E_FAIL，execStorageCalls 不加 —— 若只有这一个计数，
            // 「调用了但内层失败」和「根本没调用」就分不开（run #24 踩过）。
            ++gExecStorageAttempts;

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
            // 到这里文件已确实存在、流已打开、正文已读入（含 CP932 解码）。
            // 此后 ExecScript 抛异常属于「脚本内容的问题」，不再是「缺文件」。
            if (!gScriptTrace.empty()) gScriptTrace += ",";
            gScriptTrace += nameU8;
            ++gScriptsRead;
            try {
                gActiveEngine->ExecScript(body.c_str(), result, NULL,
                                          name.c_str(), 0);
                ++gExecStorageCalls;
            } catch (...) {
                // 内层脚本抛的异常必须在这里就把「哪一句、什么正文」记下来。
                // 只写一句 "exception in execStorage(xxx)" 会把 TJS 的真实错误
                // （脚本名/行号/Exception 正文）永久丢掉 —— Initialize.tjs 的
                // 失败点正是靠这些字段才能定位。
                CaptureCurrentException();
                // 同样按「不覆盖更具体信息」处理：最内层（真正的根因）先写，
                // 外层 execStorage 只补空。否则 upstream 一层层回卷时，
                // 「哪个文件没找到」会被覆盖成一句 "exception in execStorage(...)"。
                if (gLastScriptError.empty())
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

        // M2e-6：官方 UpdateConfig.tjs:93 `Scripts.exec(lines)` 执行一段
        // **脚本文本**（不是文件名）—— 把继承合并后的新 Config 内容当脚本
        // 再跑一遍，使其中 KAGWindow_config 等函数定义重新生效。
        // 与 Scripts.eval 的差别只在语义命名；底层同样是 ExecScript。
        TJS_BEGIN_NATIVE_METHOD_DECL(exec)
        {
            if (numparams < 1) return TJS_E_BADPARAMCOUNT;
            if (!gActiveEngine) {
                gLastScriptError = "no active TJS2 engine for Scripts.exec";
                return TJS_E_FAIL;
            }
            ttstr code(*param[0]);
            try {
                gActiveEngine->ExecScript(code.c_str(), NULL, NULL,
                                          TJS_W("Scripts.exec"), 0);
            } catch (...) {
                CaptureCurrentException();
                if (gLastScriptError.empty())
                    gLastScriptError = "Scripts.exec threw";
                return TJS_E_FAIL;
            }
            return TJS_S_OK;
        }
        TJS_END_NATIVE_STATIC_METHOD_DECL(exec)

        // M2e-32：`evalStorage(name, mode?)` —— 读 storage 正文，当 TJS
        // **表达式**求值后返回结果（区别于 execStorage 只执行不返回）。
        // 官方 MainWindow.tjs:1020 `scflags = Scripts.evalStorage(fn)`
        // 期望拿到一个 Dictionary；1957 `data = Scripts.evalStorage(fn, modestr)`
        // 随后读 `data.id`。这里语法走 EvalExpression，语义与 eval 一致，
        // 唯一差别是表达式正文来自 storage 而非直接参数。
        // 只在 gEvalStorageEnabled 为真时注册：负向对照关掉它，复现缺口。
        if (gEvalStorageEnabled) {
            TJS_BEGIN_NATIVE_METHOD_DECL(evalStorage)
            {
                if (numparams < 1) return TJS_E_BADPARAMCOUNT;
                ttstr name(*param[0]);
                ttstr mode;
                if (numparams >= 2 && param[1]->Type() != tvtVoid)
                    mode = *param[1];

                std::string nameU8 = WideToUtf8(name.c_str());
                // 与 execStorage 一致：先判存在，缺失文件交给调用方兜底
                // （官方 `x = %[] if x === void`）。不写 gLastScriptError，
                // 因为「存档不存在」是正常分支，不是错误。
                if (!galbox::host::IsExistentStorage(nameU8)) {
                    if (result) *result = tTJSVariant();
                    return TJS_S_OK;
                }
                iTJSTextReadStream *stream = TJSCreateTextStreamForRead(name, mode);
                if (!stream) {
                    gLastScriptError = "evalStorage: stream factory returned NULL for " +
                                       nameU8;
                    if (result) *result = tTJSVariant();
                    return TJS_S_OK;
                }
                tTJSString body;
                try {
                    stream->Read(body, 0);
                    stream->Destruct();
                } catch (...) {
                    try { stream->Destruct(); } catch (...) {}
                    gLastScriptError = "evalStorage: host read failed for " + nameU8;
                    if (result) *result = tTJSVariant();
                    return TJS_S_OK;
                }
                if (!gActiveEngine) {
                    gLastScriptError = "no active TJS2 engine for Scripts.evalStorage";
                    if (result) *result = tTJSVariant();
                    return TJS_S_OK;
                }
                tTJSVariant local;
                tTJSVariant *out = result ? result : &local;
                try {
                    gActiveEngine->EvalExpression(body.c_str(), out, NULL,
                                                 name.c_str(), 0);
                    ++gEvalStorageCalls;
                } catch (...) {
                    CaptureCurrentException();
                    if (gLastScriptError.empty())
                        gLastScriptError = "Scripts.evalStorage threw for " + nameU8;
                    return TJS_E_FAIL;
                }
                return TJS_S_OK;
            }
            TJS_END_NATIVE_STATIC_METHOD_DECL(evalStorage)
        }
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

        // M2e-6：官方 UpdateConfig.tjs:83 用 Storages.getLocalName(path) 取
        // 「本地文件名」（basename，去掉目录）。官方语义见 StoragesIntf.cpp：
        // 返回路径最后一段。这里对 / 和 \ 都切，覆盖 Windows/POSIX 两种来源。
        TJS_BEGIN_NATIVE_METHOD_DECL(getLocalName)
        {
            if (numparams < 1) return TJS_E_BADPARAMCOUNT;
            ttstr name(*param[0]);
            std::string p = WideToUtf8(name.c_str());
            size_t slash = p.find_last_of("/\\");
            std::string base = (slash == std::string::npos)
                                   ? p : p.substr(slash + 1);
            if (result) *result = ttstr(Utf8ToWide(base.c_str()).c_str());
            return TJS_S_OK;
        }
        TJS_END_NATIVE_STATIC_METHOD_DECL(getLocalName)
        TJS_END_NATIVE_MEMBERS
    }
private:
    static tjs_uint32 ClassID;
};
tjs_uint32 tTJSNC_Storages::ClassID = (tjs_uint32)-1;

// ---- M2c：官方启动链第三段 System / Debug -------------------------------
// 逐条对着 kag3/template/system/Initialize.tjs 的语句实现：
//   System.exceptionHandler = function(e){...}   → 可写属性
//   System.exePath                               → 读（拼接 "video/" 等）
//   Storages.addAutoPath(System.exePath + ...)   → 依赖上一条返回字符串
//   Debug.notice / System.osName / platformName / versionString
//   System.getTickCount()
//   System.getArgument('-ovr')
//   System.createAppLock(...) / System.inform(...) / System.exit()
// 其余成员（screenWidth/getKeyState/addContinuousHandler…）先给"不撒谎"的最小值，
// 让脚本能继续往下走，并在 JSON 里如实记录被调用了几次。

unsigned gScreenWidth = 1280;
unsigned gScreenHeight = 720;

#if defined(__ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__)
const char kOsName[] = "iOS";
#elif defined(__APPLE__)
const char kOsName[] = "macOS";
#elif defined(_WIN32)
const char kOsName[] = "Windows";
#else
const char kOsName[] = "POSIX";
#endif

#if defined(__aarch64__) || defined(__arm64__)
const char kPlatformName[] = "arm64";
#elif defined(__x86_64__)
const char kPlatformName[] = "x86_64";
#elif defined(_M_IX86) || defined(__i386__)
const char kPlatformName[] = "x86";
#else
const char kPlatformName[] = "unknown";
#endif

tjs_int TickCountMs() {
    using namespace std::chrono;
    static const steady_clock::time_point start = steady_clock::now();
    return static_cast<tjs_int>(
        duration_cast<milliseconds>(steady_clock::now() - start).count());
}

std::string JoinParams(tjs_int numparams, tTJSVariant **param) {
    std::string out;
    for (tjs_int i = 0; i < numparams; ++i) {
        if (!param[i]) continue;
        if (param[i]->Type() == tvtVoid) continue;
        out += WideToUtf8(ttstr(*param[i]).c_str());
    }
    return out;
}

class tTJSNC_System : public tTJSNativeClass {
public:
    tTJSNC_System() : tTJSNativeClass(TJS_W("System")) {
        TJS_BEGIN_NATIVE_MEMBERS(System)
        TJS_DECL_EMPTY_FINALIZE_METHOD

        TJS_BEGIN_NATIVE_PROP_DECL(exePath)
        {
            TJS_BEGIN_NATIVE_PROP_GETTER
            {
                *result = ttstr(Utf8ToWide(gSys.exePath.c_str()).c_str());
                return TJS_S_OK;
            }
            TJS_END_NATIVE_PROP_GETTER
            TJS_DENY_NATIVE_PROP_SETTER
        }
        TJS_END_NATIVE_STATIC_PROP_DECL(exePath)

        TJS_BEGIN_NATIVE_PROP_DECL(title)
        {
            TJS_BEGIN_NATIVE_PROP_GETTER
            {
                *result = ttstr(Utf8ToWide(gSys.title.c_str()).c_str());
                return TJS_S_OK;
            }
            TJS_END_NATIVE_PROP_GETTER
            TJS_BEGIN_NATIVE_PROP_SETTER
            {
                gSys.title = WideToUtf8(ttstr(*param).c_str());
                return TJS_S_OK;
            }
            TJS_END_NATIVE_PROP_SETTER
        }
        TJS_END_NATIVE_STATIC_PROP_DECL(title)

        TJS_BEGIN_NATIVE_PROP_DECL(osName)
        {
            TJS_BEGIN_NATIVE_PROP_GETTER
            {
                *result = ttstr(Utf8ToWide(kOsName).c_str());
                return TJS_S_OK;
            }
            TJS_END_NATIVE_PROP_GETTER
            TJS_DENY_NATIVE_PROP_SETTER
        }
        TJS_END_NATIVE_STATIC_PROP_DECL(osName)

        TJS_BEGIN_NATIVE_PROP_DECL(platformName)
        {
            TJS_BEGIN_NATIVE_PROP_GETTER
            {
                *result = ttstr(Utf8ToWide(kPlatformName).c_str());
                return TJS_S_OK;
            }
            TJS_END_NATIVE_PROP_GETTER
            TJS_DENY_NATIVE_PROP_SETTER
        }
        TJS_END_NATIVE_STATIC_PROP_DECL(platformName)

        TJS_BEGIN_NATIVE_PROP_DECL(versionString)
        {
            TJS_BEGIN_NATIVE_PROP_GETTER
            {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "TJS2 %d.%d.%d (GalBox)",
                              (int)TJS::TJSVersionMajor,
                              (int)TJS::TJSVersionMinor,
                              (int)TJS::TJSVersionRelease);
                *result = ttstr(Utf8ToWide(buf).c_str());
                return TJS_S_OK;
            }
            TJS_END_NATIVE_PROP_GETTER
            TJS_DENY_NATIVE_PROP_SETTER
        }
        TJS_END_NATIVE_STATIC_PROP_DECL(versionString)

        TJS_BEGIN_NATIVE_PROP_DECL(eventDisabled)
        {
            TJS_BEGIN_NATIVE_PROP_GETTER
            {
                *result = static_cast<tjs_int>(gSys.eventDisabled ? 1 : 0);
                return TJS_S_OK;
            }
            TJS_END_NATIVE_PROP_GETTER
            TJS_BEGIN_NATIVE_PROP_SETTER
            {
                gSys.eventDisabled = (static_cast<tjs_int>(*param) != 0);
                return TJS_S_OK;
            }
            TJS_END_NATIVE_PROP_SETTER
        }
        TJS_END_NATIVE_STATIC_PROP_DECL(eventDisabled)

        // 官方 Initialize.tjs 第 20 行就 `System.exceptionHandler = function(e){...}`，
        // 这个属性必须可写，否则整篇脚本第一句赋值就失败。
        TJS_BEGIN_NATIVE_PROP_DECL(exceptionHandler)
        {
            TJS_BEGIN_NATIVE_PROP_GETTER
            {
                if (result) result->CopyRef(gSys.exceptionHandler);
                return TJS_S_OK;
            }
            TJS_END_NATIVE_PROP_GETTER
            TJS_BEGIN_NATIVE_PROP_SETTER
            {
                // 脚本写进来的是函数对象；只在这里保留引用（CopyRef 加计数）。
                // 引擎销毁前由 ClearEngineScopedDiagnostics() 清掉，避免变量
                // 比引擎活得更久。
                if (param) {
                    gSys.exceptionHandler.CopyRef(*param);
                    gSys.exceptionHandlerSet = true;
                }
                return TJS_S_OK;
            }
            TJS_END_NATIVE_PROP_SETTER
        }
        TJS_END_NATIVE_STATIC_PROP_DECL(exceptionHandler)

        TJS_BEGIN_NATIVE_PROP_DECL(screenWidth)
        {
            TJS_BEGIN_NATIVE_PROP_GETTER
            {
                *result = static_cast<tjs_int>(gScreenWidth);
                return TJS_S_OK;
            }
            TJS_END_NATIVE_PROP_GETTER
            TJS_DENY_NATIVE_PROP_SETTER
        }
        TJS_END_NATIVE_STATIC_PROP_DECL(screenWidth)

        TJS_BEGIN_NATIVE_PROP_DECL(screenHeight)
        {
            TJS_BEGIN_NATIVE_PROP_GETTER
            {
                *result = static_cast<tjs_int>(gScreenHeight);
                return TJS_S_OK;
            }
            TJS_END_NATIVE_PROP_GETTER
            TJS_DENY_NATIVE_PROP_SETTER
        }
        TJS_END_NATIVE_STATIC_PROP_DECL(screenHeight)

        TJS_BEGIN_NATIVE_PROP_DECL(desktopLeft)
        {
            TJS_BEGIN_NATIVE_PROP_GETTER
            {
                *result = static_cast<tjs_int>(0);
                return TJS_S_OK;
            }
            TJS_END_NATIVE_PROP_GETTER
            TJS_DENY_NATIVE_PROP_SETTER
        }
        TJS_END_NATIVE_STATIC_PROP_DECL(desktopLeft)

        TJS_BEGIN_NATIVE_PROP_DECL(desktopTop)
        {
            TJS_BEGIN_NATIVE_PROP_GETTER
            {
                *result = static_cast<tjs_int>(0);
                return TJS_S_OK;
            }
            TJS_END_NATIVE_PROP_GETTER
            TJS_DENY_NATIVE_PROP_SETTER
        }
        TJS_END_NATIVE_STATIC_PROP_DECL(desktopTop)

        TJS_BEGIN_NATIVE_PROP_DECL(desktopWidth)
        {
            TJS_BEGIN_NATIVE_PROP_GETTER
            {
                *result = static_cast<tjs_int>(gScreenWidth);
                return TJS_S_OK;
            }
            TJS_END_NATIVE_PROP_GETTER
            TJS_DENY_NATIVE_PROP_SETTER
        }
        TJS_END_NATIVE_STATIC_PROP_DECL(desktopWidth)

        TJS_BEGIN_NATIVE_PROP_DECL(desktopHeight)
        {
            TJS_BEGIN_NATIVE_PROP_GETTER
            {
                *result = static_cast<tjs_int>(gScreenHeight);
                return TJS_S_OK;
            }
            TJS_END_NATIVE_PROP_GETTER
            TJS_DENY_NATIVE_PROP_SETTER
        }
        TJS_END_NATIVE_STATIC_PROP_DECL(desktopHeight)

        TJS_BEGIN_NATIVE_METHOD_DECL(getTickCount)
        {
            ++gSys.tickCalls;
            if (result) *result = static_cast<tjs_int>(TickCountMs());
            return TJS_S_OK;
        }
        TJS_END_NATIVE_STATIC_METHOD_DECL(getTickCount)

        TJS_BEGIN_NATIVE_METHOD_DECL(inform)
        {
            ++gSys.informCalls;
            gSys.lastInform = JoinParams(numparams, param);
            PushDebugLine("[inform] " + gSys.lastInform, true);
            return TJS_S_OK;
        }
        TJS_END_NATIVE_STATIC_METHOD_DECL(inform)

        // 官方语义是"终止应用"：这里只记录请求并抛 silent 异常把脚本栈解开，
        // 不真的退出进程 —— iOS 上没有可退的"应用"，但脚本必须停止执行。
        TJS_BEGIN_NATIVE_METHOD_DECL(exit)
        {
            gSys.exitRequested = true;
            throw TJS::eTJSSilent();
        }
        TJS_END_NATIVE_STATIC_METHOD_DECL(exit)

        TJS_BEGIN_NATIVE_METHOD_DECL(getArgument)
        {
            // iOS 无命令行。官方 Initialize.tjs 用它读 -ovr，返回 void 即可。
            gSys.lastArgument = JoinParams(numparams, param);
            return TJS_S_OK;
        }
        TJS_END_NATIVE_STATIC_METHOD_DECL(getArgument)

        TJS_BEGIN_NATIVE_METHOD_DECL(createAppLock)
        {
            // 二重起動チェック：iOS 应用天然单实例，直接放行。
            // 但键值必须原样记下来 —— 官方传的是
            // System.exePath.replace(/[^A-Za-z]/g, '_')，
            // 这个串是「脚本侧正则真的算对了」的唯一直接证据（M2d）。
            gSys.appLockKey = JoinParams(numparams, param);
            gSys.appLockGranted = true;
            if (result) *result = static_cast<tjs_int>(1);
            return TJS_S_OK;
        }
        TJS_END_NATIVE_STATIC_METHOD_DECL(createAppLock)

        TJS_BEGIN_NATIVE_METHOD_DECL(addContinuousHandler)
        {
            ++gSys.continuousHandlers;
            return TJS_S_OK;
        }
        TJS_END_NATIVE_STATIC_METHOD_DECL(addContinuousHandler)

        TJS_BEGIN_NATIVE_METHOD_DECL(removeContinuousHandler)
        {
            if (gSys.continuousHandlers > 0) --gSys.continuousHandlers;
            return TJS_S_OK;
        }
        TJS_END_NATIVE_STATIC_METHOD_DECL(removeContinuousHandler)

        TJS_BEGIN_NATIVE_METHOD_DECL(getKeyState)
        {
            // 键盘状态要等 M2d 的 iOS 输入层；现在如实返回"未按下"。
            ++gSys.getKeyStateCalls;
            if (result) *result = static_cast<tjs_int>(0);
            return TJS_S_OK;
        }
        TJS_END_NATIVE_STATIC_METHOD_DECL(getKeyState)

        // M2e-3：官方 KAGWindow 的类成员初始化就写了 `var saveDataLocation =
        // System.dataPath;`（MainWindow.tjs:38）—— 也就是说**实例化** KAGWindow
        // 的那一刻就会读它。M2c 建的 System 没有这个成员，于是它会挡住整条
        // 启动链。官方语义是「数据（栞）保存目录」，这里给出宿主真实的上报目录。
        TJS_BEGIN_NATIVE_PROP_DECL(dataPath)
        {
            TJS_BEGIN_NATIVE_PROP_GETTER
            {
                std::string p = galbox::host::StorageRootPath();
                if (!p.empty() && p[p.size() - 1] == '/')
                    p.erase(p.size() - 1);
                *result = ttstr(Utf8ToWide(p.c_str()).c_str());
                return TJS_S_OK;
            }
            TJS_END_NATIVE_PROP_GETTER
            TJS_DENY_NATIVE_PROP_SETTER
        }
        TJS_END_NATIVE_STATIC_PROP_DECL(dataPath)

        TJS_END_NATIVE_MEMBERS
    }
private:
    static tjs_uint32 ClassID;
};
tjs_uint32 tTJSNC_System::ClassID = (tjs_uint32)-1;

class tTJSNC_Debug : public tTJSNativeClass {
public:
    tTJSNC_Debug() : tTJSNativeClass(TJS_W("Debug")) {
        TJS_BEGIN_NATIVE_MEMBERS(Debug)
        TJS_DECL_EMPTY_FINALIZE_METHOD

        // 官方 Initialize.tjs: `var dm = Debug.message;` 然后 dm("...")。
        // 取函数值能不能用，取决于注册时存的是纯 dispatch（RegisterNCM 里
        // `val = dsp;`，不带 objthis）—— 因此这里必须用静态方法宏。
        TJS_BEGIN_NATIVE_METHOD_DECL(message)
        {
            PushDebugLine(JoinParams(numparams, param), false);
            return TJS_S_OK;
        }
        TJS_END_NATIVE_STATIC_METHOD_DECL(message)

        TJS_BEGIN_NATIVE_METHOD_DECL(notice)
        {
            PushDebugLine(JoinParams(numparams, param), true);
            return TJS_S_OK;
        }
        TJS_END_NATIVE_STATIC_METHOD_DECL(notice)

        TJS_BEGIN_NATIVE_METHOD_DECL(logAsError)
        {
            ++gDbg.logAsErrorCalls;
            return TJS_S_OK;
        }
        TJS_END_NATIVE_STATIC_METHOD_DECL(logAsError)

        // M2e-3：官方 KAGWindow 构造器里有 `Debug.logLocation = saveDataLocation;`
        // （MainWindow.tjs:290）。M2c 建的 Debug 没有这个属性，实例化 KAGWindow
        // 时会直接报成员不存在。官方语义是「日志输出目标路径」。
        TJS_BEGIN_NATIVE_PROP_DECL(logLocation)
        {
            TJS_BEGIN_NATIVE_PROP_GETTER
            {
                *result = ttstr(Utf8ToWide(gDbg.logLocation.c_str()).c_str());
                return TJS_S_OK;
            }
            TJS_END_NATIVE_PROP_GETTER
            TJS_BEGIN_NATIVE_PROP_SETTER
            {
                gDbg.logLocation = WideToUtf8(ttstr(*param).c_str());
                return TJS_S_OK;
            }
            TJS_END_NATIVE_PROP_SETTER
        }
        TJS_END_NATIVE_STATIC_PROP_DECL(logLocation)

        TJS_END_NATIVE_MEMBERS
    }
private:
    static tjs_uint32 ClassID;
};
tjs_uint32 tTJSNC_Debug::ClassID = (tjs_uint32)-1;

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

void RegisterSystemClass(tTJS *engine) {
    iTJSDispatch2 *global = engine->GetGlobalNoAddRef();
    if (!global) return;
    iTJSDispatch2 *dsp = new tTJSNC_System();
    tTJSVariant val;
    val = tTJSVariant(dsp, NULL);
    dsp->Release();
    global->PropSet(TJS_MEMBERENSURE, TJS_W("System"), NULL, &val, global);
}

void RegisterDebugClass(tTJS *engine) {
    iTJSDispatch2 *global = engine->GetGlobalNoAddRef();
    if (!global) return;
    iTJSDispatch2 *dsp = new tTJSNC_Debug();
    tTJSVariant val;
    val = tTJSVariant(dsp, NULL);
    dsp->Release();
    global->PropSet(TJS_MEMBERENSURE, TJS_W("Debug"), NULL, &val, global);
}

// ---- M2e-2：加载 GalBox 自写的 Layer 骨架 --------------------------------
// 官方 `Layer` 是 kirikiri2/src/core/visual/LayerIntf.cpp 里的原生类（293 KB，
// 绑死 Windows tp_stub）。KAG 的 20 个系统脚本里有 18 个 `extends Layer`，
// 没有它一个都跑不起来。M2e-2 先用一份**纯 TJS2 的骨架**顶上，让这些脚本
// 能真正被实例化，从而把「下一批缺哪个成员」逼出来。
//
// 刻意**不**经过 Scripts.execStorage：那会把 execStorageAttempts /
// scriptTrace / lastScriptError 三条判据一起污染，「官方脚本加载序列」
// 就失真了（M2e-1 刚在 lastResolvedPath 上踩过坑）。
// 同样**不**调用 CaptureCurrentException()：骨架的失败不是入口脚本的错，
// 不该占用 fatalBlock/fatalLine；它的信息单独走 JSON 的 shim 字段。
static bool LoadEngineShim(tTJS *engine) {
    // 每个骨架一个文件。状态串记录**实际装了哪些**，于是「只装了 Layer」
    // 与「Layer + Window 都装了」在诊断里能分开（M2e-3 的负向对照要用）。
    // 顺序有讲究：官方常量表用 TJS2 的 `const` 声明全局常量，必须**最先**装载
    // —— 否则后面骨架里的 global.xxx = ... 与同名常量冲突。
    static const char *kShims[] = {
        "system/GalBoxConsts.tjs",  // M2e-4：官方常量表（摘自 ScriptMgnIntf.cpp）
        "system/GalBoxFont.tjs",    // M2e-10：Layer.font 用到的 Font 骨架（Layer 构造会 new Font）
        "system/GalBoxLayer.tjs",   // M2e-2
        "system/GalBoxWindow.tjs",  // M2e-3
        "system/GalBoxPad.tjs",     // M2e-6：UpdateConfig.tjs 配置升级路径
        "system/GalBoxMenu.tjs",    // M2e-7：Menus.tjs 的 MenuItem / Menu 骨架
        "system/GalBoxTimer.tjs",   // M2e-9：MainWindow.tjs 的 Timer 骨架
        "system/GalBoxKAGParser.tjs", // M2e-11：Conductor.tjs 的 KAGParser 父类骨架
        "system/GalBoxAsyncTrigger.tjs", // M2e-12：Conductor.tjs 的 AsyncTrigger 骨架
        "system/GalBoxSound.tjs",   // M2e-25：BGM.tjs 的 MIDISoundBuffer 骨架
        "system/GalBoxMovie.tjs",   // M2e-27：Movie.tjs 的 VideoOverlay 骨架
    };
    static const char *kTags[] = {
        "M2E4_OFFICIAL_CONSTS",
        "M2E10_FONT_SKELETON",
        "M2E2_LAYER_SKELETON",
        "M2E3_WINDOW_SKELETON",
        "M2E6_PAD_SKELETON",
        "M2E7_MENU_SKELETON",
        "M2E9_TIMER_SKELETON",
        "M2E11_KAGPARSER_SKELETON",
        "M2E12_ASYNCTRIGGER_SKELETON",
        "M2E25_SOUND_SKELETON",
        "M2E27_MOVIE_SKELETON",
    };
    if (!engine) { gShimStatus = "error: no engine"; return false; }
    // tag 只在**真正装载成功**时进 status —— run #38 里 Window 骨架是语法
    // 错误，可状态串里照样出现了 "M2E3_WINDOW_SKELETON"，判据差点被骗过去。
    // 失败另进 failed，串在状态末尾的 "!failed:" 之后。
    std::string status;
    std::string failed;
    bool any = false;
    for (size_t i = 0; i < sizeof(kShims) / sizeof(kShims[0]); ++i) {
        if (!galbox::host::IsExistentStorage(kShims[i])) continue;
        any = true;
        std::wstring pathW = Utf8ToWide(kShims[i]);
        ttstr path(pathW.c_str());
        iTJSTextReadStream *st = TJSCreateTextStreamForRead(path, ttstr());
        if (!st) {
            if (!failed.empty()) failed += ";";
            failed += kTags[i];
            failed += ":open";
            continue;
        }
        tTJSString body;
        try {
            st->Read(body, 0);
            st->Destruct();
        } catch (...) {
            try { st->Destruct(); } catch (...) {}
            if (!failed.empty()) failed += ";";
            failed += kTags[i];
            failed += ":read";
            continue;
        }
        try {
            engine->ExecScript(body.c_str(), NULL, NULL, path.c_str(), 0);
            if (!status.empty()) status += "+";
            status += kTags[i];
        } catch (const TJS::eTJSScriptError &e) {
            // 语法错误必须带 block/line：run #38 里只看到一句
            // "Syntax error (syntax error)"，定位不到是第几行，只能靠二分试。
            std::string blk = e.GetBlockName() ? WideToUtf8(e.GetBlockName()) : "?";
            std::ostringstream line;
            line << e.GetSourceLine();
            if (!failed.empty()) failed += ";";
            failed += kTags[i];
            failed += ":";
            failed += blk;
            failed += ":";
            failed += line.str();
            failed += ": ";
            failed += WideToUtf8(e.GetMessage().c_str());
        } catch (const TJS::eTJS &e) {
            if (!failed.empty()) failed += ";";
            failed += kTags[i];
            failed += ":";
            failed += WideToUtf8(e.GetMessage().c_str());
        } catch (...) {
            if (!failed.empty()) failed += ";";
            failed += kTags[i];
            failed += ":unknown";
        }
    }
    if (!any) { gShimStatus = "absent"; return false; }
    gShimStatus = status;
    if (!failed.empty()) {
        // 注意：`(cond ? "" : "!") + "failed:"` 是 `const char* + const char[8]`，
        // 编译器直接报 invalid operands（run #39 红的原因）。逐段拼，别混着加。
        if (!gShimStatus.empty()) gShimStatus += "!";
        gShimStatus += "failed:";
        gShimStatus += failed;
    }
    return failed.empty();
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

// M2c：让上层把真实屏幕尺寸告诉 TJS2（System.screenWidth / desktopWidth）。
// 未调用时用 1280x720，UI 侧接上 UIScreen 后以实际值覆盖。
extern "C" void GalBoxTJS2SetScreenSize(int width, int height) {
    if (width > 0) gScreenWidth = static_cast<unsigned>(width);
    if (height > 0) gScreenHeight = static_cast<unsigned>(height);
}

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
    gExecStorageAttempts = 0;
    gEvalStorageCalls = 0;
    ResetDiagnostics();
    long long probeValue = -1;
    bool probeRan = false;
    std::string reason;
    tTJS *engine = nullptr;

    try {
        engine = new tTJS();
        gActiveEngine = engine;
        RegisterScriptsClass(engine);
        RegisterStoragesClass(engine);
        RegisterSystemClass(engine);
        RegisterDebugClass(engine);
        // M2e-2：先装上 GalBox 的 Layer 骨架（若游戏包里带了），再跑入口脚本。
        // 商业游戏通常自带官方原生 Layer 语义，这里只是过渡兜底。
        LoadEngineShim(engine);

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
                    CaptureCurrentException();
                    // 优先级：脚本自己 throw 的正文 > execStorage 的定位串 >
                    // TJS 引擎消息 > 兜底。早期只有最后一项，等于把根因藏起来。
                    if (!gFatalException.empty()) reason = gFatalException;
                    else if (!gLastScriptError.empty()) reason = gLastScriptError;
                    else if (!gFatalMessage.empty()) reason = gFatalMessage;
                    else reason = "exception while executing entry script";
                }
                // 注意：执行成功时**不要**再用 gLastScriptError 兜底。脚本自己
                // try/catch 吞掉的失败会把它留在那里，拿它当 reason 会把「脚本
                // 主动容错」误报成「整体失败」。它单独走 JSON 的 lastScriptError。
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
                CaptureCurrentException();
                reason = "probe expression threw (loaded script functions unavailable?)";
            }
        }
    } catch (...) {
        reason = "tTJS construction threw";
    }
    ClearEngineScopedDiagnostics();
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
