// Windows-only: forward the two zlib entry points used by the engine core to
// the system zlib1.dll (Git for Windows ships one in mingw64/bin).
// This lets the *same* .cpp files be compiled and exercised on this Windows
// machine without installing a full C++ toolchain's SDK headers.
// Not part of the Xcode target.
#include "zlib.h"
#ifdef _WIN32
#include <windows.h>
#include <cstdio>

typedef int (__cdecl* UncompressFn)(Bytef*, uLongf*, const Bytef*, uLong);
typedef uLong (__cdecl* AdlerFn)(uLong, const Bytef*, uInt);

static FARPROC sym(const char* name) {
    static HMODULE dll = []() -> HMODULE {
        HMODULE h = LoadLibraryA("zlib1.dll");
        if (!h) h = LoadLibraryA("C:/Program Files/Git/mingw64/bin/zlib1.dll");
        if (!h) std::fprintf(stderr, "[stub] cannot load zlib1.dll\n");
        return h;
    }();
    return dll ? GetProcAddress(dll, name) : nullptr;
}

extern "C" int uncompress(Bytef* dest, uLongf* destLen,
                          const Bytef* src, uLong srcLen) {
    auto fn = reinterpret_cast<UncompressFn>(sym("uncompress"));
    if (!fn) return Z_DATA_ERROR;
    return fn(dest, destLen, src, srcLen);
}

extern "C" uLong adler32(uLong adler, const Bytef* buf, uInt len) {
    auto fn = reinterpret_cast<AdlerFn>(sym("adler32"));
    if (!fn) return 0;
    return fn(adler, buf, len);
}
#endif
