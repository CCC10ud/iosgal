// Objective-C surface of the C++ engine. Swift sees only Foundation types;
// no C++ headers leak through the bridging header.
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface GBEngineBridge : NSObject

+ (BOOL)openPackageAtPath:(NSString *)path NS_SWIFT_NAME(openPackage(atPath:));
+ (BOOL)isPackageOpen;
+ (void)closePackage;
+ (NSString *)lastError;

+ (NSArray<NSDictionary<NSString *, id> *> *)fileList;
+ (nullable NSData *)dataForFile:(NSString *)name NS_SWIFT_NAME(data(forFile:));

+ (BOOL)loadScript:(NSString *)ksText;
+ (NSDictionary<NSString *, id> *)currentStep;
+ (NSDictionary<NSString *, id> *)advance;
+ (NSDictionary<NSString *, id> *)choose:(NSInteger)index;
+ (NSString *)saveState;
+ (BOOL)restoreState:(NSString *)state;
+ (NSArray<NSDictionary<NSString *, id> *> *)history;
+ (BOOL)jumpToHistory:(NSInteger)index;
+ (void)closeScript;

// Kirikiroid2-style signature detection: returns the engine id for a folder.
+ (NSString *)detectEngineAtPath:(NSString *)path NS_SWIFT_NAME(detectEngine(atPath:));

// YES when the currently open package contains a TJS2 entry point (startup.tjs).
// The TJS2 engine itself runs since M1c; the KAG layer on top of it is M2.
+ (BOOL)hasTJS2Startup NS_SWIFT_NAME(hasTJS2Startup());

// Milestone 1c: really run the vendored TJS2 engine inside the app.
// Returns a dictionary with ok / version / eval / hostReadDelta /
// hostWriteDelta / build / reason. When the app was built without the
// iOS TJS2 library it returns ok=false with a reason instead of crashing.
+ (NSDictionary<NSString *, id> *)tjs2SmokeTest NS_SWIFT_NAME(tjs2SmokeTest());

// Milestone 2a: execute startup.tjs through the official boot chain's first
// native class (Scripts.execStorage). Diagnostic only — commercial startup
// scripts still stop at Initialize.tjs because the KAG system layer is M2b.
// Returns ok / entry / hostReadDelta / execStorageCalls / error.
+ (NSDictionary<NSString *, id> *)tjs2ExecStartup NS_SWIFT_NAME(tjs2ExecStartup());

@end

NS_ASSUME_NONNULL_END
