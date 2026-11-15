// GalBox Native Engine - XP3 archive reader
// Byte layout follows Kirikiri2 / Kirikiroid2 src/core/base/XP3Archive.cpp:
//   header  11 bytes  "XP3\r\n \n\x1a\x8b\x67\x01"
//   +11     uint64 LE index offset (relative to archive base)
//   index   1 byte flag (0x07 encode mask: 0 raw / 1 zlib, 0x80 continue)
//   chunks  4-byte ASCII name + uint64 LE size + payload ("File"/"info"/"segm"/"adlr")
#pragma once
#include <cstdint>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace galbox {

struct XP3Segment {
    bool compressed = false;
    uint64_t start = 0;       // absolute offset in the .xp3 file
    uint64_t logical = 0;     // offset inside the in-archive file
    uint64_t orgSize = 0;     // uncompressed size
    uint64_t arcSize = 0;     // stored size
};

struct XP3Entry {
    std::string name;         // UTF-8
    std::string lowerName;    // case-folded ASCII for lookup
    uint32_t hash = 0;        // 'adlr' chunk value
    uint64_t orgSize = 0;
    uint64_t arcSize = 0;
    bool protectedFlag = false;
    std::vector<XP3Segment> segments;
};

class XP3Archive {
public:
    static const uint8_t kMark[11];
    static const size_t kMaxIndexSize = 64u * 1024u * 1024u;
    static const int kMaxIndexChain = 16;

    ~XP3Archive();
    bool open(const std::string& path, std::string* err);
    void close();
    bool isOpen() const { return opened_; }
    // True when `open` was given a folder and the archive is backed by loose
    // files on disk (Kirikiri unpacked "Data folder" form) rather than a .xp3.
    bool isDirectoryMode() const { return dirMode_; }

    size_t count() const { return entries_.size(); }
    const std::vector<XP3Entry>& entries() const { return entries_; }
    const XP3Entry* find(const std::string& name) const;
    bool read(const std::string& name, std::vector<uint8_t>& out, std::string* err);
    bool readAsText(const std::string& name, std::string& out, std::string* err);

    const std::string& path() const { return path_; }
    uint64_t baseOffset() const { return base_; }
    const std::vector<std::string>& warnings() const { return warnings_; }

private:
    bool readAt(uint64_t off, uint8_t* dst, size_t len);
    bool locateBase(std::string* err);
    bool loadIndexChain(std::string* err);
    bool parseIndex(const std::vector<uint8_t>& idx, std::string* err);
    bool fail(std::string* err, const std::string& msg);

    // Directory ("Data folder") support, mirroring Kirikiri's acceptance of a
    // folder as the game root. When a folder is opened we either delegate to the
    // embedded data.xp3 (preferred) or build a virtual archive from loose files.
    bool openDir(const std::string& dir, std::string* err);
    void collectDir(const std::string& dir, const std::string& rel, int depth);

    std::ifstream file_;
    std::string path_;
    uint64_t fileSize_ = 0;
    uint64_t base_ = 0;
    bool opened_ = false;
    std::vector<XP3Entry> entries_;
    std::vector<std::string> warnings_;

    bool dirMode_ = false;
    std::string dirRoot_;
    std::map<std::string, std::string> dirFiles_;   // lowerName -> absolute path
    std::map<std::string, std::string> dirBase_;     // lower basename -> absolute path
};

}  // namespace galbox
