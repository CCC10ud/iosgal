// GalBox Native Engine - shared byte helpers
// No third-party runtime dependency: C++14 + zlib only.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <zlib.h>

namespace galbox {

inline uint16_t rd16(const uint8_t* p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
inline uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
inline uint64_t rd64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (8 * i);
    return v;
}
inline void wr32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = (uint8_t)((v >> (8 * i)) & 0xff);
}
inline void wr64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = (uint8_t)((v >> (8 * i)) & 0xff);
}

// UTF-16LE (Kirikiri stores archive names as BMP Unicode) -> UTF-8.
// Handles surrogate pairs and replaces malformed units with U+FFFD.
inline std::string utf16leToUtf8(const uint8_t* p, size_t units) {
    auto emit = [](std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out.push_back((char)cp);
        } else if (cp < 0x800) {
            out.push_back((char)(0xc0 | (cp >> 6)));
            out.push_back((char)(0x80 | (cp & 0x3f)));
        } else if (cp < 0x10000) {
            out.push_back((char)(0xe0 | (cp >> 12)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3f)));
            out.push_back((char)(0x80 | (cp & 0x3f)));
        } else {
            out.push_back((char)(0xf0 | (cp >> 18)));
            out.push_back((char)(0x80 | ((cp >> 12) & 0x3f)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3f)));
            out.push_back((char)(0x80 | (cp & 0x3f)));
        }
    };
    std::string out;
    for (size_t i = 0; i < units; ++i) {
        uint32_t cp = rd16(p + i * 2);
        if (cp >= 0xd800 && cp <= 0xdbff && i + 1 < units) {
            uint32_t lo = rd16(p + (i + 1) * 2);
            if (lo >= 0xdc00 && lo <= 0xdfff) {
                cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                ++i;
            } else cp = 0xfffd;
        } else if (cp >= 0xd800 && cp <= 0xdfff) {
            cp = 0xfffd;
        }
        emit(out, cp);
    }
    return out;
}

// Inflate a zlib stream whose uncompressed size is already known from XP3 index.
// Returns false with a readable message instead of throwing across the ObjC bridge.
inline bool zlibInflate(const std::vector<uint8_t>& src,
                        std::vector<uint8_t>& dst, std::string* err) {
    if (dst.empty()) return true;
    if (dst.size() > (size_t)512 * 1024 * 1024) {
        if (err) *err = "解压目标过大 (>512MB)，拒绝分配";
        return false;
    }
    uLongf len = (uLongf)dst.size();
    int r = uncompress(dst.empty() ? nullptr : &dst[0], &len,
                       src.empty() ? nullptr : (const Bytef*)&src[0],
                       (uLong)src.size());
    if (r != Z_OK || (size_t)len != dst.size()) {
        if (err) *err = std::string("zlib 解压失败 (") + std::to_string(r) + ")";
        return false;
    }
    return true;
}

inline uint32_t adlerOf(const std::vector<uint8_t>& data) {
    // zlib's adler seed is 1, not 0 (adler32(0, NULL, 0) returns 1). Passing 0
    // silently yields a different checksum than Python's zlib.adler32(data).
    return (uint32_t)adler32(1L, data.empty() ? Z_NULL : (const Bytef*)&data[0],
                             (uInt)data.size());
}

}  // namespace galbox
