#include "XP3Archive.hpp"
#include "ByteUtils.hpp"
#include <algorithm>
#include <cctype>

namespace galbox {

// "XP3" + CR + LF + SPACE + LF + EOF + 0x8b 0x67 + version 0x01
// Note byte 5 is 0x20 (space), NOT 0x0d: taken from Kirikiroid2 XP3Mark1/2.
const uint8_t XP3Archive::kMark[11] = {
    0x58, 0x50, 0x33, 0x0d, 0x0a, 0x20, 0x0a, 0x1a, 0x8b, 0x67, 0x01
};

XP3Archive::~XP3Archive() { close(); }

bool XP3Archive::fail(std::string* err, const std::string& msg) {
    if (err) *err = msg;
    return false;
}

void XP3Archive::close() {
    if (file_.is_open()) file_.close();
    opened_ = false;
    entries_.clear();
    warnings_.clear();
    base_ = 0;
    fileSize_ = 0;
}

static std::string asciiLower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// Kirikiri built-in XP3 XOR (TVP standard scheme): the stored data is XORed
// with a keystream derived from the entry's adlr hash. The same operation
// encrypts and decrypts. Games using the custom CX/Yuz/Hx plugins still will
// not decode -- that is outside this subset (see native.html / compare.html).
static void xorDecrypt(std::vector<uint8_t>& d, uint32_t hash) {
    uint32_t h = hash;
    size_t i = 0;
    for (; i + 4 <= d.size(); i += 4) {
        uint32_t v = rd32(&d[i]) ^ h;
        wr32(&d[i], v);
        h *= 21311941u;
    }
    for (; i < d.size(); ++i)
        d[i] ^= (uint8_t)(h >> ((i & 3) * 8));
}

bool XP3Archive::readAt(uint64_t off, uint8_t* dst, size_t len) {
    if (!file_.is_open() || len == 0) return len == 0;
    if (off > fileSize_ || len > fileSize_ - off) return false;
    file_.clear();
    file_.seekg((std::streamoff)off, std::ios::beg);
    if (!file_.good()) return false;
    file_.read((char*)dst, (std::streamsize)len);
    return (size_t)file_.gcount() == len;
}

bool XP3Archive::locateBase(std::string* err) {
    uint8_t head[11] = {0};
    if (!readAt(0, head, 11)) return fail(err, "文件不足 11 字节，不是 XP3");

    if (head[0] == 0x4d && head[1] == 0x5a) {  // "MZ": self-extracting exe
        // Kirikiri searches the XP3 mark on 16-byte paragraph boundaries.
        const size_t kChunk = 256 * 1024;
        std::vector<uint8_t> buf(kChunk);
        bool found = false;
        for (uint64_t pos = 16; pos + 11 <= fileSize_; pos += kChunk) {
            size_t want = (size_t)std::min<uint64_t>(kChunk, fileSize_ - pos);
            if (!readAt(pos, &buf[0], want)) break;
            for (size_t p = 0; p + 11 <= want; p += 16) {
                if (std::memcmp(kMark, &buf[p], 11) == 0) {
                    base_ = pos + p;
                    found = true;
                    break;
                }
            }
            if (found) break;
        }
        if (!found) return fail(err, "MZ 可执行文件内未找到 XP3 标记");
        warnings_.push_back("自解压包：XP3 数据起始于偏移 " + std::to_string(base_));
        return true;
    }
    if (std::memcmp(kMark, head, 11) != 0)
        return fail(err, "XP3 魔数不匹配（前 11 字节错误）");
    base_ = 0;
    return true;
}

// Sequential chunk scan. `cursor` in = chunk head, out = payload start.
static bool findChunk(const std::vector<uint8_t>& d, const char* name,
                      size_t& cursor, size_t limit,
                      size_t& payload, size_t& payloadSize) {
    size_t p = cursor;
    while (p + 12 <= limit) {
        uint64_t sz = rd64(&d[p + 4]);
        if (sz > (uint64_t)(limit - p - 12)) return false;
        if (std::memcmp(&d[p], name, 4) == 0) {
            payload = p + 12;
            payloadSize = (size_t)sz;
            cursor = payload;
            return true;
        }
        p += 12 + (size_t)sz;
    }
    return false;
}

bool XP3Archive::loadIndexChain(std::string* err) {
    uint64_t cursor = base_ + 11;
    for (int chain = 0; chain < kMaxIndexChain; ++chain) {
        uint8_t offBuf[8] = {0};
        if (!readAt(cursor, offBuf, 8))
            return fail(err, "无法读取索引偏移");
        uint64_t indexAbs = base_ + rd64(offBuf);
        if (indexAbs + 1 > fileSize_)
            return fail(err, "索引偏移越界");

        uint8_t flag = 0;
        if (!readAt(indexAbs, &flag, 1)) return fail(err, "无法读取索引标志");
        uint64_t p = indexAbs + 1;
        std::vector<uint8_t> idx;

        int method = flag & 0x07;
        if (method == 0) {  // raw
            uint8_t b[8] = {0};
            if (!readAt(p, b, 8)) return fail(err, "无法读取索引长度");
            uint64_t sz = rd64(b);
            p += 8;
            if (sz > kMaxIndexSize || sz > fileSize_ - p)
                return fail(err, "索引长度越界");
            idx.resize((size_t)sz);
            if (!readAt(p, idx.empty() ? nullptr : &idx[0], idx.size()))
                return fail(err, "索引数据读取失败");
            p += sz;
        } else if (method == 1) {  // zlib
            uint8_t b[16] = {0};
            if (!readAt(p, b, 16)) return fail(err, "无法读取压缩索引头");
            uint64_t csz = rd64(b), rsz = rd64(b + 8);
            p += 16;
            if (rsz > kMaxIndexSize || csz > fileSize_ - p)
                return fail(err, "压缩索引长度越界");
            std::vector<uint8_t> comp((size_t)csz);
            if (!readAt(p, comp.empty() ? nullptr : &comp[0], comp.size()))
                return fail(err, "压缩索引读取失败");
            p += csz;
            idx.resize((size_t)rsz);
            if (!zlibInflate(comp, idx, err)) return false;
        } else {
            return fail(err, "未知索引编码方式 (flag=" + std::to_string(flag) + ")");
        }

        if (!parseIndex(idx, err)) return false;
        if (!(flag & 0x80)) return true;  // TVP_XP3_INDEX_CONTINUE not set
        cursor = p;                       // next uint64 index offset follows
        warnings_.push_back("多段索引链：继续读第 " + std::to_string(chain + 2) + " 段");
    }
    return fail(err, "索引链超过 " + std::to_string(kMaxIndexChain) + " 段，疑似损坏");
}

bool XP3Archive::parseIndex(const std::vector<uint8_t>& idx, std::string* err) {
    size_t cursor = 0, limit = idx.size();
    size_t filePayload = 0, fileSize = 0;

    while (findChunk(idx, "File", cursor, limit, filePayload, fileSize)) {
        size_t subLimit = filePayload + fileSize;
        if (subLimit > idx.size()) return fail(err, "File chunk 越界");

        // info: flags(4) OrgSize(8) ArcSize(8) nameLen(2) UTF-16LE name
        size_t p = filePayload, info = 0, infoSize = 0;
        if (!findChunk(idx, "info", p, subLimit, info, infoSize))
            return fail(err, "缺少 info 子块");
        if (infoSize < 22) return fail(err, "info 子块过短");
        uint32_t flags = rd32(&idx[info]);
        uint64_t orgSize = rd64(&idx[info + 4]);
        uint64_t arcSize = rd64(&idx[info + 12]);
        uint16_t nameLen = rd16(&idx[info + 20]);
        if ((size_t)22 + (size_t)nameLen * 2 > infoSize)
            return fail(err, "文件名长度超出 info 子块");

        XP3Entry e;
        e.name = utf16leToUtf8(&idx[info + 22], nameLen);
        e.lowerName = asciiLower(e.name);
        e.hash = 0;
        e.orgSize = orgSize;
        e.arcSize = arcSize;
        e.protectedFlag = (flags & 0x80000000u) != 0;  // TVP_XP3_FILE_PROTECTED

        // segm: N * 28 bytes. flags(4) start(8) orgSize(8) arcSize(8)
        p = filePayload;
        size_t segm = 0, segmSize = 0;
        if (!findChunk(idx, "segm", p, subLimit, segm, segmSize))
            return fail(err, "缺少 segm 子块");
        int n = (int)(segmSize / 28);
        uint64_t logical = 0;
        for (int i = 0; i < n; ++i) {
            const uint8_t* s = &idx[segm + (size_t)i * 28];
            XP3Segment seg;
            uint32_t sf = rd32(s);
            int sm = sf & 0x07;
            if (sm != 0 && sm != 1)
                return fail(err, "未知段编码方式 (" + std::to_string(sm) + ")");
            seg.compressed = (sm == 1);
            seg.start = base_ + rd64(s + 4);
            seg.logical = logical;
            seg.orgSize = rd64(s + 12);
            seg.arcSize = rd64(s + 20);
            if (seg.start > fileSize_ || seg.arcSize > fileSize_ - seg.start)
                return fail(err, "段数据偏移越界: " + e.name);
            logical += seg.orgSize;
            e.segments.push_back(seg);
        }

        // adlr: 4-byte file hash. Kirikiri requires it; we tolerate a missing one.
        p = filePayload;
        size_t adlr = 0, adlrSize = 0;
        if (findChunk(idx, "adlr", p, subLimit, adlr, adlrSize) && adlrSize >= 4)
            e.hash = rd32(&idx[adlr]);
        else
            warnings_.push_back("缺少 adlr 子块: " + e.name);

        if (e.protectedFlag)
            warnings_.push_back("受保护条目（可能需要专用解密滤镜）: " + e.name);

        entries_.push_back(e);
        cursor = filePayload + fileSize;  // next File chunk head
    }
    (void)err;
    return true;
}

bool XP3Archive::open(const std::string& path, std::string* err) {
    close();
    path_ = path;
    file_.open(path.c_str(), std::ios::binary);
    if (!file_.is_open()) return fail(err, "无法打开: " + path);
    file_.seekg(0, std::ios::end);
    std::streamoff end = file_.tellg();
    if (end < 11) return fail(err, "文件过小");
    fileSize_ = (uint64_t)end;
    file_.seekg(0, std::ios::beg);

    if (!locateBase(err)) return false;
    if (!loadIndexChain(err)) return false;
    opened_ = true;
    return true;
}

const XP3Entry* XP3Archive::find(const std::string& name) const {
    std::string key = asciiLower(name);
    for (const XP3Entry& e : entries_) {
        if (e.lowerName == key) return &e;
        // tolerate Windows separators used by some KAG scripts
        std::string alt = e.lowerName;
        std::replace(alt.begin(), alt.end(), '\\', '/');
        if (alt == key) return &e;
    }
    return nullptr;
}

bool XP3Archive::read(const std::string& name, std::vector<uint8_t>& out,
                      std::string* err) {
    out.clear();
    const XP3Entry* e = find(name);
    if (!e) return fail(err, "包内找不到: " + name);
    if (e->orgSize > (uint64_t)512 * 1024 * 1024)
        return fail(err, "文件过大 (>512MB)");
    out.reserve((size_t)e->orgSize);

    for (const XP3Segment& seg : e->segments) {
        std::vector<uint8_t> stored((size_t)seg.arcSize);
        if (!readAt(seg.start, stored.empty() ? nullptr : &stored[0], stored.size()))
            return fail(err, "段读取失败: " + name);
        if (e->protectedFlag) xorDecrypt(stored, e->hash);   // KRKR built-in XOR
        if (seg.compressed) {
            std::vector<uint8_t> plain((size_t)seg.orgSize);
            if (!zlibInflate(stored, plain, err)) return false;
            out.insert(out.end(), plain.begin(), plain.end());
        } else {
            out.insert(out.end(), stored.begin(), stored.end());
        }
    }
    if (out.size() != e->orgSize) {
        return fail(err, "解压后长度与索引不符: " + name + " (" +
                         std::to_string(out.size()) + " vs " +
                         std::to_string(e->orgSize) + ")");
    }
    return true;
}

bool XP3Archive::readAsText(const std::string& name, std::string& out,
                            std::string* err) {
    std::vector<uint8_t> bytes;
    if (!read(name, bytes, err)) return false;
    // Strip UTF-8 BOM if present; KAG scripts are commonly UTF-8 or Shift-JIS.
    size_t skip = 0;
    if (bytes.size() >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb && bytes[2] == 0xbf)
        skip = 3;
    out.assign((const char*)(bytes.data() + skip), bytes.size() - skip);
    return true;
}

}  // namespace galbox
