// HostInterface.hpp — GalBox ↔ 官方 TJS2 宿主契约（Milestone 1，已对齐上游源码）
//
// 【重要更正】本文件早期版本是离线凭印象写的 iTJS2Host / iTVPHost / iTVPFileSystem
// 抽象接口。核对 vendored 官方源码后确认：TJS2 **核心**里根本没有 iTJS2Host，
// 也没有 tp_stub 的 TVP* 平台层。TJS2 核心真正的宿主边界只有：
//
//   1) tjs.h:192-199 的 4 个全局函数指针（默认实现返回 NULL）：
//        TJSCreateTextStreamForRead(const ttstr&, const ttstr&) -> iTJSTextReadStream*
//        TJSCreateTextStreamForWrite(...)                        -> iTJSTextWriteStream*
//        TJSCreateBinaryStreamForRead(...)                      -> tTJSBinaryStream*
//        TJSCreateBinaryStreamForWrite(...)                     -> tTJSBinaryStream*
//      调用点：tjsArray.cpp:269/375/422/434、tjsDictionary.cpp:76/88
//      （Array.load/save、Dictionary.load/save、二进制字节码）。
//   2) tjsConfig.cpp:30 的 TJSGetTickCount()（Windows 分支 GetTickCount；
//      POSIX 分支已在 M0 编译通过，无需宿主实现）。
//
// tp_stub / TVP* 平台接口属于 kirikiri2 的插件层（KAG、视觉、音频、事件），
// 是 Milestone 2 的工作，不是 TJS2 核心的前置条件。
//
// 本契约暴露 GalBox 侧的宿主实现入口：把上述 4 个函数指针安装到
// XP3Archive（游戏包内文件）+ Documents（散文件/存档）上。
// 实现见 HostStreams.cpp。

#ifndef GALBOX_HOST_INTERFACE_HPP
#define GALBOX_HOST_INTERFACE_HPP

#include <string>

namespace galbox {
class XP3Archive;

namespace host {

// 安装宿主：archive 为当前打开的游戏包（可为 nullptr，表示只有散文件）；
// storageRoot = Documents（可写，存档落点）；bundleRoot = app bundle（只读资源）。
// 调用后 TJS::TJSCreate* 四个函数指针全部指向 GalBox 实现。
void Install(const XP3Archive* archive,
             const std::string& storageRoot,
             const std::string& bundleRoot);

// 恢复官方默认实现（返回 NULL），用于卸载游戏包或单元测试收尾。
void Uninstall();

bool IsInstalled();

// 自测/诊断：记录 TJS2 实际向宿主请求了多少次文件（证明宿主线确实被走到）。
unsigned int ReadCallCount();
unsigned int WriteCallCount();

// Milestone 2 待办（不在本契约内，先记边界）：
//   - KAG 插件的 TP_STUB 平台层：TVPGetTickCount / TVPPrint /
//     TVPThrowExceptionMessage / TVPPostEvent / TVPProcessEvents / TVPGetAppPath
//   - 事件队列 → iOS run loop + CADisplayLink（KAG @wait 靠它推进）
//   - KAG → 渲染/音频/输入回调面（文本、图像、层、转场、选项）
//   - TLG/TPP/WAV/OGG 解码、视频换 AVFoundation

}  // namespace host
}  // namespace galbox

#endif  // GALBOX_HOST_INTERFACE_HPP
