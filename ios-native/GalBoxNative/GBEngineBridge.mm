#import "GBEngineBridge.h"
#include "XP3Archive.hpp"
#include "KagEngine.hpp"
#include <vector>
#include <string>

// M1c: 只有 build_native.sh 检测到 iOS 静态库并定义 GALBOX_HAS_TJS2 时，
// app 才引用 HostBridge 的 C 符号；否则仍可按原配置降级出包。
// 官方 tjs.h 一律不进 ObjC++ 编译单元（编译开关必须与静态库完全一致）。
#if defined(GALBOX_HAS_TJS2)
#include "HostBridge.h"
#endif

static galbox::XP3Archive gArchive;
static galbox::KagRuntime gRuntime;
static BOOL gPackageOpen = NO;
static BOOL gScriptLoaded = NO;
static NSString *gLastError = @"";

static void SetError(const std::string &msg) {
    gLastError = [NSString stringWithUTF8String:msg.c_str()] ?: @"(unknown error)";
}
static std::string ToStd(NSString *s) {
    return std::string([s UTF8String] ? [s UTF8String] : "");
}
static NSString *ToNS(const std::string &s) {
    return [NSString stringWithUTF8String:s.c_str()] ?: @"";
}

static NSDictionary<NSString *, id> *StepToDict(const galbox::Step &s) {
    NSMutableArray<NSString *> *opts = [NSMutableArray array];
    for (const auto &o : s.options) [opts addObject:ToNS(o.text)];
    NSString *type = @"Error";
    switch (s.type) {
        case galbox::StepType::Say: type = @"Say"; break;
        case galbox::StepType::Narr: type = @"Narr"; break;
        case galbox::StepType::Choice: type = @"Choice"; break;
        case galbox::StepType::Bg: type = @"Bg"; break;
        case galbox::StepType::Image: type = @"Image"; break;
        case galbox::StepType::End: type = @"End"; break;
        default: break;
    }
    return @{
        @"type": type,
        @"who": ToNS(s.who),
        @"text": ToNS(s.text),
        @"bg": ToNS(s.storage),
        @"image": ToNS(s.imageStorage),
        @"layer": ToNS(s.layer),
        @"message": ToNS(s.message),
        @"visited": @(s.visited),
        @"clearPage": @(s.clearPage),
        @"pc": @(s.pc),
        @"options": opts
    };
}

@implementation GBEngineBridge

+ (BOOL)openPackageAtPath:(NSString *)path {
    [self closePackage];
    std::string err;
    if (!gArchive.open(ToStd(path), &err)) { SetError(err); return NO; }
    gPackageOpen = YES;
    gLastError = @"";
#if defined(GALBOX_HAS_TJS2)
    // 把 4 个 TJS2 流工厂指向刚打开的游戏包 + 可写的 Documents。
    NSArray<NSString *> *dirs = NSSearchPathForDirectoriesInDomains(
        NSDocumentDirectory, NSUserDomainMask, YES);
    NSString *storage = dirs.firstObject ?: NSTemporaryDirectory();
    NSString *bundle = [[NSBundle mainBundle] bundlePath] ?: @"";
    if (!GalBoxTJS2Install(&gArchive, [storage UTF8String], [bundle UTF8String])) {
        // 宿主装不上不影响旧的 .ks 路径，但要在 UI 可查。
        SetError("TJS2 宿主安装失败（.ks 路径仍可用）");
    }
#endif
    return YES;
}

+ (BOOL)isPackageOpen { return gPackageOpen; }

+ (void)closePackage {
#if defined(GALBOX_HAS_TJS2)
    GalBoxTJS2Uninstall();
#endif
    gArchive.close();
    gPackageOpen = NO;
    [self closeScript];
}

+ (NSString *)lastError { return gLastError; }

+ (NSArray<NSDictionary<NSString *, id> *> *)fileList {
    if (!gPackageOpen) return @[];
    NSMutableArray *out = [NSMutableArray array];
    for (const auto &e : gArchive.entries()) {
        NSMutableArray *parts = [NSMutableArray array];
        for (const auto &seg : e.segments) [parts addObject:@(seg.compressed)];
        [out addObject:@{
            @"name": ToNS(e.name),
            @"orgSize": @(e.orgSize),
            @"arcSize": @(e.arcSize),
            @"hash": @(e.hash),
            @"protected": @(e.protectedFlag),
            @"parts": parts
        }];
    }
    return out;
}

+ (NSData *)dataForFile:(NSString *)name {
    if (!gPackageOpen) { SetError("尚未打开游戏包"); return nil; }
    std::vector<uint8_t> bytes;
    std::string err;
    if (!gArchive.read(ToStd(name), bytes, &err)) { SetError(err); return nil; }
    return [NSData dataWithBytes:bytes.data() length:bytes.size()];
}

+ (BOOL)loadScript:(NSString *)ksText {
    std::string err;
    gScriptLoaded = gRuntime.loadText(ToStd(ksText), &err) ? YES : NO;
    if (!gScriptLoaded) { SetError(err); return NO; }
    gLastError = @"";
    return YES;
}

+ (NSDictionary<NSString *, id> *)currentStep {
    if (!gScriptLoaded) return @{@"type": @"Error", @"message": @"剧本未载入"};
    return StepToDict(gRuntime.current());
}

+ (NSDictionary<NSString *, id> *)advance {
    if (!gScriptLoaded) return @{@"type": @"Error", @"message": @"剧本未载入"};
    return StepToDict(gRuntime.advance());
}

+ (NSDictionary<NSString *, id> *)choose:(NSInteger)index {
    if (!gScriptLoaded) return @{@"type": @"Error", @"message": @"剧本未载入"};
    return StepToDict(gRuntime.choose((int)index));
}

+ (NSString *)saveState {
    std::string out;
    gRuntime.save(out);
    return ToNS(out);
}

+ (BOOL)restoreState:(NSString *)state {
    std::string err;
    BOOL ok = gRuntime.restore(ToStd(state), &err) ? YES : NO;
    if (!ok) { SetError(err); return NO; }
    gScriptLoaded = YES;
    return YES;
}

+ (NSArray<NSDictionary<NSString *, id> *> *)history {
    NSMutableArray *out = [NSMutableArray array];
    for (const auto &h : gRuntime.history())
        [out addObject:@{@"pc": @(h.pc), @"who": ToNS(h.who), @"text": ToNS(h.text)}];
    return out;
}

+ (BOOL)jumpToHistory:(NSInteger)index {
    return gRuntime.jumpToHistory((int)index) ? YES : NO;
}

+ (void)closeScript { gScriptLoaded = NO; }

+ (NSString *)detectEngineAtPath:(NSString *)path {
    NSFileManager *fm = [NSFileManager defaultManager];
    BOOL isDir = NO;
    if (![fm fileExistsAtPath:path isDirectory:&isDir]) return @"unknown";
    NSArray *names = nil;
    if (isDir) {
        // Kirikiroid2-style: recursive-ish signature probe over the top levels.
        NSMutableArray *all = [NSMutableArray array];
        NSDirectoryEnumerator *en = [fm enumeratorAtPath:path];
        NSString *f = nil;
        while ((f = [en nextObject])) {
            [all addObject:f.lastPathComponent.lowercaseString];
            if (all.count > 4000) break;
        }
        names = all;
    } else {
        NSString *ext = path.pathExtension.lowercaseString;
        if ([ext isEqualToString:@"xp3"]) return @"kirikiri";
        return @"unknown";
    }
    BOOL hasXP3 = [names containsObject:@"data.xp3"] || [names containsObject:@"startup.tjs"];
    BOOL hasNS = [names containsObject:@"nscript.dat"] || [names containsObject:@"0.txt"];
    BOOL hasRenpy = [names containsObject:@"renpy"] || [names containsObject:@"game"];
    BOOL hasKag = NO;
    for (NSString *n in names) if ([n hasSuffix:@".ks"]) { hasKag = YES; break; }
    if (hasXP3 || hasKag) return @"kirikiri";
    if (hasNS) return @"onscripter";
    if (hasRenpy) return @"renpy";
    return @"kag-script";
}

+ (BOOL)hasTJS2Startup {
    if (!gPackageOpen) return NO;
    for (const auto &e : gArchive.entries()) {
        NSString *low = [ToNS(e.name) lowercaseString];
        if ([low isEqualToString:@"startup.tjs"] ||
            [low hasSuffix:@"/startup.tjs"]) {
            return YES;
        }
    }
    return NO;
}

+ (NSDictionary<NSString *, id> *)tjs2SmokeTest {
#if defined(GALBOX_HAS_TJS2)
    if (!gPackageOpen)
        return @{@"ok": @NO, @"reason": @"尚未打开游戏包"};

    // 挑一个包内的 ASCII 脚本名作为 Array.load 探针；找不到就传空串，
    // 冒烟仍然会跑 eval + Array.save（宿主写）。
    NSString *probe = @"";
    for (const auto &e : gArchive.entries()) {
        NSString *n = ToNS(e.name);
        NSString *low = [n lowercaseString];
        if (![low hasSuffix:@".ks"] && ![low hasSuffix:@".tjs"]) continue;
        BOOL ascii = YES;
        for (NSUInteger i = 0; i < n.length; ++i) {
            if ([n characterAtIndex:i] > 127) { ascii = NO; break; }
        }
        if (ascii) { probe = n; break; }
    }

    char *json = GalBoxTJS2SmokeTest([probe UTF8String]);
    if (!json) return @{@"ok": @NO, @"reason": @"TJS2 冒烟返回空结果"};
    NSString *text = [NSString stringWithUTF8String:json] ?: @"";
    GalBoxTJS2Free(json);

    NSData *data = [text dataUsingEncoding:NSUTF8StringEncoding];
    id parsed = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    if (![parsed isKindOfClass:[NSDictionary class]]) {
        return @{@"ok": @NO, @"reason": @"冒烟结果解析失败", @"raw": text};
    }
    NSMutableDictionary *out = [NSMutableDictionary dictionaryWithDictionary:parsed];
    out[@"probeFile"] = probe;
    out[@"tjs2Linked"] = @(GalBoxTJS2Available() != 0);
    return out;
#else
    return @{@"ok": @NO, @"reason": @"本包未链接 iOS TJS2（GALBOX_HAS_TJS2 未定义）"};
#endif
}

+ (NSDictionary<NSString *, id> *)tjs2ExecStartup {
#if defined(GALBOX_HAS_TJS2)
    if (!gPackageOpen)
        return @{@"ok": @NO, @"reason": @"尚未打开游戏包"};

    NSString *entry = nil;
    for (const auto &e : gArchive.entries()) {
        NSString *n = ToNS(e.name);
        NSString *low = [n lowercaseString];
        if ([low isEqualToString:@"startup.tjs"] ||
            [low hasSuffix:@"/startup.tjs"]) { entry = n; break; }
    }
    if (!entry) return @{@"ok": @NO, @"reason": @"包内没有 startup.tjs"};

    // 注意：这是诊断入口，caller 必须放后台队列。商业 startup.tjs 会递归
    // 加载 Initialize.tjs 等系统脚本，M2b 之前必然在缺原生类处抛异常；
    // 绝不能在主线程同步执行（可能长循环/等待）。
    char *json = GalBoxTJS2ExecScript([entry UTF8String], NULL);
    if (!json) return @{@"ok": @NO, @"reason": @"执行返回空结果"};
    NSString *text = [NSString stringWithUTF8String:json] ?: @"";
    GalBoxTJS2Free(json);

    NSData *data = [text dataUsingEncoding:NSUTF8StringEncoding];
    id parsed = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    if (![parsed isKindOfClass:[NSDictionary class]]) {
        return @{@"ok": @NO, @"reason": @"结果解析失败", @"raw": text};
    }
    NSMutableDictionary *out = [NSMutableDictionary dictionaryWithDictionary:parsed];
    out[@"entryFile"] = entry;
    return out;
#else
    return @{@"ok": @NO, @"reason": @"本包未链接 iOS TJS2（GALBOX_HAS_TJS2 未定义）"};
#endif
}

@end
