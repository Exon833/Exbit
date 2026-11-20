// Zip archives (see zip.h).
#include "importers/zip.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <set>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/util.h"
#include "gfx/image.h"
#include "importers/gzip.h"

namespace zip {

namespace {

constexpr uint32_t LOCAL = 0x04034b50, CENTRAL = 0x02014b50, END = 0x06054b50;
constexpr uint64_t MOST = 0xFFFFFFFFull;      // no zip64: sizes and offsets in 32 bits
constexpr size_t DEFLATE_UP_TO = 64u << 20;   // bigger files are stored (deflating them takes long, gains little)

void put16(std::string& s, uint32_t v) {
    s += (char)(v & 0xFF);
    s += (char)((v >> 8) & 0xFF);
}
void put32(std::string& s, uint32_t v) {
    put16(s, v & 0xFFFF);
    put16(s, v >> 16);
}
uint32_t get16(const uint8_t* p) { return p[0] | (uint32_t)p[1] << 8; }
uint32_t get32(const uint8_t* p) { return get16(p) | get16(p + 2) << 16; }

struct Item {
    std::string name;
    uint16_t method = 0, flags = 0;
    uint32_t crc = 0;
    uint64_t packed = 0, size = 0, offset = 0;
};

bool readAt(int fd, uint64_t at, size_t n, std::string& out) {
    out.resize(n);
    size_t got = 0;
    while (got < n) {
        ssize_t r = pread(fd, &out[got], n - got, (off_t)(at + got));
        if (r <= 0) return false;
        got += (size_t)r;
    }
    return true;
}

// the archive's central directory, every name checked
bool readDirectory(int fd, std::vector<Item>& items, std::string& error) {
    struct stat st;
    if (fstat(fd, &st) != 0) {
        error = strerror(errno);
        return false;
    }
    uint64_t total = (uint64_t)st.st_size;
    size_t tail = (size_t)std::min<uint64_t>(total, 22 + 65535);
    std::string end;
    if (tail < 22 || !readAt(fd, total - tail, tail, end)) {
        error = "This is not a zip archive.";
        return false;
    }
    const uint8_t* e = (const uint8_t*)end.data();
    size_t at = std::string::npos;
    for (size_t i = tail - 22 + 1; i-- > 0;)
        if (get32(e + i) == END) {
            at = i;
            break;
        }
    if (at == std::string::npos) {
        error = "This is not a zip archive (or its end is cut off).";
        return false;
    }
    uint32_t count = get16(e + at + 10), dirSize = get32(e + at + 12), dirAt = get32(e + at + 16);
    if (get16(e + at + 4) != 0 || get16(e + at + 6) != 0 || (uint64_t)dirAt + dirSize > total - tail + at) {
        error = "The archive is damaged (its list of files is not where it says).";
        return false;
    }
    std::string dir;
    if (!readAt(fd, dirAt, dirSize, dir)) {
        error = "The archive is damaged (its list of files can't be read).";
        return false;
    }
    const uint8_t* d = (const uint8_t*)dir.data();
    size_t p = 0;
    std::set<std::string> names;
    for (uint32_t k = 0; k < count; k++) {
        if (p + 46 > dir.size() || get32(d + p) != CENTRAL) {
            error = "The archive is damaged (its list of files is cut short).";
            return false;
        }
        Item it;
        it.flags = (uint16_t)get16(d + p + 8);
        it.method = (uint16_t)get16(d + p + 10);
        it.crc = get32(d + p + 16);
        it.packed = get32(d + p + 20);
        it.size = get32(d + p + 24);
        size_t nameLen = get16(d + p + 28), extra = get16(d + p + 30), comment = get16(d + p + 32);
        it.offset = get32(d + p + 42);
        if (p + 46 + nameLen + extra + comment > dir.size()) {
            error = "The archive is damaged (its list of files is cut short).";
            return false;
        }
        it.name.assign((const char*)d + p + 46, nameLen);
        p += 46 + nameLen + extra + comment;
        if (!safeName(it.name)) {
            error = "The archive holds a file that would land outside its folder, so nothing was unpacked: " + it.name;
            util::debugf("zip: unsafe name refused: %s\n", it.name.c_str());
            return false;
        }
        if (!names.insert(it.name).second) {
            error = "The archive holds the same file twice: " + it.name;
            return false;
        }
        if (it.flags & 1) {
            error = "The archive is encrypted: " + it.name;
            return false;
        }
        if (it.method != 0 && it.method != 8) {
            error = "The archive packs " + it.name + " in a way Tiny DAW can't read (method " + std::to_string(it.method) + ").";
            return false;
        }
        if (it.packed == MOST || it.size == MOST || it.offset == MOST) {
            error = "The archive is too big to read (zip64).";
            return false;
        }
        if (it.offset + 30 + nameLen + it.packed > total || (it.method == 0 && it.packed != it.size) ||
            it.size > it.packed * 1032 + 1024) {  // (deflate never packs more than 1032 to 1: a zip bomb's claim)
            error = "The archive is damaged (a file is cut short): " + it.name;
            return false;
        }
        items.push_back(it);
    }
    return true;
}

// the entry's contents, checked
bool readItem(int fd, const Item& it, std::string& out, std::string& error) {
    std::string head;
    if (!readAt(fd, it.offset, 30, head) || get32((const uint8_t*)head.data()) != LOCAL) {
        error = "The archive is damaged: " + it.name;
        return false;
    }
    uint64_t at = it.offset + 30 + get16((const uint8_t*)head.data() + 26) + get16((const uint8_t*)head.data() + 28);
    std::string packed;
    if (!readAt(fd, at, (size_t)it.packed, packed)) {
        error = "The archive is damaged (cut short): " + it.name;
        return false;
    }
    if (it.method == 0) {
        out = std::move(packed);
    } else {
        std::vector<uint8_t> raw;
        if (!gzip::inflateRaw((const uint8_t*)packed.data(), packed.size(), raw, nullptr, (size_t)it.size)) {
            error = "The archive is damaged (can't be unpacked): " + it.name;
            return false;
        }
        out.assign((const char*)raw.data(), raw.size());
    }
    if (out.size() != it.size || crc32((const uint8_t*)out.data(), out.size()) != it.crc) {
        error = "The archive is damaged (a file's check sum is wrong): " + it.name;
        return false;
    }
    return true;
}

bool writeAll(int fd, const std::string& s) {
    size_t done = 0;
    while (done < s.size()) {
        ssize_t w = ::write(fd, s.data() + done, s.size() - done);
        if (w <= 0) return false;
        done += (size_t)w;
    }
    return true;
}

}  // namespace

bool safeName(const std::string& name) {
    if (name.empty() || name[0] == '/' || name.size() > 4096) return false;
    for (unsigned char c : name)
        if (c < 0x20 || c == 0x7F || c == '\\') return false;
    std::string body = name.back() == '/' ? name.substr(0, name.size() - 1) : name;
    if (body.empty()) return false;
    for (const std::string& part : util::split(body, '/'))
        if (part.empty() || part == "." || part == "..") return false;
    return true;
}

bool write(const std::string& path, const std::vector<Entry>& entries, std::string& error,
           const std::function<void(int done, int total)>& progress) {
    if (entries.size() > 0xFFFF) {
        error = "Too many files for one archive (at most 65535).";
        return false;
    }
    std::string temp = path + ".part";
    int fd = open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        error = "Cannot write " + temp + ": " + strerror(errno);
        return false;
    }
    auto fail = [&](const std::string& why) {
        error = why;
        close(fd);
        unlink(temp.c_str());
        return false;
    };
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    uint32_t dosTime = (uint32_t)(t.tm_hour << 11 | t.tm_min << 5 | t.tm_sec / 2);
    uint32_t dosDate = (uint32_t)(std::max(0, t.tm_year - 80) << 9 | (t.tm_mon + 1) << 5 | t.tm_mday);
    std::string directory;
    uint64_t offset = 0;
    for (size_t i = 0; i < entries.size(); i++) {
        if (progress) progress((int)i, (int)entries.size());
        const Entry& e = entries[i];
        if (!safeName(e.name)) return fail("Not a name a zip archive may hold: " + e.name);
        std::string data;
        if (!util::readFile(e.file, data)) return fail("Cannot read " + e.file + ": " + strerror(errno));
        uint32_t crc = crc32((const uint8_t*)data.data(), data.size());
        uint16_t method = 0;
        std::string packed;
        if (data.size() >= 64 && data.size() <= DEFLATE_UP_TO) {
            std::vector<uint8_t> z = zlibDeflate((const uint8_t*)data.data(), data.size(), 6);  // (2-byte header, adler32)
            if (z.size() >= 6 && z.size() - 6 < data.size() - data.size() / 16) {  // (worth it: a sixteenth smaller)
                packed.assign((const char*)z.data() + 2, z.size() - 6);
                method = 8;
            }
        }
        const std::string& body = method ? packed : data;
        if (offset + 30 + e.name.size() + body.size() > MOST || data.size() > MOST)
            return fail("The archive would be bigger than 4 GB, which Tiny DAW can't write yet: use File → Collect and save.");
        std::string head;
        put32(head, LOCAL);
        put16(head, 20);
        put16(head, 0x0800);  // (names in UTF-8)
        put16(head, method);
        put16(head, dosTime);
        put16(head, dosDate);
        put32(head, crc);
        put32(head, (uint32_t)body.size());
        put32(head, (uint32_t)data.size());
        put16(head, (uint32_t)e.name.size());
        put16(head, 0);
        head += e.name;
        if (!writeAll(fd, head) || !writeAll(fd, body)) return fail("Cannot write " + temp + ": " + strerror(errno));
        put32(directory, CENTRAL);
        put16(directory, 0x031E);  // (made on Unix, version 3.0)
        put16(directory, 20);
        put16(directory, 0x0800);
        put16(directory, method);
        put16(directory, dosTime);
        put16(directory, dosDate);
        put32(directory, crc);
        put32(directory, (uint32_t)body.size());
        put32(directory, (uint32_t)data.size());
        put16(directory, (uint32_t)e.name.size());
        put16(directory, 0);
        put16(directory, 0);
        put16(directory, 0);
        put16(directory, 0);
        put32(directory, 0100644u << 16);  // (a plain file, rw-r--r--)
        put32(directory, (uint32_t)offset);
        directory += e.name;
        offset += head.size() + body.size();
    }
    if (offset + directory.size() > MOST) return fail("The archive would be bigger than 4 GB.");
    std::string end;
    put32(end, END);
    put16(end, 0);
    put16(end, 0);
    put16(end, (uint32_t)entries.size());
    put16(end, (uint32_t)entries.size());
    put32(end, (uint32_t)directory.size());
    put32(end, (uint32_t)offset);
    put16(end, 0);
    if (!writeAll(fd, directory) || !writeAll(fd, end) || fsync(fd) != 0)
        return fail("Cannot write " + temp + ": " + strerror(errno));
    close(fd);
    if (rename(temp.c_str(), path.c_str()) != 0) {
        error = "Cannot write " + path + ": " + strerror(errno);
        unlink(temp.c_str());
        return false;
    }
    if (progress) progress((int)entries.size(), (int)entries.size());
    util::debugf("zip: wrote %s: %zu file(s), %llu bytes\n", path.c_str(), entries.size(),
                 (unsigned long long)(offset + directory.size() + end.size()));
    return true;
}

bool list(const std::string& path, std::vector<std::string>& names, std::string& error) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        error = "Cannot read " + path + ": " + strerror(errno);
        return false;
    }
    std::vector<Item> items;
    bool ok = readDirectory(fd, items, error);
    close(fd);
    if (!ok) return false;
    names.clear();
    for (auto& it : items)
        if (it.name.back() != '/') names.push_back(it.name);
    return true;
}

bool extract(const std::string& path, const std::string& folder, std::vector<std::string>& files, std::string& error,
             const std::function<void(int done, int total)>& progress) {
    files.clear();
    if (util::fileExists(folder) || util::isDir(folder)) {
        error = "There is something called " + folder + " already.";
        return false;
    }
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        error = "Cannot read " + path + ": " + strerror(errno);
        return false;
    }
    std::vector<Item> items;
    if (!readDirectory(fd, items, error)) {
        close(fd);
        return false;
    }
    std::string temp = folder + ".part";
    if (util::fileExists(temp) || util::isDir(temp) || mkdir(temp.c_str(), 0755) != 0) {
        close(fd);
        error = "Cannot make the folder " + temp + (errno ? std::string(": ") + strerror(errno) : std::string());
        return false;
    }
    std::vector<std::string> made = {temp}, written;  // (only what this made is removed on failure)
    auto undo = [&] {
        for (auto& f : written) unlink(f.c_str());
        for (auto it = made.rbegin(); it != made.rend(); ++it) rmdir(it->c_str());
    };
    auto makeDir = [&](const std::string& dir) {  // (and the folders above it, under temp)
        std::string at = temp;
        for (const std::string& part : util::split(dir, '/')) {
            if (part.empty()) continue;
            at += "/" + part;
            if (util::isDir(at)) continue;
            if (mkdir(at.c_str(), 0755) != 0) return false;
            made.push_back(at);
        }
        return true;
    };
    bool ok = true;
    for (size_t i = 0; i < items.size() && ok; i++) {
        if (progress) progress((int)i, (int)items.size());
        const Item& it = items[i];
        if (it.name.back() == '/') {
            ok = makeDir(it.name);
            if (!ok) error = "Cannot make the folder " + it.name + ": " + strerror(errno);
            continue;
        }
        std::string data;
        if (!readItem(fd, it, data, error)) {
            ok = false;
            break;
        }
        if (!makeDir(util::dirName(it.name))) {
            error = "Cannot make the folder for " + it.name + ": " + strerror(errno);
            ok = false;
            break;
        }
        std::string out = temp + "/" + it.name;
        int o = open(out.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644);
        if (o < 0) {
            error = "Cannot write " + out + ": " + strerror(errno);
            ok = false;
            break;
        }
        written.push_back(out);
        if (!writeAll(o, data)) {
            error = "Cannot write " + out + ": " + strerror(errno);
            ok = false;
        }
        close(o);
    }
    close(fd);
    if (ok && rename(temp.c_str(), folder.c_str()) != 0) {
        error = "Cannot make the folder " + folder + ": " + strerror(errno);
        ok = false;
    }
    if (!ok) {
        undo();
        util::debugf("zip: %s not unpacked: %s\n", path.c_str(), error.c_str());
        return false;
    }
    for (auto& f : written) files.push_back(folder + f.substr(temp.size()));
    if (progress) progress((int)items.size(), (int)items.size());
    util::debugf("zip: unpacked %s into %s: %zu file(s)\n", path.c_str(), folder.c_str(), files.size());
    return true;
}

}  // namespace zip
