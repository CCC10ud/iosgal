// HostInterface.hpp — GalBox 宿主接口契约（TJS2 / tp_stub 风格）
//
// 这是"完整 Kirikiri 运行时"移植的拱心石（Milestone 1 落地的边界定义）。
// 官方 TJS2 通过 iTJS2Host 回调引擎层，通过 tp_stub 风格的 iTVPHost 回调平台层
// （文件/流、tick、日志、异常、事件队列、应用路径）。GalBox 必须实现这些接口，
// 把 TJS2 接到现有的 XP3Archive（文件）、Swift/ObjC++（渲染/音频）、iOS run loop（事件）上。
//
// 注意：方法名/签名是"按官方 tjs2/tjs.h + tp_stub.h 整理的契约近似"，
//       待 native/TJS2/ 源码 vendoring 后，以官方头文件为准对齐（不改本契约的语义边界）。
//       本文件仅作架构骨架与设计依据，不含实现；实现见 Host_iOS.mm（Milestone 1）。

#ifndef GALBOX_HOST_INTERFACE_HPP
#define GALBOX_HOST_INTERFACE_HPP

#include <cstdint>
#include <string>
#include <vector>

// TJS 字符类型（官方 tjs2 用 16-bit 宽字符；iOS 用 UTF-16 的 unichar 对接）
typedef char16_t tjs_char;

// ---------------------------------------------------------------------------
// iTJS2Host — 引擎层宿主回调（对应官方 tjs2/tjs.h 的 iTJS2Host）
// ---------------------------------------------------------------------------
class iTJS2Host {
public:
    virtual ~iTJS2Host() {}

    // 引擎输出（TVPPrint / console）—— GalBox 接到 os_log + Swift 控制台
    virtual void Print(const tjs_char* msg) = 0;

    // 阻塞睡眠（TJS2 内置 sleep）—— iOS 上尽量不用，留作兼容
    virtual void Sleep(tjs_uint32 ms) = 0;

    // 单调毫秒时钟 —— 实现：mach_absolute_time()/NSEC_PER_SEC
    virtual tjs_uint32 GetMilliseconds() = 0;

    // 本地化名 / 语言（TVPGetLocalName / TVPGetLanguage）
    virtual std::string GetLocalName() = 0;     // 如 "zh_CN"
    virtual int GetLanguage() = 0;              // 0=ja, 1=en, 2=zh, ... 按官方枚举

    // 错误/异常上报（引擎层不可恢复错误）
    virtual void OnError(const tjs_char* msg) = 0;

    // 退出请求（TJS2 调 exit / 宿主应退出脚本）
    virtual bool RequestQuit() = 0;

    // 多线程 dispatcher（TJS2 可选多线程）—— iOS 用 GCD 映射
    // virtual void Dispatch(...) = 0;  // Milestone 1 按官方 iTJS2Dispatcher 细化
};

// ---------------------------------------------------------------------------
// iTVPFileSystem — 平台层文件/流（对应官方 tp_stub 的 iTVPFileSystem / iTVPStream）
// 实现：读 XP3Archive 内文件 + NSFileManager(Documents) 散文件
// ---------------------------------------------------------------------------
class iTVPStream {
public:
    virtual ~iTVPStream() {}
    virtual tjs_uint64 GetSize() = 0;
    virtual tjs_uint64 GetPosition() = 0;
    virtual void SetPosition(tjs_uint64 pos) = 0;
    virtual tjs_uint Read(void* buf, tjs_uint size) = 0;   // 返回实际读取字节
    virtual void Write(const void* buf, tjs_uint size) = 0;
};

class iTVPFileSystem {
public:
    virtual ~iTVPFileSystem() {}

    // 按游戏内相对路径打开流（xp3 内或 Documents 散目录）
    virtual iTVPStream* OpenStream(const std::string& path, bool write = false) = 0;

    // 是否存在 / 大小（GalBox 接 XP3Archive::find + NSFileManager）
    virtual bool Exists(const std::string& path) = 0;
    virtual tjs_uint64 GetFileSize(const std::string& path) = 0;

    // 列出目录（KAG 读 system/、scenario/ 等）
    virtual std::vector<std::string> ListDir(const std::string& dir) = 0;

    // 字符编码探测（Shift-JIS / EUC-JP / UTF-8）—— 文本读取前判定
    virtual std::string GuessEncoding(const void* data, tjs_uint len) = 0;
};

// ---------------------------------------------------------------------------
// iTVPHost — 平台层聚合宿主（tp_stub 风格的 TVP* 入口）
// ---------------------------------------------------------------------------
class iTVPHost {
public:
    virtual ~iTVPHost() {}

    // === 时钟 / 计时 ===
    virtual tjs_uint32 GetTickCount() = 0;          // TVPGetTickCount → mach time

    // === 日志 / 异常 ===
    virtual void Print(const tjs_char* msg) = 0;    // TVPPrint
    virtual void ShowMessageBox(const tjs_char* msg) = 0; // TVPShowMessageBox
    virtual void ThrowException(const tjs_char* msg) = 0; // TVPThrowExceptionMessage

    // === 事件队列（关键：KAG @wait / 计时器靠它推进）===
    // GalBox 接到 iOS run loop + CADisplayLink：每帧 drain 事件、推进 KAG 状态机
    virtual void PostEvent(int eventId, void* data) = 0;     // TVPPostEvent
    virtual void ProcessEvents() = 0;                        // TVPProcessEvents（每帧调用）
    virtual bool QueueEmpty() = 0;

    // === 路径 / 本地化 ===
    virtual std::string GetAppPath() = 0;          // bundle 路径
    virtual std::string GetStoragePath() = 0;      // Documents（游戏存档/导入落点）
    virtual std::string GetLocalName() = 0;
    virtual int GetLanguage() = 0;

    // === 随机熵（TVPGetRandomBits 等，KAG 随机分支用）===
    virtual tjs_uint32 GetRandom() = 0;

    // === 文件系统（见上）===
    virtual iTVPFileSystem* GetFileSystem() = 0;

    // === 渲染 / 音频宿主回调（Milestone 2 细化；KAG 每帧下发指令）===
    // virtual void KAG_DrawText(...); virtual void KAG_DrawImage(...);
    // virtual void KAG_PlayAudio(...); virtual void KAG_Transition(...);
};

// ---------------------------------------------------------------------------
// 全局接入点：TJS2 引擎初始化时拿宿主实现（Milestone 1 由 Host_iOS.mm 提供）
// ---------------------------------------------------------------------------
// 官方以 TVPInit / TJS2Host 全局对象形式存在；此处用 getter 约定，
// 待源码到位对齐官方宏/全局名（如 TVPHost，TJSGetHost）。
iTJS2Host*  GalBox_GetTJS2Host();
iTVPHost*   GalBox_GetTVPHost();

#endif // GALBOX_HOST_INTERFACE_HPP
