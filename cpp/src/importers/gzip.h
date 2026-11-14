// gzip files (RFC 1952) and raw deflate streams (RFC 1951): what an Ableton Live set (.als) is, gzip-
// compressed XML. Tiny DAW's own inflate (gfx/image.h's zlibInflate reads zlib streams only, and keeps
// to PNG's sizes; this one is quicker on the tens of MB a set can hold).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gzip {

// Inflate a raw deflate stream. `used`: the bytes of `data` it took (to find what follows it).
// False if it is malformed or cut short.
bool inflateRaw(const uint8_t* data, size_t size, std::vector<uint8_t>& out, size_t* used = nullptr);

// The contents of a gzip stream (several members one after the other are joined, as gzip does);
// checks each member's CRC-32 and length. False and `error` if it isn't gzip or is damaged.
bool decompress(const uint8_t* data, size_t size, std::string& out, std::string& error);
// the file at `path`, ungzipped
bool readFile(const std::string& path, std::string& out, std::string& error);
// does it start like a gzip stream (1f 8b)?
inline bool isGzip(const uint8_t* data, size_t size) { return size >= 2 && data[0] == 0x1f && data[1] == 0x8b; }

// `data` as a gzip stream (made with gfx's zlibDeflate): for tests, and anything writing .gz files
std::vector<uint8_t> compress(const uint8_t* data, size_t size, int level = 6);
inline std::vector<uint8_t> compress(const std::string& s, int level = 6) {
    return compress((const uint8_t*)s.data(), s.size(), level);
}

}  // namespace gzip
