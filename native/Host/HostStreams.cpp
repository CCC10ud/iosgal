// HostStreams.cpp — Milestone 1a：官方 TJS2 四个流工厂的 GalBox 实现
//
// 把 TJS::TJSCreate{Text,Binary}StreamFor{Read,Write} 接到
//   galbox::XP3Archive（游戏包内文件，含目录/分卷模式）
//   + Documents（散文件、存档，可写）
//   + app bundle（只读资源）
//
// 设计要点：
//  - 不修改官方源码，只在 Install() 时覆盖 tjs.h:192-199 的 4 个全局函数指针。
//  - 文本流做编码探测（UTF-8 / UTF-16LE BOM / SHIFT_JIS via iconv），
//    统一转成 tjs_char(wchar_t, 4B) 交给 TJS2；写回流统一 UTF-8。
//  - 文件不存在时返回"空流"而不是 NULL：官方 Array.load 不做空指针检查。
//  - 纯 C++14 / POSIX，不依赖 Foundation，因此同一个 .o 可同时用于
//    macOS 自测和 iOS arm64 app（app 侧已有 XP3Archive.o，勿重复打包）。

#include "HostInterface.hpp"

#include "tjsCommHead.h"   // 已包含 tjs.h，并提供官方平台配置
#include "XP3Archive.hpp"

#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>
#include <iconv.h>
#include <sys/stat.h>

namespace {

using TJS::tjs_char;
using TJS::tjs_uint;
using TJS::tjs_uint64;
using TJS::tjs_int64;
using TJS::tjs_int;
using TJS::ttstr;

const galbox::XP3Archive* gArchive = nullptr;
std::string gStorageRoot;
std::string gBundleRoot;
bool gInstalled = false;
unsigned int gReadCount = 0;
unsigned int gWriteCount = 0;

typedef TJS::iTJSTextReadStream*  (*TextReadFactory)(const ttstr&, const ttstr&);
typedef TJS::iTJSTextWriteStream* (*TextWriteFactory)(const ttstr&, const ttstr&);
typedef TJS::tTJSBinaryStream*    (*BinaryReadFactory)(const ttstr&, const ttstr&);
typedef TJS::tTJSBinaryStream*    (*BinaryWriteFactory)(const ttstr&, const ttstr&);

TextReadFactory   gOrigTextRead   = nullptr;
TextWriteFactory  gOrigTextWrite  = nullptr;
BinaryReadFactory gOrigBinaryRead = nullptr;
BinaryWriteFactory gOrigBinaryWrite = nullptr;

// ---------------------------------------------------------------------------
// 文本编码（tjs_char = wchar_t = UTF-32 on macOS/iOS）
// ---------------------------------------------------------------------------
std::string WideToUtf8(const wchar_t* s) {
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

bool ScanUtf8(const uint8_t* p, size_t n, std::wstring* out) {
    std::wstring w;
    size_t i = 0;
    while (i < n) {
        uint8_t c = p[i];
        uint32_t cp = 0;
        int extra = 0;
        if (c < 0x80) { cp = c; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
        else return false;
        ++i;
        for (int k = 0; k < extra; ++k) {
            if (i >= n || (p[i] & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (p[i] & 0x3F);
            ++i;
        }
        if (cp > 0x10FFFF) return false;
        w.push_back(static_cast<wchar_t>(cp));
    }
    if (out) out->swap(w);
    return true;
}

std::wstring Utf16LEToWide(const uint8_t* p, size_t n) {
    std::wstring out;
    for (size_t i = 0; i + 1 < n; i += 2) {
        uint32_t u = static_cast<uint32_t>(p[i]) | (static_cast<uint32_t>(p[i + 1]) << 8);
        if (u >= 0xD800 && u <= 0xDBFF && i + 3 < n) {
            uint32_t lo = static_cast<uint32_t>(p[i + 2]) | (static_cast<uint32_t>(p[i + 3]) << 8);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                out.push_back(static_cast<wchar_t>(0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00)));
                i += 2;
                continue;
            }
        }
        out.push_back(static_cast<wchar_t>(u));
    }
    return out;
}

bool IconvToWide(const char* encoding, const std::vector<uint8_t>& data, std::wstring& out) {
    iconv_t cd = iconv_open("UTF-32LE", encoding);
    if (cd == (iconv_t)-1) return false;
    std::vector<char> tmp(data.size() * 4 + 16, 0);
    char* inbuf = reinterpret_cast<char*>(const_cast<uint8_t*>(data.data()));
    size_t inleft = data.size();
    char* outbuf = tmp.data();
    size_t outleft = tmp.size();
    size_t r = iconv(cd, &inbuf, &inleft, &outbuf, &outleft);
    iconv_close(cd);
    if (r == static_cast<size_t>(-1) || inleft != 0) return false;
    size_t produced = tmp.size() - outleft;
    out.assign(reinterpret_cast<const wchar_t*>(tmp.data()), produced / sizeof(wchar_t));
    if (!out.empty() && static_cast<uint32_t>(out[0]) == 0xFEFFu) out.erase(out.begin());
    return true;
}

std::wstring DecodeText(const std::vector<uint8_t>& d) {
    if (d.empty()) return std::wstring();
    if (d.size() >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF) {
        std::wstring w;
        if (ScanUtf8(d.data() + 3, d.size() - 3, &w)) return w;
    }
    if (d.size() >= 2 && d[0] == 0xFF && d[1] == 0xFE) {
        return Utf16LEToWide(d.data() + 2, d.size() - 2);
    }
    std::wstring w;
    if (ScanUtf8(d.data(), d.size(), &w)) return w;
    // Kirikiri 的日文脚本多数是 Shift-JIS（无 BOM）。
    if (IconvToWide("SHIFT_JIS", d, w)) return w;
    w.clear();
    for (uint8_t b : d) w.push_back(static_cast<wchar_t>(b));
    return w;
}

// ---------------------------------------------------------------------------
// 逻辑路径 → 实际字节
// ---------------------------------------------------------------------------
std::string Normalize(const std::string& s) {
    std::string r;
    for (char c : s) r.push_back(c == '\\' ? '/' : c);
    while (r.size() >= 2 && r[0] == '.' && r[1] == '/') r.erase(0, 2);
    while (!r.empty() && r[0] == '/') r.erase(0, 1);
    return r;
}

bool ReadAll(const std::string& logical, std::vector<uint8_t>& out) {
    ++gReadCount;
    std::string n = Normalize(logical);
    out.clear();
    if (n.empty()) return false;
    if (gArchive && gArchive->isOpen()) {
        std::string err;
        if (gArchive->read(n, out, &err)) return true;
    }
    const std::string* roots[2] = { &gStorageRoot, &gBundleRoot };
    for (const std::string* root : roots) {
        if (root->empty()) continue;
        std::ifstream f(*root + "/" + n, std::ios::binary);
        if (!f) continue;
        f.seekg(0, std::ios::end);
        std::streamoff sz = f.tellg();
        f.seekg(0, std::ios::beg);
        if (sz < 0) continue;
        out.assign(static_cast<size_t>(sz), 0);
        if (sz > 0 && !out.empty()) f.read(reinterpret_cast<char*>(out.data()), sz);
        return true;
    }
    return false;
}

void EnsureParentDirs(const std::string& path) {
    size_t pos = 0;
    while ((pos = path.find('/', pos)) != std::string::npos) {
        std::string d = path.substr(0, pos);
        if (!d.empty()) mkdir(d.c_str(), 0755);
        ++pos;
    }
}

bool WriteAll(const std::string& logical, const std::vector<uint8_t>& data) {
    ++gWriteCount;
    std::string n = Normalize(logical);
    if (n.empty() || gStorageRoot.empty()) return false;
    std::string p = gStorageRoot + "/" + n;
    EnsureParentDirs(p);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    if (!data.empty()) f.write(reinterpret_cast<const char*>(data.data()), data.size());
    return static_cast<bool>(f);
}

// ---------------------------------------------------------------------------
// 官方接口实现
// ---------------------------------------------------------------------------
class GBTextReadStream : public TJS::iTJSTextReadStream {
public:
    explicit GBTextReadStream(const std::string& name) {
        std::vector<uint8_t> raw;
        if (ReadAll(name, raw)) text_ = DecodeText(raw);
    }
    tjs_uint TJS_INTF_METHOD Read(TJS::tTJSString& targ, tjs_uint size) {
        size_t avail = text_.size() > pos_ ? text_.size() - pos_ : 0;
        size_t n = (size == 0 || size > avail) ? avail : static_cast<size_t>(size);
        std::wstring chunk = text_.substr(pos_, n);
        targ = TJS::tTJSString(chunk.c_str());
        pos_ += n;
        return static_cast<tjs_uint>(n);
    }
    void TJS_INTF_METHOD Destruct() { delete this; }
private:
    std::wstring text_;
    size_t pos_ = 0;
};

class GBTextWriteStream : public TJS::iTJSTextWriteStream {
public:
    explicit GBTextWriteStream(const std::string& name) : name_(name) {}
    void TJS_INTF_METHOD Write(const TJS::tTJSString& targ) {
        const tjs_char* p = targ.c_str();
        if (p) while (*p) buf_.push_back(static_cast<wchar_t>(*p++));
    }
    void TJS_INTF_METHOD Destruct() {
        std::string u8 = WideToUtf8(buf_.c_str());
        std::vector<uint8_t> data(u8.begin(), u8.end());
        WriteAll(name_, data);
        delete this;
    }
private:
    std::string name_;
    std::wstring buf_;
};

class GBBinaryReadStream : public TJS::tTJSBinaryStream {
public:
    explicit GBBinaryReadStream(const std::string& name) { ReadAll(name, data_); }

    tjs_uint64 TJS_INTF_METHOD Seek(tjs_int64 offset, tjs_int whence) {
        tjs_int64 base = (whence == TJS_BS_SEEK_SET) ? 0
                       : (whence == TJS_BS_SEEK_CUR) ? static_cast<tjs_int64>(pos_)
                       : static_cast<tjs_int64>(data_.size());
        tjs_int64 np = base + offset;
        if (np < 0) np = 0;
        if (static_cast<tjs_uint64>(np) > data_.size()) np = static_cast<tjs_int64>(data_.size());
        pos_ = static_cast<tjs_uint64>(np);
        return pos_;
    }
    tjs_uint TJS_INTF_METHOD Read(void* buffer, tjs_uint read_size) {
        if (pos_ >= data_.size()) return 0;
        size_t n = data_.size() - static_cast<size_t>(pos_);
        if (n > read_size) n = read_size;
        std::memcpy(buffer, data_.data() + pos_, n);
        pos_ += n;
        return static_cast<tjs_uint>(n);
    }
    tjs_uint TJS_INTF_METHOD Write(const void*, tjs_uint) { return 0; }
    tjs_uint64 TJS_INTF_METHOD GetSize() { return data_.size(); }
private:
    std::vector<uint8_t> data_;
    tjs_uint64 pos_ = 0;
};

class GBBinaryWriteStream : public TJS::tTJSBinaryStream {
public:
    explicit GBBinaryWriteStream(const std::string& name) : name_(name) {}
    ~GBBinaryWriteStream() {
        std::vector<uint8_t> snapshot(data_.begin(), data_.end());
        WriteAll(name_, snapshot);
    }
    tjs_uint64 TJS_INTF_METHOD Seek(tjs_int64 offset, tjs_int whence) {
        tjs_int64 base = (whence == TJS_BS_SEEK_SET) ? 0
                       : (whence == TJS_BS_SEEK_CUR) ? static_cast<tjs_int64>(pos_)
                       : static_cast<tjs_int64>(data_.size());
        tjs_int64 np = base + offset;
        if (np < 0) np = 0;
        if (static_cast<tjs_uint64>(np) > data_.size()) np = static_cast<tjs_int64>(data_.size());
        pos_ = static_cast<tjs_uint64>(np);
        return pos_;
    }
    tjs_uint TJS_INTF_METHOD Read(void* buffer, tjs_uint read_size) {
        if (pos_ >= data_.size()) return 0;
        size_t n = data_.size() - static_cast<size_t>(pos_);
        if (n > read_size) n = read_size;
        std::memcpy(buffer, data_.data() + pos_, n);
        pos_ += n;
        return static_cast<tjs_uint>(n);
    }
    tjs_uint TJS_INTF_METHOD Write(const void* buffer, tjs_uint write_size) {
        if (write_size == 0) return 0;
        size_t end = static_cast<size_t>(pos_) + write_size;
        if (end > data_.size()) data_.resize(end, 0);
        std::memcpy(data_.data() + pos_, buffer, write_size);
        pos_ += write_size;
        return write_size;
    }
    void TJS_INTF_METHOD SetEndOfStorage() {
        data_.resize(static_cast<size_t>(pos_));
    }
    tjs_uint64 TJS_INTF_METHOD GetSize() { return data_.size(); }
private:
    std::string name_;
    std::vector<uint8_t> data_;
    tjs_uint64 pos_ = 0;
};

TJS::iTJSTextReadStream* CreateTextRead(const ttstr& name, const ttstr&) {
    return new GBTextReadStream(WideToUtf8(name.c_str()));
}
TJS::iTJSTextWriteStream* CreateTextWrite(const ttstr& name, const ttstr&) {
    return new GBTextWriteStream(WideToUtf8(name.c_str()));
}
TJS::tTJSBinaryStream* CreateBinaryRead(const ttstr& name, const ttstr&) {
    return new GBBinaryReadStream(WideToUtf8(name.c_str()));
}
TJS::tTJSBinaryStream* CreateBinaryWrite(const ttstr& name, const ttstr&) {
    return new GBBinaryWriteStream(WideToUtf8(name.c_str()));
}

}  // namespace

namespace galbox {
namespace host {

void Install(const XP3Archive* archive,
             const std::string& storageRoot,
             const std::string& bundleRoot) {
    if (!gOrigTextRead)   gOrigTextRead   = TJS::TJSCreateTextStreamForRead;
    if (!gOrigTextWrite)  gOrigTextWrite  = TJS::TJSCreateTextStreamForWrite;
    if (!gOrigBinaryRead) gOrigBinaryRead = TJS::TJSCreateBinaryStreamForRead;
    if (!gOrigBinaryWrite) gOrigBinaryWrite = TJS::TJSCreateBinaryStreamForWrite;

    gArchive = archive;
    gStorageRoot = storageRoot;
    gBundleRoot = bundleRoot;
    gInstalled = true;

    TJS::TJSCreateTextStreamForRead   = &CreateTextRead;
    TJS::TJSCreateTextStreamForWrite  = &CreateTextWrite;
    TJS::TJSCreateBinaryStreamForRead = &CreateBinaryRead;
    TJS::TJSCreateBinaryStreamForWrite = &CreateBinaryWrite;
}

void Uninstall() {
    if (gOrigTextRead)   TJS::TJSCreateTextStreamForRead   = gOrigTextRead;
    if (gOrigTextWrite)  TJS::TJSCreateTextStreamForWrite  = gOrigTextWrite;
    if (gOrigBinaryRead) TJS::TJSCreateBinaryStreamForRead = gOrigBinaryRead;
    if (gOrigBinaryWrite) TJS::TJSCreateBinaryStreamForWrite = gOrigBinaryWrite;
    gArchive = nullptr;
    gStorageRoot.clear();
    gBundleRoot.clear();
    gInstalled = false;
}

bool IsInstalled() { return gInstalled; }
unsigned int ReadCallCount()  { return gReadCount; }
unsigned int WriteCallCount() { return gWriteCount; }

}  // namespace host
}  // namespace galbox
