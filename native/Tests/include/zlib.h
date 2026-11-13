// Minimal zlib declarations for the Windows verification build only.
// macOS / iOS builds use the real SDK <zlib.h> and link -lz / libz.tbd.
// The implementation in win32_zlib_stub.cpp forwards to the system zlib1.dll.
#pragma once
#ifndef ZLIB_H
#define ZLIB_H

typedef unsigned char Bytef;
typedef unsigned long uLong;
typedef unsigned int uInt;
typedef unsigned long uLongf;
typedef void* voidpf;

#define Z_NULL 0
#define Z_OK 0
#define Z_BUF_ERROR (-5)
#define Z_DATA_ERROR (-3)
#define Z_MEM_ERROR (-4)

#ifdef __cplusplus
extern "C" {
#endif
int uncompress(Bytef* dest, uLongf* destLen, const Bytef* source, uLong sourceLen);
uLong adler32(uLong adler, const Bytef* buf, uInt len);
#ifdef __cplusplus
}
#endif
#endif  // ZLIB_H
