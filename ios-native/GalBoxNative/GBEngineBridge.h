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

// YES when the currently open package contains a TJS2 entry point (startup.tjs),
// which this engine does not execute yet. Lets the UI warn instead of failing.
+ (BOOL)hasTJS2Startup NS_SWIFT_NAME(hasTJS2Startup());

@end

NS_ASSUME_NONNULL_END
