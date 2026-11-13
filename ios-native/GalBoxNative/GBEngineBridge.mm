#import "GBEngineBridge.h"
#include "XP3Archive.hpp"
#include "KagEngine.hpp"
#include <vector>
#include <string>

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
    return YES;
}

+ (BOOL)isPackageOpen { return gPackageOpen; }

+ (void)closePackage {
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

@end
