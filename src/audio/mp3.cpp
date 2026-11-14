// MP3 decoder (Tiny DAW's own): MPEG-1, MPEG-2 LSF and MPEG-2.5 audio, Layers I, II and III.
//
// Written from the descriptions in ISO/IEC 11172-3 and 13818-3. The overall structure (flat
// scalefactor-band tables in decoding order, bit reservoir assembled into one buffer, stereo
// processing per band) follows the well-known public-domain decoder minimp3, but the code is our own.
//
// Supported: all three layers; mono, stereo, dual channel and joint stereo (MS and intensity
// stereo, MPEG-1 and MPEG-2 flavours); long, short and mixed blocks; the bit reservoir. ID3v2 tags
// at the start, ID3v1/APE tags at the end and garbage between frames are skipped (resync). A
// Xing/Info frame is not audio and is dropped; when it carries a LAME tag the encoder delay and
// padding are trimmed (as gapless decoders do: delay + 529 samples at the start). CRCs are ignored,
// free-format streams are not supported.
#include "audio/codecs.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------- bit reader, MSB first
// The buffer must have at least 8 readable (zero) bytes after `bytes`.
struct BitsBE {
    const uint8_t* d = nullptr;
    size_t bytes = 0;
    size_t pos = 0;  // in bits
    void init(const uint8_t* data, size_t n) { d = data; bytes = n; pos = 0; }
    uint32_t peek32() const {
        size_t b = pos >> 3;
        if (b >= bytes) return 0;
        uint64_t v;
        memcpy(&v, d + b, 8);
        v = __builtin_bswap64(v);
        return (uint32_t)((v << (pos & 7)) >> 32);
    }
    uint32_t get(int n) {
        if (n <= 0) return 0;
        uint32_t v = peek32() >> (32 - n);
        pos += n;
        return v;
    }
    uint32_t bit() { return get(1); }
};

// ---------------------------------------------------------------- Huffman tables (ISO 11172-3 B.7)
// Codes (as integers, MSB first) and lengths for the pair tables, row = x, column = y. Lengths do
// not include sign bits. Tables 16..23 share table 16's codes and 24..31 share table 24's, with
// linbits for values >= 15. All of them are complete prefix codes (checked by the tests).
const uint16_t kHc1[4] = {
    1, 1,
    1, 0};
const uint8_t kHl1[4] = {
    1, 3,
    2, 3};
const uint16_t kHc2[9] = {
    1, 2, 1,
    3, 1, 1,
    3, 2, 0};
const uint8_t kHl2[9] = {
    1, 3, 6,
    3, 3, 5,
    5, 5, 6};
const uint16_t kHc3[9] = {
    3, 2, 1,
    1, 1, 1,
    3, 2, 0};
const uint8_t kHl3[9] = {
    2, 2, 6,
    3, 2, 5,
    5, 5, 6};
const uint16_t kHc5[16] = {
    1, 2, 6, 5,
    3, 1, 4, 4,
    7, 5, 7, 1,
    6, 1, 1, 0};
const uint8_t kHl5[16] = {
    1, 3, 6, 7,
    3, 3, 6, 7,
    6, 6, 7, 8,
    7, 6, 7, 8};
const uint16_t kHc6[16] = {
    7, 3, 5, 1,
    6, 2, 3, 2,
    5, 4, 4, 1,
    3, 3, 2, 0};
const uint8_t kHl6[16] = {
    3, 3, 5, 7,
    3, 2, 4, 5,
    4, 4, 5, 6,
    6, 5, 6, 7};
const uint16_t kHc7[36] = {
    1, 2, 10, 19, 16, 10,
    3, 3, 7, 10, 5, 3,
    11, 4, 13, 17, 8, 4,
    12, 11, 18, 15, 11, 2,
    7, 6, 9, 14, 3, 1,
    6, 4, 5, 3, 2, 0};
const uint8_t kHl7[36] = {
    1, 3, 6, 8, 8, 9,
    3, 4, 6, 7, 7, 8,
    6, 5, 7, 8, 8, 9,
    7, 7, 8, 9, 9, 9,
    7, 7, 8, 9, 9, 10,
    8, 8, 9, 10, 10, 10};
const uint16_t kHc8[36] = {
    3, 4, 6, 18, 12, 5,
    5, 1, 2, 16, 9, 3,
    7, 3, 5, 14, 7, 3,
    19, 17, 15, 13, 10, 4,
    13, 5, 8, 11, 5, 1,
    12, 4, 4, 1, 1, 0};
const uint8_t kHl8[36] = {
    2, 3, 6, 8, 8, 9,
    3, 2, 4, 8, 8, 8,
    6, 4, 6, 8, 8, 9,
    8, 8, 8, 9, 9, 10,
    8, 7, 8, 9, 10, 10,
    9, 8, 9, 9, 11, 11};
const uint16_t kHc9[36] = {
    7, 5, 9, 14, 15, 7,
    6, 4, 5, 5, 6, 7,
    7, 6, 8, 8, 8, 5,
    15, 6, 9, 10, 5, 1,
    11, 7, 9, 6, 4, 1,
    14, 4, 6, 2, 6, 0};
const uint8_t kHl9[36] = {
    3, 3, 5, 6, 8, 9,
    3, 3, 4, 5, 6, 8,
    4, 4, 5, 6, 7, 8,
    6, 5, 6, 7, 7, 8,
    7, 6, 7, 7, 8, 9,
    8, 7, 8, 8, 9, 9};
const uint16_t kHc10[64] = {
    1, 2, 10, 23, 35, 30, 12, 17,
    3, 3, 8, 12, 18, 21, 12, 7,
    11, 9, 15, 21, 32, 40, 19, 6,
    14, 13, 22, 34, 46, 23, 18, 7,
    20, 19, 33, 47, 27, 22, 9, 3,
    31, 22, 41, 26, 21, 20, 5, 3,
    14, 13, 10, 11, 16, 6, 5, 1,
    9, 8, 7, 8, 4, 4, 2, 0};
const uint8_t kHl10[64] = {
    1, 3, 6, 8, 9, 9, 9, 10,
    3, 4, 6, 7, 8, 9, 8, 8,
    6, 6, 7, 8, 9, 10, 9, 9,
    7, 7, 8, 9, 10, 10, 9, 10,
    8, 8, 9, 10, 10, 10, 10, 10,
    9, 9, 10, 10, 11, 11, 10, 11,
    8, 8, 9, 10, 10, 10, 11, 11,
    9, 8, 9, 10, 10, 11, 11, 11};
const uint16_t kHc11[64] = {
    3, 4, 10, 24, 34, 33, 21, 15,
    5, 3, 4, 10, 32, 17, 11, 10,
    11, 7, 13, 18, 30, 31, 20, 5,
    25, 11, 19, 59, 27, 18, 12, 5,
    35, 33, 31, 58, 30, 16, 7, 5,
    28, 26, 32, 19, 17, 15, 8, 14,
    14, 12, 9, 13, 14, 9, 4, 1,
    11, 4, 6, 6, 6, 3, 2, 0};
const uint8_t kHl11[64] = {
    2, 3, 5, 7, 8, 9, 8, 9,
    3, 3, 4, 6, 8, 8, 7, 8,
    5, 5, 6, 7, 8, 9, 8, 8,
    7, 6, 7, 9, 8, 10, 8, 9,
    8, 8, 8, 9, 9, 10, 9, 10,
    8, 8, 9, 10, 10, 11, 10, 11,
    8, 7, 7, 8, 9, 10, 10, 10,
    8, 7, 8, 9, 10, 10, 10, 10};
const uint16_t kHc12[64] = {
    9, 6, 16, 33, 41, 39, 38, 26,
    7, 5, 6, 9, 23, 16, 26, 11,
    17, 7, 11, 14, 21, 30, 10, 7,
    17, 10, 15, 12, 18, 28, 14, 5,
    32, 13, 22, 19, 18, 16, 9, 5,
    40, 17, 31, 29, 17, 13, 4, 2,
    27, 12, 11, 15, 10, 7, 4, 1,
    27, 12, 8, 12, 6, 3, 1, 0};
const uint8_t kHl12[64] = {
    4, 3, 5, 7, 8, 9, 9, 9,
    3, 3, 4, 5, 7, 7, 8, 8,
    5, 4, 5, 6, 7, 8, 7, 8,
    6, 5, 6, 6, 7, 8, 8, 8,
    7, 6, 7, 7, 8, 8, 8, 9,
    8, 7, 8, 8, 8, 9, 8, 9,
    8, 7, 7, 8, 8, 9, 9, 10,
    9, 8, 8, 9, 9, 9, 9, 10};
const uint16_t kHc13[256] = {
    1, 5, 14, 21, 34, 51, 46, 71, 42, 52, 68, 52, 67, 44, 43, 19,
    3, 4, 12, 19, 31, 26, 44, 33, 31, 24, 32, 24, 31, 35, 22, 14,
    15, 13, 23, 36, 59, 49, 77, 65, 29, 40, 30, 40, 27, 33, 42, 16,
    22, 20, 37, 61, 56, 79, 73, 64, 43, 76, 56, 37, 26, 31, 25, 14,
    35, 16, 60, 57, 97, 75, 114, 91, 54, 73, 55, 41, 48, 53, 23, 24,
    58, 27, 50, 96, 76, 70, 93, 84, 77, 58, 79, 29, 74, 49, 41, 17,
    47, 45, 78, 74, 115, 94, 90, 79, 69, 83, 71, 50, 59, 38, 36, 15,
    72, 34, 56, 95, 92, 85, 91, 90, 86, 73, 77, 65, 51, 44, 43, 42,
    43, 20, 30, 44, 55, 78, 72, 87, 78, 61, 46, 54, 37, 30, 20, 16,
    53, 25, 41, 37, 44, 59, 54, 81, 66, 76, 57, 54, 37, 18, 39, 11,
    35, 33, 31, 57, 42, 82, 72, 80, 47, 58, 55, 21, 22, 26, 38, 22,
    53, 25, 23, 38, 70, 60, 51, 36, 55, 26, 34, 23, 27, 14, 9, 7,
    34, 32, 28, 39, 49, 75, 30, 52, 48, 40, 52, 28, 18, 17, 9, 5,
    45, 21, 34, 64, 56, 50, 49, 45, 31, 19, 12, 15, 10, 7, 6, 3,
    48, 23, 20, 39, 36, 35, 53, 21, 16, 23, 13, 10, 6, 1, 4, 2,
    16, 15, 17, 27, 25, 20, 29, 11, 17, 12, 16, 8, 1, 1, 0, 1};
const uint8_t kHl13[256] = {
    1, 4, 6, 7, 8, 9, 9, 10, 9, 10, 11, 11, 12, 12, 13, 13,
    3, 4, 6, 7, 8, 8, 9, 9, 9, 9, 10, 10, 11, 12, 12, 12,
    6, 6, 7, 8, 9, 9, 10, 10, 9, 10, 10, 11, 11, 12, 13, 13,
    7, 7, 8, 9, 9, 10, 10, 10, 10, 11, 11, 11, 11, 12, 13, 13,
    8, 7, 9, 9, 10, 10, 11, 11, 10, 11, 11, 12, 12, 13, 13, 14,
    9, 8, 9, 10, 10, 10, 11, 11, 11, 11, 12, 11, 13, 13, 14, 14,
    9, 9, 10, 10, 11, 11, 11, 11, 11, 12, 12, 12, 13, 13, 14, 14,
    10, 9, 10, 11, 11, 11, 12, 12, 12, 12, 13, 13, 13, 14, 16, 16,
    9, 8, 9, 10, 10, 11, 11, 12, 12, 12, 12, 13, 13, 14, 15, 15,
    10, 9, 10, 10, 11, 11, 11, 13, 12, 13, 13, 14, 14, 14, 16, 15,
    10, 10, 10, 11, 11, 12, 12, 13, 12, 13, 14, 13, 14, 15, 16, 17,
    11, 10, 10, 11, 12, 12, 12, 12, 13, 13, 13, 14, 15, 15, 15, 16,
    11, 11, 11, 12, 12, 13, 12, 13, 14, 14, 15, 15, 15, 16, 16, 16,
    12, 11, 12, 13, 13, 13, 14, 14, 14, 14, 14, 15, 16, 15, 16, 16,
    13, 12, 12, 13, 13, 13, 15, 14, 14, 17, 15, 15, 15, 17, 16, 16,
    12, 12, 13, 14, 14, 14, 15, 14, 15, 15, 16, 16, 19, 18, 19, 16};
const uint16_t kHc15[256] = {
    7, 12, 18, 53, 47, 76, 124, 108, 89, 123, 108, 119, 107, 81, 122, 63,
    13, 5, 16, 27, 46, 36, 61, 51, 42, 70, 52, 83, 65, 41, 59, 36,
    19, 17, 15, 24, 41, 34, 59, 48, 40, 64, 50, 78, 62, 80, 56, 33,
    29, 28, 25, 43, 39, 63, 55, 93, 76, 59, 93, 72, 54, 75, 50, 29,
    52, 22, 42, 40, 67, 57, 95, 79, 72, 57, 89, 69, 49, 66, 46, 27,
    77, 37, 35, 66, 58, 52, 91, 74, 62, 48, 79, 63, 90, 62, 40, 38,
    125, 32, 60, 56, 50, 92, 78, 65, 55, 87, 71, 51, 73, 51, 70, 30,
    109, 53, 49, 94, 88, 75, 66, 122, 91, 73, 56, 42, 64, 44, 21, 25,
    90, 43, 41, 77, 73, 63, 56, 92, 77, 66, 47, 67, 48, 53, 36, 20,
    71, 34, 67, 60, 58, 49, 88, 76, 67, 106, 71, 54, 38, 39, 23, 15,
    109, 53, 51, 47, 90, 82, 58, 57, 48, 72, 57, 41, 23, 27, 62, 9,
    86, 42, 40, 37, 70, 64, 52, 43, 70, 55, 42, 25, 29, 18, 11, 11,
    118, 68, 30, 55, 50, 46, 74, 65, 49, 39, 24, 16, 22, 13, 14, 7,
    91, 44, 39, 38, 34, 63, 52, 45, 31, 52, 28, 19, 14, 8, 9, 3,
    123, 60, 58, 53, 47, 43, 32, 22, 37, 24, 17, 12, 15, 10, 2, 1,
    71, 37, 34, 30, 28, 20, 17, 26, 21, 16, 10, 6, 8, 6, 2, 0};
const uint8_t kHl15[256] = {
    3, 4, 5, 7, 7, 8, 9, 9, 9, 10, 10, 11, 11, 11, 12, 13,
    4, 3, 5, 6, 7, 7, 8, 8, 8, 9, 9, 10, 10, 10, 11, 11,
    5, 5, 5, 6, 7, 7, 8, 8, 8, 9, 9, 10, 10, 11, 11, 11,
    6, 6, 6, 7, 7, 8, 8, 9, 9, 9, 10, 10, 10, 11, 11, 11,
    7, 6, 7, 7, 8, 8, 9, 9, 9, 9, 10, 10, 10, 11, 11, 11,
    8, 7, 7, 8, 8, 8, 9, 9, 9, 9, 10, 10, 11, 11, 11, 12,
    9, 7, 8, 8, 8, 9, 9, 9, 9, 10, 10, 10, 11, 11, 12, 12,
    9, 8, 8, 9, 9, 9, 9, 10, 10, 10, 10, 10, 11, 11, 11, 12,
    9, 8, 8, 9, 9, 9, 9, 10, 10, 10, 10, 11, 11, 12, 12, 12,
    9, 8, 9, 9, 9, 9, 10, 10, 10, 11, 11, 11, 11, 12, 12, 12,
    10, 9, 9, 9, 10, 10, 10, 10, 10, 11, 11, 11, 11, 12, 13, 12,
    10, 9, 9, 9, 10, 10, 10, 10, 11, 11, 11, 11, 12, 12, 12, 13,
    11, 10, 9, 10, 10, 10, 11, 11, 11, 11, 11, 11, 12, 12, 13, 13,
    11, 10, 10, 10, 10, 11, 11, 11, 11, 12, 12, 12, 12, 12, 13, 13,
    12, 11, 11, 11, 11, 11, 11, 11, 12, 12, 12, 12, 13, 13, 12, 13,
    12, 11, 11, 11, 11, 11, 11, 12, 12, 12, 12, 12, 13, 13, 13, 13};
const uint16_t kHc16[256] = {
    1, 5, 14, 44, 74, 63, 110, 93, 172, 149, 138, 242, 225, 195, 376, 17,
    3, 4, 12, 20, 35, 62, 53, 47, 83, 75, 68, 119, 201, 107, 207, 9,
    15, 13, 23, 38, 67, 58, 103, 90, 161, 72, 127, 117, 110, 209, 206, 16,
    45, 21, 39, 69, 64, 114, 99, 87, 158, 140, 252, 212, 199, 387, 365, 26,
    75, 36, 68, 65, 115, 101, 179, 164, 155, 264, 246, 226, 395, 382, 362, 9,
    66, 30, 59, 56, 102, 185, 173, 265, 142, 253, 232, 400, 388, 378, 445, 16,
    111, 54, 52, 100, 184, 178, 160, 133, 257, 244, 228, 217, 385, 366, 715, 10,
    98, 48, 91, 88, 165, 157, 148, 261, 248, 407, 397, 372, 380, 889, 884, 8,
    85, 84, 81, 159, 156, 143, 260, 249, 427, 401, 392, 383, 727, 713, 708, 7,
    154, 76, 73, 141, 131, 256, 245, 426, 406, 394, 384, 735, 359, 710, 352, 11,
    139, 129, 67, 125, 247, 233, 229, 219, 393, 743, 737, 720, 885, 882, 439, 4,
    243, 120, 118, 115, 227, 223, 396, 746, 742, 736, 721, 712, 706, 223, 436, 6,
    202, 224, 222, 218, 216, 389, 386, 381, 364, 888, 443, 707, 440, 437, 1728, 4,
    747, 211, 210, 208, 370, 379, 734, 723, 714, 1735, 883, 877, 876, 3459, 865, 2,
    377, 369, 102, 187, 726, 722, 358, 711, 709, 866, 1734, 871, 3458, 870, 434, 0,
    12, 10, 7, 11, 10, 17, 11, 9, 13, 12, 10, 7, 5, 3, 1, 3};
const uint8_t kHl16[256] = {
    1, 4, 6, 8, 9, 9, 10, 10, 11, 11, 11, 12, 12, 12, 13, 9,
    3, 4, 6, 7, 8, 9, 9, 9, 10, 10, 10, 11, 12, 11, 12, 8,
    6, 6, 7, 8, 9, 9, 10, 10, 11, 10, 11, 11, 11, 12, 12, 9,
    8, 7, 8, 9, 9, 10, 10, 10, 11, 11, 12, 12, 12, 13, 13, 10,
    9, 8, 9, 9, 10, 10, 11, 11, 11, 12, 12, 12, 13, 13, 13, 9,
    9, 8, 9, 9, 10, 11, 11, 12, 11, 12, 12, 13, 13, 13, 14, 10,
    10, 9, 9, 10, 11, 11, 11, 11, 12, 12, 12, 12, 13, 13, 14, 10,
    10, 9, 10, 10, 11, 11, 11, 12, 12, 13, 13, 13, 13, 15, 15, 10,
    10, 10, 10, 11, 11, 11, 12, 12, 13, 13, 13, 13, 14, 14, 14, 10,
    11, 10, 10, 11, 11, 12, 12, 13, 13, 13, 13, 14, 13, 14, 13, 11,
    11, 11, 10, 11, 12, 12, 12, 12, 13, 14, 14, 14, 15, 15, 14, 10,
    12, 11, 11, 11, 12, 12, 13, 14, 14, 14, 14, 14, 14, 13, 14, 11,
    12, 12, 12, 12, 12, 13, 13, 13, 13, 15, 14, 14, 14, 14, 16, 11,
    14, 12, 12, 12, 13, 13, 14, 14, 14, 16, 15, 15, 15, 17, 15, 11,
    13, 13, 11, 12, 14, 14, 13, 14, 14, 15, 16, 15, 17, 15, 14, 11,
    9, 8, 8, 9, 9, 10, 10, 10, 11, 11, 11, 11, 11, 11, 11, 8};
const uint16_t kHc24[256] = {
    15, 13, 46, 80, 146, 262, 248, 434, 426, 669, 653, 649, 621, 517, 1032, 88,
    14, 12, 21, 38, 71, 130, 122, 216, 209, 198, 327, 345, 319, 297, 279, 42,
    47, 22, 41, 74, 68, 128, 120, 221, 207, 194, 182, 340, 315, 295, 541, 18,
    81, 39, 75, 70, 134, 125, 116, 220, 204, 190, 178, 325, 311, 293, 271, 16,
    147, 72, 69, 135, 127, 118, 112, 210, 200, 188, 352, 323, 306, 285, 540, 14,
    263, 66, 129, 126, 119, 114, 214, 202, 192, 180, 341, 317, 301, 281, 262, 12,
    249, 123, 121, 117, 113, 215, 206, 195, 185, 347, 330, 308, 291, 272, 520, 10,
    435, 115, 111, 109, 211, 203, 196, 187, 353, 332, 313, 298, 283, 531, 381, 17,
    427, 212, 208, 205, 201, 193, 186, 177, 169, 320, 303, 286, 268, 514, 377, 16,
    335, 199, 197, 191, 189, 181, 174, 333, 321, 305, 289, 275, 521, 379, 371, 11,
    668, 184, 183, 179, 175, 344, 331, 314, 304, 290, 277, 530, 383, 373, 366, 10,
    652, 346, 171, 168, 164, 318, 309, 299, 287, 276, 263, 513, 375, 368, 362, 6,
    648, 322, 316, 312, 307, 302, 292, 284, 269, 261, 512, 376, 370, 364, 359, 4,
    620, 300, 296, 294, 288, 282, 273, 266, 515, 380, 374, 369, 365, 361, 357, 2,
    1033, 280, 278, 274, 267, 264, 259, 382, 378, 372, 367, 363, 360, 358, 356, 0,
    43, 20, 19, 17, 15, 13, 11, 9, 7, 6, 4, 7, 5, 3, 1, 3};
const uint8_t kHl24[256] = {
    4, 4, 6, 7, 8, 9, 9, 10, 10, 11, 11, 11, 11, 11, 12, 9,
    4, 4, 5, 6, 7, 8, 8, 9, 9, 9, 10, 10, 10, 10, 10, 8,
    6, 5, 6, 7, 7, 8, 8, 9, 9, 9, 9, 10, 10, 10, 11, 7,
    7, 6, 7, 7, 8, 8, 8, 9, 9, 9, 9, 10, 10, 10, 10, 7,
    8, 7, 7, 8, 8, 8, 8, 9, 9, 9, 10, 10, 10, 10, 11, 7,
    9, 7, 8, 8, 8, 8, 9, 9, 9, 9, 10, 10, 10, 10, 10, 7,
    9, 8, 8, 8, 8, 9, 9, 9, 9, 10, 10, 10, 10, 10, 11, 7,
    10, 8, 8, 8, 9, 9, 9, 9, 10, 10, 10, 10, 10, 11, 11, 8,
    10, 9, 9, 9, 9, 9, 9, 9, 9, 10, 10, 10, 10, 11, 11, 8,
    10, 9, 9, 9, 9, 9, 9, 10, 10, 10, 10, 10, 11, 11, 11, 8,
    11, 9, 9, 9, 9, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 8,
    11, 10, 9, 9, 9, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 8,
    11, 10, 10, 10, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 8,
    11, 10, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 11, 11, 8,
    12, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 11, 11, 11, 8,
    8, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 8, 8, 8, 8, 4};

const uint16_t kHcA[16] = {1, 5, 4, 5, 6, 5, 4, 4, 7, 3, 6, 0, 7, 2, 3, 1};  // count1 table A (vwxy)
const uint8_t kHlA[16] = {1, 4, 4, 5, 4, 6, 5, 6, 4, 5, 5, 6, 5, 6, 6, 6};

const int kLinbits[32] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                          1, 2, 3, 4, 6, 8, 10, 13, 4, 5, 6, 7, 8, 9, 11, 13};

constexpr int kFastBits = 10;

struct HuffTable {
    int dim = 0;
    const uint16_t* codes = nullptr;
    const uint8_t* lens = nullptr;
    uint16_t fast[1 << kFastBits];  // sym << 5 | len, 0 = longer code
    std::vector<uint32_t> keys;     // long codes, left-aligned, sorted
    std::vector<uint16_t> syms;
    std::vector<uint8_t> klens;

    void build(const uint16_t* c, const uint8_t* l, int n, int d, bool quad) {
        dim = d;
        codes = c;
        lens = l;
        memset(fast, 0, sizeof(fast));
        struct Long { uint32_t key; uint16_t sym; uint8_t len; };
        std::vector<Long> lng;
        for (int i = 0; i < n; ++i) {
            int len = l[i];
            uint16_t sym = quad ? (uint16_t)i : (uint16_t)(((i / d) << 4) | (i % d));
            if (len <= kFastBits) {
                uint32_t first = (uint32_t)c[i] << (kFastBits - len);
                for (uint32_t k = 0; k < (1u << (kFastBits - len)); ++k) fast[first + k] = (uint16_t)(sym << 5 | len);
            } else {
                lng.push_back({(uint32_t)c[i] << (32 - len), sym, (uint8_t)len});
            }
        }
        std::sort(lng.begin(), lng.end(), [](const Long& a, const Long& b) { return a.key < b.key; });
        for (auto& e : lng) {
            keys.push_back(e.key);
            syms.push_back(e.sym);
            klens.push_back(e.len);
        }
    }
    int decode(BitsBE& br) const {
        uint32_t p = br.peek32();
        uint16_t e = fast[p >> (32 - kFastBits)];
        if (e) {
            br.pos += e & 31;
            return e >> 5;
        }
        if (keys.empty()) {
            br.pos += 1;
            return 0;
        }
        size_t lo = 0, hi = keys.size();
        while (hi - lo > 1) {
            size_t mid = (lo + hi) / 2;
            if (keys[mid] <= p) lo = mid;
            else hi = mid;
        }
        br.pos += klens[lo];
        return syms[lo];
    }
};

struct HuffSet {
    HuffTable t[34];  // [table_select] for 0..31 (16..23 -> 16, 24..31 -> 24), 32 = count1 A
    const HuffTable* forSelect[32];
    HuffSet() {
        struct Src { int num; const uint16_t* c; const uint8_t* l; int dim; };
        const Src src[] = {{1, kHc1, kHl1, 2},     {2, kHc2, kHl2, 3},      {3, kHc3, kHl3, 3},
                           {5, kHc5, kHl5, 4},     {6, kHc6, kHl6, 4},      {7, kHc7, kHl7, 6},
                           {8, kHc8, kHl8, 6},     {9, kHc9, kHl9, 6},      {10, kHc10, kHl10, 8},
                           {11, kHc11, kHl11, 8},  {12, kHc12, kHl12, 8},   {13, kHc13, kHl13, 16},
                           {15, kHc15, kHl15, 16}, {16, kHc16, kHl16, 16},  {24, kHc24, kHl24, 16}};
        for (auto& s : src) t[s.num].build(s.c, s.l, s.dim * s.dim, s.dim, false);
        t[32].build(kHcA, kHlA, 16, 4, true);
        for (int i = 0; i < 32; ++i) {
            int k = i >= 24 ? 24 : i >= 16 ? 16 : i;
            forSelect[i] = (k == 0 || k == 4 || k == 14) ? nullptr : &t[k];
        }
    }
};

const HuffSet& huff() {
    static const HuffSet h;
    return h;
}

// ---------------------------------------------------------------- frame header
const int kBitrates[2][3][16] = {
    {{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0},
     {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0},
     {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0}},
    {{0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0},
     {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},
     {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0}}};
const int kSampleRates[3][3] = {{44100, 48000, 32000}, {22050, 24000, 16000}, {11025, 12000, 8000}};

struct Header {
    int version = 0;  // 0 MPEG-1, 1 MPEG-2, 2 MPEG-2.5
    int layer = 3;
    bool crc = false;
    int bitrate = 0;  // kbps
    int sampleRate = 0;
    int srIndex = 0;  // 0..8: 44.1, 48, 32, 22.05, 24, 16, 11.025, 12, 8 kHz
    int padding = 0;
    int mode = 0;     // 0 stereo, 1 joint stereo, 2 dual channel, 3 mono
    int modeExt = 0;
    int channels = 2;
    int frameBytes = 0;
    int samples = 0;  // per channel
    bool lsf() const { return version != 0; }
};

bool parseHeader(const uint8_t* p, Header& h) {
    if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return false;
    int vbits = (p[1] >> 3) & 3, lbits = (p[1] >> 1) & 3;
    int brIdx = p[2] >> 4, srIdx = (p[2] >> 2) & 3;
    if (vbits == 1 || lbits == 0 || brIdx == 0 || brIdx == 15 || srIdx == 3) return false;
    if ((p[3] & 3) == 2) return false;  // reserved emphasis: almost surely not a header
    h.version = vbits == 3 ? 0 : vbits == 2 ? 1 : 2;
    h.layer = 4 - lbits;
    h.crc = !(p[1] & 1);
    h.bitrate = kBitrates[h.lsf()][h.layer - 1][brIdx];
    h.sampleRate = kSampleRates[h.version][srIdx];
    h.srIndex = h.version * 3 + srIdx;
    h.padding = (p[2] >> 1) & 1;
    h.mode = p[3] >> 6;
    h.modeExt = (p[3] >> 4) & 3;
    h.channels = h.mode == 3 ? 1 : 2;
    if (h.layer == 1) {
        h.frameBytes = (12000 * h.bitrate / h.sampleRate + h.padding) * 4;
        h.samples = 384;
    } else if (h.layer == 2) {
        h.frameBytes = 144000 * h.bitrate / h.sampleRate + h.padding;
        h.samples = 1152;
    } else {
        h.frameBytes = (h.lsf() ? 72000 : 144000) * h.bitrate / h.sampleRate + h.padding;
        h.samples = h.lsf() ? 576 : 1152;
    }
    return h.frameBytes > 4;
}

bool compatible(const Header& a, const Header& b) {
    return a.version == b.version && a.layer == b.layer && a.srIndex == b.srIndex;
}

// ---------------------------------------------------------------- polyphase synthesis filterbank
// The prototype window of ISO 11172-3 table B.3 is D[i] = h[i] * (-1)^(i/64) / 65536 with h
// symmetric (h[512 - i] = h[i]); h[0..256] in units of 1/65536:
const int32_t kWindowHalf[257] = {
    0,      -1,     -1,     -1,     -1,     -1,     -1,     -2,     -2,     -2,     -2,     -3,     -3,
    -4,     -4,     -5,     -5,     -6,     -7,     -7,     -8,     -9,     -10,    -11,    -13,    -14,
    -16,    -17,    -19,    -21,    -24,    -26,    -29,    -31,    -35,    -38,    -41,    -45,    -49,
    -53,    -58,    -63,    -68,    -73,    -79,    -85,    -91,    -97,    -104,   -111,   -117,   -125,
    -132,   -139,   -147,   -154,   -161,   -169,   -176,   -183,   -190,   -196,   -202,   -208,   -213,
    -218,   -222,   -225,   -227,   -228,   -228,   -227,   -224,   -221,   -215,   -208,   -200,   -189,
    -177,   -163,   -146,   -127,   -106,   -83,    -57,    -29,    2,      36,     72,     111,    153,
    197,    244,    294,    347,    401,    459,    519,    581,    645,    711,    779,    848,    919,
    991,    1064,   1137,   1210,   1283,   1356,   1428,   1498,   1567,   1634,   1698,   1759,   1817,
    1870,   1919,   1962,   2001,   2032,   2057,   2075,   2085,   2087,   2080,   2063,   2037,   2000,
    1952,   1893,   1822,   1739,   1644,   1535,   1414,   1280,   1131,   970,    794,    605,    402,
    185,    -45,    -288,   -545,   -814,   -1095,  -1388,  -1692,  -2006,  -2330,  -2663,  -3004,  -3351,
    -3705,  -4063,  -4425,  -4788,  -5153,  -5517,  -5879,  -6237,  -6589,  -6935,  -7271,  -7597,  -7910,
    -8209,  -8491,  -8755,  -8998,  -9219,  -9416,  -9585,  -9727,  -9838,  -9916,  -9959,  -9966,  -9935,
    -9863,  -9750,  -9592,  -9389,  -9139,  -8840,  -8492,  -8092,  -7640,  -7134,  -6574,  -5959,  -5288,
    -4561,  -3776,  -2935,  -2037,  -1082,  -70,    998,    2122,   3300,   4533,   5818,   7154,   8540,
    9975,   11455,  12980,  14548,  16155,  17799,  19478,  21189,  22929,  24694,  26482,  28289,  30112,
    31947,  33791,  35640,  37489,  39336,  41176,  43006,  44821,  46617,  48390,  50137,  51853,  53534,
    55178,  56778,  58333,  59838,  61289,  62684,  64019,  65290,  66494,  67629,  68692,  69679,  70590,
    71420,  72169,  72835,  73415,  73908,  74313,  74630,  74856,  74992,  75038};

struct SynthTables {
    float D[512];
    float dctCoef[32];  // 1/(2 cos(pi (2n+1) / 2N)) for N = 32 (16), 16 (8), 8 (4), 4 (2), 2 (1)
    int coefOffset[6];
    SynthTables() {
        for (int i = 0; i < 512; ++i) {
            int k = i <= 256 ? i : 512 - i;
            int v = kWindowHalf[k];
            D[i] = (float)(v / 65536.0 * (((i >> 6) & 1) ? -1 : 1));
        }
        int off = 0;
        for (int N = 32, s = 0; N >= 2; N /= 2, ++s) {
            coefOffset[s] = off;
            for (int n = 0; n < N / 2; ++n) dctCoef[off + n] = (float)(1.0 / (2.0 * std::cos(kPi * (2 * n + 1) / (2.0 * N))));
            off += N / 2;
        }
    }
};

const SynthTables& synthTables() {
    static const SynthTables t;
    return t;
}

// DCT-II: out[k] = sum in[n] cos(pi (2n+1) k / 2N), by Lee's recursion
template <int N, int S>
struct Dct2 {
    static void run(const float* in, float* out, const float* coef) {
        float a[N / 2], b[N / 2], A[N / 2], B[N / 2];
        const float* c = coef + synthTables().coefOffset[S];
        for (int n = 0; n < N / 2; ++n) {
            float x0 = in[n], x1 = in[N - 1 - n];
            a[n] = x0 + x1;
            b[n] = (x0 - x1) * c[n];
        }
        Dct2<N / 2, S + 1>::run(a, A, coef);
        Dct2<N / 2, S + 1>::run(b, B, coef);
        for (int k = 0; k < N / 2 - 1; ++k) {
            out[2 * k] = A[k];
            out[2 * k + 1] = B[k] + B[k + 1];
        }
        out[N - 2] = A[N / 2 - 1];
        out[N - 1] = B[N / 2 - 1];
    }
};
template <int S>
struct Dct2<1, S> {
    static void run(const float* in, float* out, const float*) { out[0] = in[0]; }
};

struct Synth {
    float V[1024];
    int off = 0;
    Synth() { reset(); }
    void reset() {
        memset(V, 0, sizeof(V));
        off = 0;
    }
    // 32 subband samples in, 32 PCM samples out (written with `stride`)
    void run(const float* S, float* out, int stride) {
        const SynthTables& T = synthTables();
        float X[32];
        Dct2<32, 0>::run(S, X, T.dctCoef);
        off = (off - 64) & 1023;
        float* v = V + off;
        for (int i = 0; i < 16; ++i) v[i] = X[i + 16];
        v[16] = 0;
        for (int i = 17; i < 48; ++i) v[i] = -X[48 - i];
        for (int i = 48; i < 64; ++i) v[i] = -X[i - 48];
        float acc[32] = {0};
        for (int q = 0; q < 16; ++q) {
            const float* vv = V + ((off + 64 * q + 32 * (q & 1)) & 1023);
            const float* d = T.D + 32 * q;
            for (int j = 0; j < 32; ++j) acc[j] += d[j] * vv[j];
        }
        for (int j = 0; j < 32; ++j) out[j * stride] = acc[j];
    }
};

// ---------------------------------------------------------------- Layer III tables
// scalefactor band widths per sample rate index: long (22 bands), short (13 bands)
const uint8_t kLongWidths[9][22] = {
    {4, 4, 4, 4, 4, 4, 6, 6, 8, 8, 10, 12, 16, 20, 24, 28, 34, 42, 50, 54, 76, 158},
    {4, 4, 4, 4, 4, 4, 6, 6, 6, 8, 10, 12, 16, 18, 22, 28, 34, 40, 46, 54, 54, 192},
    {4, 4, 4, 4, 4, 4, 6, 6, 8, 10, 12, 16, 20, 24, 30, 38, 46, 56, 68, 84, 102, 26},
    {6, 6, 6, 6, 6, 6, 8, 10, 12, 14, 16, 20, 24, 28, 32, 38, 46, 52, 60, 68, 58, 54},
    {6, 6, 6, 6, 6, 6, 8, 10, 12, 14, 16, 18, 22, 26, 32, 38, 46, 54, 62, 70, 76, 36},
    {6, 6, 6, 6, 6, 6, 8, 10, 12, 14, 16, 20, 24, 28, 32, 38, 46, 52, 60, 68, 58, 54},
    {6, 6, 6, 6, 6, 6, 8, 10, 12, 14, 16, 20, 24, 28, 32, 38, 46, 52, 60, 68, 58, 54},
    {6, 6, 6, 6, 6, 6, 8, 10, 12, 14, 16, 20, 24, 28, 32, 38, 46, 52, 60, 68, 58, 54},
    {12, 12, 12, 12, 12, 12, 16, 20, 24, 28, 32, 40, 48, 56, 64, 76, 90, 2, 2, 2, 2, 2}};
const uint8_t kShortWidths[9][13] = {
    {4, 4, 4, 4, 6, 8, 10, 12, 14, 18, 22, 30, 56}, {4, 4, 4, 4, 6, 6, 10, 12, 14, 16, 20, 26, 66},
    {4, 4, 4, 4, 6, 8, 12, 16, 20, 26, 34, 42, 12}, {4, 4, 4, 6, 6, 8, 10, 14, 18, 26, 32, 42, 18},
    {4, 4, 4, 6, 8, 10, 12, 14, 18, 24, 32, 44, 12}, {4, 4, 4, 6, 8, 10, 12, 14, 18, 24, 30, 40, 18},
    {4, 4, 4, 6, 8, 10, 12, 14, 18, 24, 30, 40, 18}, {4, 4, 4, 6, 8, 10, 12, 14, 18, 24, 30, 40, 18},
    {8, 8, 8, 12, 16, 20, 24, 28, 36, 2, 2, 2, 26}};

const uint8_t kPretab[22] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 3, 2, 0};

// Flat band layout in decoding order: long bands, then short bands as (band, window) triples.
struct BandLayout {
    uint8_t w[40];
    int nLong = 0, nShort = 0;  // entries
    int n() const { return nLong + nShort; }
};

struct L3Tables {
    BandLayout layout[9][3];  // [srIndex][0 long, 1 short, 2 mixed]
    float pow43[8207];
    float win[4][36];          // block types 0, 1, 3 (index 2 = short window in [0..11])
    float imdctCos[18][18];    // [k][n] cos(pi/72 (2n+1)(2k+1))
    float shortCos[6][12];     // [k][i] cos(pi/24 (2i+7)(2k+1))
    float cs[8], ca[8];
    L3Tables() {
        for (int s = 0; s < 9; ++s) {
            BandLayout& L = layout[s][0];
            for (int b = 0; b < 22; ++b) L.w[L.nLong++] = kLongWidths[s][b];
            BandLayout& S = layout[s][1];
            for (int b = 0; b < 13; ++b)
                for (int k = 0; k < 3; ++k) S.w[S.nShort++] = kShortWidths[s][b];
            BandLayout& M = layout[s][2];
            int lines = 0;
            for (int b = 0; b < 22 && lines < 36; ++b) {
                M.w[M.nLong++] = kLongWidths[s][b];
                lines += kLongWidths[s][b];
            }
            int sl = 0;  // short line position (per window)
            for (int b = 0; b < 13; ++b) {
                int w0 = kShortWidths[s][b], start = sl;
                sl += w0;
                if (sl <= 12) continue;
                int width = start < 12 ? sl - 12 : w0;
                for (int k = 0; k < 3; ++k) M.w[M.nLong + M.nShort++] = (uint8_t)width;
            }
        }
        for (int i = 0; i < 8207; ++i) pow43[i] = (float)std::pow((double)i, 4.0 / 3.0);
        for (int i = 0; i < 36; ++i) {
            double longw = std::sin(kPi / 36 * (i + 0.5));
            win[0][i] = (float)longw;
            win[1][i] = (float)(i < 18 ? longw : i < 24 ? 1.0 : i < 30 ? std::sin(kPi / 12 * (i - 18 + 0.5)) : 0.0);
            win[3][i] = (float)(i < 6 ? 0.0 : i < 12 ? std::sin(kPi / 12 * (i - 6 + 0.5)) : i < 18 ? 1.0 : longw);
            win[2][i] = (float)(i < 12 ? std::sin(kPi / 12 * (i + 0.5)) : 0.0);
        }
        for (int k = 0; k < 18; ++k)
            for (int n = 0; n < 18; ++n) imdctCos[k][n] = (float)std::cos(kPi / 72 * (2 * n + 1) * (2 * k + 1));
        for (int k = 0; k < 6; ++k)
            for (int i = 0; i < 12; ++i) shortCos[k][i] = (float)std::cos(kPi / 24 * (2 * i + 7) * (2 * k + 1));
        const double c[8] = {-0.6, -0.535, -0.33, -0.185, -0.095, -0.041, -0.0142, -0.0037};
        for (int i = 0; i < 8; ++i) {
            double sq = std::sqrt(1.0 + c[i] * c[i]);
            cs[i] = (float)(1.0 / sq);
            ca[i] = (float)(c[i] / sq);
        }
    }
};

const L3Tables& l3() {
    static const L3Tables t;
    return t;
}

// 36-point IMDCT of 18 values (no window): y[i] = sum X[k] cos(pi/72 (2i+19)(2k+1)), via an 18-point
// DCT-IV and its symmetries
void imdct36(const float* X, float* y) {
    const L3Tables& T = l3();
    float Z[18] = {0};
    for (int k = 0; k < 18; ++k) {
        float x = X[k];
        if (x == 0) continue;
        const float* c = T.imdctCos[k];
        for (int n = 0; n < 18; ++n) Z[n] += x * c[n];
    }
    for (int i = 0; i < 9; ++i) y[i] = Z[i + 9];
    for (int i = 9; i < 27; ++i) y[i] = -Z[26 - i];
    for (int i = 27; i < 36; ++i) y[i] = -Z[i - 27];
}

// 12-point IMDCT of 6 values (no window)
void imdct12(const float* X, float* y) {
    const L3Tables& T = l3();
    for (int i = 0; i < 12; ++i) y[i] = 0;
    for (int k = 0; k < 6; ++k) {
        float x = X[k];
        if (x == 0) continue;
        for (int i = 0; i < 12; ++i) y[i] += x * T.shortCos[k][i];
    }
}

struct Granule {
    int part23 = 0, bigValues = 0, globalGain = 0, sfCompress = 0;
    int blockType = 0, mixed = 0, table[3] = {0, 0, 0}, subGain[3] = {0, 0, 0};
    int region0 = 0, region1 = 0, preflag = 0, sfScale = 0, count1Table = 0;
    const BandLayout* bands = nullptr;
};

struct SideInfo {
    int mainDataBegin = 0;
    int scfsi[2] = {0, 0};
    Granule gr[2][2];
};

const uint8_t kSlen[2][16] = {{0, 0, 0, 0, 3, 1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4}, {0, 1, 2, 3, 0, 1, 2, 3, 1, 2, 3, 1, 2, 3, 2, 3}};
// MPEG-2 LSF: number of scalefactors per partition [table][long, short, mixed][partition]
const uint8_t kLsfCounts[6][3][4] = {
    {{6, 5, 5, 5}, {9, 9, 9, 9}, {6, 9, 9, 9}},     {{6, 5, 7, 3}, {9, 9, 12, 6}, {6, 9, 12, 6}},
    {{11, 10, 0, 0}, {18, 18, 0, 0}, {15, 18, 0, 0}}, {{7, 7, 7, 0}, {12, 12, 12, 0}, {6, 15, 12, 0}},
    {{6, 6, 6, 3}, {12, 9, 9, 6}, {6, 12, 9, 6}},     {{8, 8, 5, 0}, {15, 12, 9, 0}, {6, 18, 9, 0}}};

struct ChannelState {
    float overlap[576];
    Synth synth;
    uint8_t scf[40];  // flat scalefactors (kept between granules for scfsi)
    uint8_t ist[40];  // intensity positions (255 = not intensity coded)
    void reset() {
        memset(overlap, 0, sizeof(overlap));
        synth.reset();
        memset(scf, 0, sizeof(scf));
        memset(ist, 0, sizeof(ist));
    }
};

// ---------------------------------------------------------------- the decoder
struct Mp3Decoder {
    ChannelState ch[2];
    std::vector<uint8_t> reservoir;
    std::vector<uint8_t> mainBuf;
    float xr[2][576];
    float sb[2][576];  // after IMDCT: [subband*18 + time]
    int nz[2] = {0, 0};
    int intensityScale = 0;

    Mp3Decoder() { reset(); }
    void reset() {
        ch[0].reset();
        ch[1].reset();
        reservoir.clear();
    }

    // ---------------- Layer III
    static bool readSideInfo(BitsBE& br, const Header& h, SideInfo& si) {
        int nch = h.channels;
        const L3Tables& T = l3();
        if (!h.lsf()) {
            si.mainDataBegin = br.get(9);
            br.get(nch == 1 ? 5 : 3);
            for (int c = 0; c < nch; ++c) si.scfsi[c] = br.get(4);
        } else {
            si.mainDataBegin = br.get(8);
            br.get(nch == 1 ? 1 : 2);
            si.scfsi[0] = si.scfsi[1] = 0;
        }
        int ngr = h.lsf() ? 1 : 2;
        for (int g = 0; g < ngr; ++g) {
            for (int c = 0; c < nch; ++c) {
                Granule& gr = si.gr[g][c];
                gr.part23 = br.get(12);
                gr.bigValues = br.get(9);
                if (gr.bigValues > 288) gr.bigValues = 288;
                gr.globalGain = br.get(8);
                gr.sfCompress = br.get(h.lsf() ? 9 : 4);
                if (br.bit()) {
                    gr.blockType = br.get(2);
                    gr.mixed = br.bit();
                    gr.table[0] = br.get(5);
                    gr.table[1] = br.get(5);
                    gr.table[2] = 0;
                    for (int w = 0; w < 3; ++w) gr.subGain[w] = br.get(3);
                    if (gr.blockType == 0) return false;  // forbidden
                    gr.region0 = (gr.blockType == 2 && !gr.mixed) ? 8 : 7;
                    gr.region1 = 36;  // region 2 is empty
                } else {
                    gr.blockType = 0;
                    gr.mixed = 0;
                    for (int r = 0; r < 3; ++r) gr.table[r] = br.get(5);
                    gr.subGain[0] = gr.subGain[1] = gr.subGain[2] = 0;
                    gr.region0 = br.get(4);
                    gr.region1 = br.get(3);
                }
                gr.preflag = h.lsf() ? 0 : br.bit();
                gr.sfScale = br.bit();
                gr.count1Table = br.bit();
                int kind = gr.blockType == 2 ? (gr.mixed ? 2 : 1) : 0;
                gr.bands = &T.layout[h.srIndex][kind];
            }
        }
        return true;
    }

    void readScalefactors(BitsBE& br, const Header& h, Granule& gr, int c, int g, int scfsi) {
        ChannelState& cs = ch[c];
        const BandLayout& B = *gr.bands;
        int counts[4] = {0, 0, 0, 0}, slen[4] = {0, 0, 0, 0};
        bool markIllegal = false;
        if (!h.lsf()) {
            int s1 = kSlen[0][gr.sfCompress], s2 = kSlen[1][gr.sfCompress];
            if (gr.blockType == 2) {
                if (gr.mixed) {
                    counts[0] = B.nLong;
                    counts[1] = 9;
                } else {
                    counts[0] = 9;
                    counts[1] = 9;
                }
                counts[2] = 6;
                counts[3] = 12;
                scfsi = 0;
            } else {
                counts[0] = 6;
                counts[1] = 5;
                counts[2] = 5;
                counts[3] = 5;
                if (g == 0) scfsi = 0;
            }
            slen[0] = slen[1] = s1;
            slen[2] = slen[3] = s2;
        } else {
            int kind = gr.blockType == 2 ? (gr.mixed ? 2 : 1) : 0;
            int sfc = gr.sfCompress, tab;
            bool isRight = c == 1 && h.mode == 1 && (h.modeExt & 1);
            if (!isRight) {
                if (sfc < 400) {
                    slen[0] = (sfc >> 4) / 5; slen[1] = (sfc >> 4) % 5; slen[2] = (sfc & 15) >> 2; slen[3] = sfc & 3;
                    tab = 0;
                } else if (sfc < 500) {
                    sfc -= 400;
                    slen[0] = (sfc >> 2) / 5; slen[1] = (sfc >> 2) % 5; slen[2] = sfc & 3; slen[3] = 0;
                    tab = 1;
                } else {
                    sfc -= 500;
                    slen[0] = sfc / 3; slen[1] = sfc % 3; slen[2] = 0; slen[3] = 0;
                    tab = 2;
                    gr.preflag = 1;
                }
            } else {
                intensityScale = sfc & 1;
                int isc = sfc >> 1;
                if (isc < 180) {
                    slen[0] = isc / 36; slen[1] = (isc % 36) / 6; slen[2] = (isc % 36) % 6; slen[3] = 0;
                    tab = 3;
                } else if (isc < 244) {
                    isc -= 180;
                    slen[0] = (isc & 63) >> 4; slen[1] = (isc & 15) >> 2; slen[2] = isc & 3; slen[3] = 0;
                    tab = 4;
                } else {
                    isc -= 244;
                    slen[0] = isc / 3; slen[1] = isc % 3; slen[2] = 0; slen[3] = 0;
                    tab = 5;
                }
                markIllegal = true;
            }
            for (int p = 0; p < 4; ++p) counts[p] = kLsfCounts[tab][kind][p];
            scfsi = 0;
        }
        int idx = 0;
        for (int p = 0; p < 4; ++p) {
            int n = counts[p];
            if (idx + n > 39) n = std::max(0, 39 - idx);
            if (scfsi & (8 >> p)) {  // copied from granule 0
                idx += n;
                continue;
            }
            int bits = slen[p];
            for (int k = 0; k < n; ++k, ++idx) {
                int s = bits ? (int)br.get(bits) : 0;
                cs.scf[idx] = (uint8_t)s;
                cs.ist[idx] = (markIllegal && bits && s == (1 << bits) - 1) ? 255 : (uint8_t)s;
            }
        }
        for (; idx < 40; ++idx) cs.scf[idx] = cs.ist[idx] = 0;
    }

    // Huffman data into integers; returns the number of lines that may be nonzero
    static int readHuffman(BitsBE& br, size_t endBit, const Granule& gr, int* is) {
        const HuffSet& H = huff();
        const BandLayout& B = *gr.bands;
        auto regionEnd = [&](int count) {
            int s = 0;
            for (int i = 0; i < count && i < B.n(); ++i) s += B.w[i];
            return s;
        };
        int big = gr.bigValues * 2;
        int r1 = std::min(regionEnd(gr.region0 + 1), big);
        int r2 = std::min(regionEnd(gr.region0 + gr.region1 + 2), big);
        int bounds[3] = {r1, r2, big};
        int i = 0;
        for (int r = 0; r < 3; ++r) {
            int sel = gr.table[r];
            const HuffTable* t = H.forSelect[sel];
            int lin = kLinbits[sel];
            int stop = bounds[r];
            if (!t) {
                for (; i < stop; ++i) is[i] = 0;
                continue;
            }
            for (; i < stop; i += 2) {
                int sym = t->decode(br);
                int x = sym >> 4, y = sym & 15;
                if (lin && x == 15) x += br.get(lin);
                if (x && br.bit()) x = -x;
                if (lin && y == 15) y += br.get(lin);
                if (y && br.bit()) y = -y;
                is[i] = x;
                is[i + 1] = y;
            }
        }
        // count1 region
        const HuffTable& A = H.t[32];
        while (i + 4 <= 576 && br.pos < endBit) {
            int q = gr.count1Table ? (int)(br.get(4) ^ 15) : A.decode(br);
            int v[4] = {(q >> 3) & 1, (q >> 2) & 1, (q >> 1) & 1, q & 1};
            for (int k = 0; k < 4; ++k)
                if (v[k] && br.bit()) v[k] = -1;
            if (br.pos > endBit) break;  // the last quadruple overran: discard it
            for (int k = 0; k < 4; ++k) is[i + k] = v[k];
            i += 4;
        }
        int n = i;
        for (; i < 576; ++i) is[i] = 0;
        return n;
    }

    void requantize(const Granule& gr, const ChannelState& cs, const int* is, int n, float* out) {
        const L3Tables& T = l3();
        const BandLayout& B = *gr.bands;
        int shift = 1 + gr.sfScale;
        int idx = 0;
        for (int b = 0; b < B.n() && idx < n; ++b) {
            int w = B.w[b];
            int q;
            if (b < B.nLong) {
                q = gr.globalGain - 210 - ((cs.scf[b] + (gr.preflag ? kPretab[b] : 0)) << shift);
            } else {
                int win = (b - B.nLong) % 3;
                q = gr.globalGain - 210 - 8 * gr.subGain[win] - (cs.scf[b] << shift);
            }
            float g = std::exp2(q * 0.25f);
            for (int k = 0; k < w && idx < 576; ++k, ++idx) {
                int v = is[idx];
                if (v == 0) {
                    out[idx] = 0;
                } else {
                    int a = v < 0 ? -v : v;
                    float m = a < 8207 ? T.pow43[a] : (float)std::pow((double)a, 4.0 / 3.0);
                    out[idx] = v < 0 ? -m * g : m * g;
                }
            }
        }
        for (; idx < 576; ++idx) out[idx] = 0;
    }

    void stereo(const Header& h, const Granule& gr) {
        bool ms = h.mode == 1 && (h.modeExt & 2);
        bool intensity = h.mode == 1 && (h.modeExt & 1);
        float* L = xr[0];
        float* R = xr[1];
        const float r2 = 0.70710678118654752f;
        if (!intensity) {
            if (ms) {
                int n = std::max(nz[0], nz[1]);
                for (int i = 0; i < n; ++i) {
                    float m = L[i], s = R[i];
                    L[i] = (m + s) * r2;
                    R[i] = (m - s) * r2;
                }
                nz[0] = nz[1] = n;
            }
            return;
        }
        const BandLayout& B = *gr.bands;
        int nb = B.n();
        // highest band (per window for short blocks) where the right channel has data
        int maxBand[3] = {-1, -1, -1};
        int pos = 0;
        for (int b = 0; b < nb; ++b) {
            for (int k = 0; k < B.w[b]; ++k)
                if (R[pos + k] != 0) {
                    maxBand[b % 3] = b;
                    break;
                }
            pos += B.w[b];
        }
        int maxBlocks = B.nShort ? 3 : 1;
        if (B.nLong) maxBand[0] = maxBand[1] = maxBand[2] = std::max(std::max(maxBand[0], maxBand[1]), maxBand[2]);
        uint8_t* ist = ch[1].ist;
        for (int i = 0; i < maxBlocks; ++i) {  // the top band(s) have no scalefactor: use the band below
            int top = nb - maxBlocks + i, prev = top - maxBlocks;
            ist[top] = maxBand[i] >= prev ? (h.lsf() ? 0 : 3) : ist[prev];
        }
        unsigned maxPos = h.lsf() ? 64 : 7;
        pos = 0;
        for (int b = 0; b < nb; ++b) {
            int w = B.w[b];
            unsigned ip = ist[b];
            int mb = maxBlocks == 1 ? maxBand[0] : maxBand[b % 3];
            if (b > mb && ip < maxPos) {
                float kl, kr;
                if (!h.lsf()) {
                    double ratio = std::tan(ip * kPi / 12);
                    if (ip == 6) {
                        kl = 1;
                        kr = 0;
                    } else {
                        kl = (float)(ratio / (1 + ratio));
                        kr = (float)(1 / (1 + ratio));
                    }
                } else {
                    float k = std::exp2(-0.25f * (float)(((ip + 1) >> 1) << intensityScale));
                    if (ip & 1) {
                        kl = k;
                        kr = 1;
                    } else {
                        kl = 1;
                        kr = k;
                    }
                }
                for (int k = 0; k < w; ++k) {
                    float v = L[pos + k];
                    L[pos + k] = v * kl;
                    R[pos + k] = v * kr;
                }
            } else if (ms) {
                for (int k = 0; k < w; ++k) {
                    float m = L[pos + k], s = R[pos + k];
                    L[pos + k] = (m + s) * r2;
                    R[pos + k] = (m - s) * r2;
                }
            }
            pos += w;
        }
        nz[0] = nz[1] = 576;
    }

    // reorder, alias reduction, IMDCT, overlap-add and frequency inversion of one channel
    void hybrid(const Granule& gr, int c) {
        const L3Tables& T = l3();
        float* x = xr[c];
        ChannelState& cs = ch[c];
        int longLines = gr.blockType != 2 ? 576 : gr.mixed ? 36 : 0;
        if (gr.blockType == 2) {
            float tmp[576];
            const BandLayout& B = *gr.bands;
            int src = longLines, dst = 0;
            for (int b = B.nLong; b + 2 < B.n(); b += 3) {
                int w = B.w[b];
                for (int k = 0; k < w; ++k) {
                    tmp[dst++] = x[src + k];
                    tmp[dst++] = x[src + w + k];
                    tmp[dst++] = x[src + 2 * w + k];
                }
                src += 3 * w;
            }
            memcpy(x + longLines, tmp, dst * sizeof(float));
        }
        int nzSub = gr.blockType == 2 ? 32 : std::min(32, (nz[c] + 17) / 18);
        // alias reduction between long-block subbands
        int aaBounds = gr.blockType != 2 ? std::min(31, nzSub) : gr.mixed ? 1 : 0;
        for (int s = 0; s < aaBounds; ++s) {
            float* a = x + 18 * s + 17;
            float* b = x + 18 * (s + 1);
            for (int i = 0; i < 8; ++i) {
                float u = a[-i], d = b[i];
                a[-i] = u * T.cs[i] - d * T.ca[i];
                b[i] = d * T.cs[i] + u * T.ca[i];
            }
        }
        int active = std::min(32, aaBounds + 1 > nzSub ? aaBounds + 1 : nzSub);
        if (gr.blockType == 2) active = 32;
        float* out = sb[c];
        for (int s = 0; s < 32; ++s) {
            float* ov = cs.overlap + 18 * s;
            float* o = out + 18 * s;
            if (s >= active) {
                for (int i = 0; i < 18; ++i) {
                    o[i] = ov[i];
                    ov[i] = 0;
                }
                continue;
            }
            const float* in = x + 18 * s;
            float y[36];
            bool isLong = 18 * s < longLines;
            if (isLong) {
                imdct36(in, y);
                const float* w = T.win[gr.blockType == 2 ? 0 : gr.blockType];
                for (int i = 0; i < 36; ++i) y[i] *= w[i];
            } else {
                for (int i = 0; i < 36; ++i) y[i] = 0;
                for (int win = 0; win < 3; ++win) {
                    float X[6], z[12];
                    for (int k = 0; k < 6; ++k) X[k] = in[3 * k + win];
                    imdct12(X, z);
                    for (int i = 0; i < 12; ++i) y[6 + 6 * win + i] += z[i] * T.win[2][i];
                }
            }
            for (int i = 0; i < 18; ++i) {
                o[i] = y[i] + ov[i];
                ov[i] = y[18 + i];
            }
        }
        for (int s = 1; s < 32; s += 2)
            for (int i = 1; i < 18; i += 2) out[18 * s + i] = -out[18 * s + i];
    }

    // decodes one Layer III frame into pcm (interleaved, h.channels); returns false if broken
    void layer3(const uint8_t* frame, const Header& h, float* pcm) {
        int nch = h.channels;
        int sideBytes = h.lsf() ? (nch == 1 ? 9 : 17) : (nch == 1 ? 17 : 32);
        int hdrBytes = 4 + (h.crc ? 2 : 0);
        int mainBytes = h.frameBytes - hdrBytes - sideBytes;
        uint8_t sidebuf[48] = {0};
        SideInfo si;
        bool ok = mainBytes >= 0;
        if (ok) {
            memcpy(sidebuf, frame + hdrBytes, sideBytes);
            BitsBE br;
            br.init(sidebuf, sideBytes);
            ok = readSideInfo(br, h, si);
        }
        if (mainBytes < 0) mainBytes = 0;
        const uint8_t* mainData = frame + hdrBytes + sideBytes;
        // assemble reservoir + this frame's main data
        bool haveData = ok && si.mainDataBegin <= (int)reservoir.size();
        size_t total = 0;
        if (haveData) {
            total = si.mainDataBegin + mainBytes;
            mainBuf.assign(total + 16, 0);
            if (si.mainDataBegin) memcpy(mainBuf.data(), reservoir.data() + reservoir.size() - si.mainDataBegin, si.mainDataBegin);
            if (mainBytes) memcpy(mainBuf.data() + si.mainDataBegin, mainData, mainBytes);
        }
        reservoir.insert(reservoir.end(), mainData, mainData + mainBytes);
        if (reservoir.size() > 4096) reservoir.erase(reservoir.begin(), reservoir.end() - 2048);

        BitsBE br;
        if (haveData) br.init(mainBuf.data(), total);
        size_t bitPos = 0;
        int ngr = h.lsf() ? 1 : 2;
        int is[576];
        for (int g = 0; g < ngr; ++g) {
            for (int c = 0; c < nch; ++c) {
                Granule& gr = si.gr[g][c];
                if (!haveData) {
                    memset(xr[c], 0, sizeof(xr[c]));
                    nz[c] = 0;
                    continue;
                }
                br.pos = bitPos;
                size_t end = bitPos + gr.part23;
                bitPos = end;
                if (end > total * 8) {  // broken: silence
                    memset(xr[c], 0, sizeof(xr[c]));
                    nz[c] = 0;
                    continue;
                }
                readScalefactors(br, h, gr, c, g, si.scfsi[c]);
                nz[c] = readHuffman(br, end, gr, is);
                requantize(gr, ch[c], is, nz[c], xr[c]);
            }
            if (nch == 2 && haveData) stereo(h, si.gr[g][0]);
            for (int c = 0; c < nch; ++c) {
                Granule& gr = si.gr[g][c];
                if (!haveData) {
                    gr.blockType = 0;
                    gr.mixed = 0;
                    gr.bands = &l3().layout[h.srIndex][0];
                }
                hybrid(gr, c);
                float* o = pcm + (size_t)g * 576 * nch + c;
                for (int t = 0; t < 18; ++t) {
                    float S[32];
                    for (int s = 0; s < 32; ++s) S[s] = sb[c][18 * s + t];
                    ch[c].synth.run(S, o + (size_t)t * 32 * nch, nch);
                }
            }
        }
    }

    // ---------------- Layers I and II
    void layer12(const uint8_t* frame, const Header& h, float* pcm) {
        int nch = h.channels;
        int hdrBytes = 4 + (h.crc ? 2 : 0);
        int bytes = h.frameBytes - hdrBytes;
        std::vector<uint8_t> buf(std::max(bytes, 0) + 16, 0);
        if (bytes > 0) memcpy(buf.data(), frame + hdrBytes, bytes);
        BitsBE br;
        br.init(buf.data(), std::max(bytes, 0));
        static float scale[64];
        static bool scaleInit = [] {
            for (int i = 0; i < 63; ++i) scale[i] = (float)(2.0 * std::pow(2.0, -i / 3.0));
            scale[63] = 0;
            return true;
        }();
        (void)scaleInit;
        int alloc[2][32] = {{0}};
        int levels[2][32] = {{0}};
        int scf[2][32][3] = {{{0}}};
        int bound = h.mode == 1 ? 4 * (h.modeExt + 1) : 32;
        if (nch == 1) bound = 32;
        float samples[2][36][32];
        memset(samples, 0, sizeof(samples));
        if (h.layer == 1) {
            for (int s = 0; s < 32; ++s)
                for (int c = 0; c < (s < bound ? nch : 1); ++c) {
                    int a = br.get(4);
                    if (a == 15) a = 0;
                    alloc[c][s] = a;
                    if (s >= bound) alloc[1][s] = a;
                }
            for (int s = 0; s < 32; ++s)
                for (int c = 0; c < nch; ++c)
                    if (alloc[c][s]) scf[c][s][0] = br.get(6);
            for (int t = 0; t < 12; ++t)
                for (int s = 0; s < 32; ++s)
                    for (int c = 0; c < (s < bound ? nch : 1); ++c) {
                        int a = alloc[c][s];
                        if (!a) continue;
                        int nb = a + 1;
                        int L = (1 << nb) - 1;
                        int code = br.get(nb);
                        float v = (float)(2 * code - (L - 1)) / L;
                        if (s < bound) {
                            samples[c][t][s] = v * scale[scf[c][s][0]];
                        } else {
                            for (int cc = 0; cc < nch; ++cc) samples[cc][t][s] = v * scale[scf[cc][s][0]];
                        }
                    }
            for (int c = 0; c < nch; ++c)
                for (int t = 0; t < 12; ++t) ch[c].synth.run(samples[c][t], pcm + (size_t)t * 32 * nch + c, nch);
            return;
        }
        // Layer II allocation tables
        static const int kA[16] = {0, 3, 7, 15, 31, 63, 127, 255, 511, 1023, 2047, 4095, 8191, 16383, 32767, 65535};
        static const int kB[16] = {0, 3, 5, 7, 9, 15, 31, 63, 127, 255, 511, 1023, 2047, 4095, 8191, 65535};
        static const int kC[8] = {0, 3, 5, 7, 9, 15, 31, 65535};
        static const int kD[4] = {0, 3, 5, 65535};
        static const int kE[16] = {0, 3, 5, 9, 15, 31, 63, 127, 255, 511, 1023, 2047, 4095, 8191, 16383, 32767};
        static const int kF[16] = {0, 3, 5, 7, 9, 15, 31, 63, 127, 255, 511, 1023, 2047, 4095, 8191, 16383};
        int sblimit;
        int nbal[32];
        const int* cls[32];
        if (h.lsf()) {
            sblimit = 30;
            for (int s = 0; s < 30; ++s) {
                if (s < 4) { nbal[s] = 4; cls[s] = kF; }
                else if (s < 11) { nbal[s] = 3; cls[s] = kE; }
                else { nbal[s] = 2; cls[s] = kE; }
            }
        } else {
            int perCh = h.bitrate / (nch == 2 ? 2 : 1);
            if (perCh < 56) {  // tables B.2c / B.2d
                sblimit = h.sampleRate == 32000 ? 12 : 8;
                for (int s = 0; s < sblimit; ++s) {
                    nbal[s] = s < 2 ? 4 : 3;
                    cls[s] = kE;
                }
            } else {  // tables B.2a / B.2b
                sblimit = (perCh >= 96 && h.sampleRate != 48000) ? 30 : 27;
                for (int s = 0; s < sblimit; ++s) {
                    if (s < 3) { nbal[s] = 4; cls[s] = kA; }
                    else if (s < 11) { nbal[s] = 4; cls[s] = kB; }
                    else if (s < 23) { nbal[s] = 3; cls[s] = kC; }
                    else { nbal[s] = 2; cls[s] = kD; }
                }
            }
        }
        if (bound > sblimit) bound = sblimit;
        for (int s = 0; s < sblimit; ++s)
            for (int c = 0; c < (s < bound ? nch : 1); ++c) {
                int a = br.get(nbal[s]);
                levels[c][s] = cls[s][a];
                alloc[c][s] = a;
                if (s >= bound) {
                    levels[1][s] = levels[0][s];
                    alloc[1][s] = a;
                }
            }
        int scfsi[2][32] = {{0}};
        for (int s = 0; s < sblimit; ++s)
            for (int c = 0; c < nch; ++c)
                if (alloc[c][s]) scfsi[c][s] = br.get(2);
        for (int s = 0; s < sblimit; ++s)
            for (int c = 0; c < nch; ++c) {
                if (!alloc[c][s]) continue;
                int* f = scf[c][s];
                switch (scfsi[c][s]) {
                    case 0: f[0] = br.get(6); f[1] = br.get(6); f[2] = br.get(6); break;
                    case 1: f[0] = f[1] = br.get(6); f[2] = br.get(6); break;
                    case 2: f[0] = f[1] = f[2] = br.get(6); break;
                    default: f[0] = br.get(6); f[1] = f[2] = br.get(6); break;
                }
            }
        for (int g = 0; g < 12; ++g) {
            int part = g >> 2;
            for (int s = 0; s < sblimit; ++s)
                for (int c = 0; c < (s < bound ? nch : 1); ++c) {
                    int L = levels[c][s];
                    if (!L) continue;
                    int code[3];
                    if (L == 3 || L == 5 || L == 9) {
                        int bits = L == 3 ? 5 : L == 5 ? 7 : 10;
                        int v = br.get(bits);
                        for (int k = 0; k < 3; ++k) {
                            code[k] = v % L;
                            v /= L;
                        }
                    } else {
                        int bits = 0;
                        while ((1 << bits) - 1 < L) ++bits;
                        for (int k = 0; k < 3; ++k) code[k] = br.get(bits);
                    }
                    for (int k = 0; k < 3; ++k) {
                        float v = (float)(2 * code[k] - (L - 1)) / L;
                        if (s < bound) {
                            samples[c][3 * g + k][s] = v * scale[scf[c][s][part]];
                        } else {
                            for (int cc = 0; cc < nch; ++cc) samples[cc][3 * g + k][s] = v * scale[scf[cc][s][part]];
                        }
                    }
                }
        }
        for (int c = 0; c < nch; ++c)
            for (int t = 0; t < 36; ++t) ch[c].synth.run(samples[c][t], pcm + (size_t)t * 32 * nch + c, nch);
    }
};

// ---------------------------------------------------------------- stream scanning
uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

size_t skipId3v2(const uint8_t* d, size_t size, size_t pos) {
    while (pos + 10 <= size && d[pos] == 'I' && d[pos + 1] == 'D' && d[pos + 2] == '3' && d[pos + 3] != 0xFF &&
           d[pos + 4] != 0xFF && !((d[pos + 6] | d[pos + 7] | d[pos + 8] | d[pos + 9]) & 0x80)) {
        size_t len = (size_t)d[pos + 6] << 21 | (size_t)d[pos + 7] << 14 | (size_t)d[pos + 8] << 7 | d[pos + 9];
        pos += 10 + len + ((d[pos + 5] & 0x10) ? 10 : 0);
    }
    return std::min(pos, size);
}

size_t trimTrailingTags(const uint8_t* d, size_t start, size_t end) {
    for (int guard = 0; guard < 4; ++guard) {
        if (end >= start + 128 && memcmp(d + end - 128, "TAG", 3) == 0) {
            end -= 128;
            continue;
        }
        if (end >= start + 32 && memcmp(d + end - 32, "APETAGEX", 8) == 0) {
            const uint8_t* f = d + end - 32;
            size_t len = (size_t)f[12] | (size_t)f[13] << 8 | (size_t)f[14] << 16 | (size_t)f[15] << 24;
            bool hasHeader = f[23] & 0x80;
            size_t total = len + (hasHeader ? 32 : 0);
            if (total <= end - start) {
                end -= total;
                continue;
            }
        }
        if (end >= start + 15 && memcmp(d + end - 9, "LYRICS200", 9) == 0) {
            size_t len = 0;
            for (int i = 0; i < 6; ++i) len = len * 10 + (d[end - 15 + i] - '0');
            if (len + 15 <= end - start) {
                end -= len + 15;
                continue;
            }
        }
        break;
    }
    return end;
}

// a header at p whose following frame (if there's room for one) also has a compatible header
bool confirmedFrame(const uint8_t* d, size_t p, size_t end, Header& h, int depth) {
    if (p + 4 > end || !parseHeader(d + p, h)) return false;
    size_t next = p + h.frameBytes;
    if (next > end) return false;
    if (next + 4 > end) return true;  // last frame
    for (int i = 0; i < depth; ++i) {
        Header n;
        if (!parseHeader(d + next, n) || !compatible(n, h)) return false;
        next += n.frameBytes;
        if (next + 4 > end) return next <= end || i > 0;
    }
    return true;
}

size_t findFrame(const uint8_t* d, size_t p, size_t end, const Header* ref, Header& h, int depth) {
    for (; p + 4 <= end; ++p) {
        if (d[p] != 0xFF || (d[p + 1] & 0xE0) != 0xE0) continue;
        if (confirmedFrame(d, p, end, h, depth) && (!ref || compatible(h, *ref))) return p;
    }
    return SIZE_MAX;
}

struct GaplessInfo {
    bool infoFrame = false;
    bool lame = false;
    int delay = 0, padding = 0;
};

GaplessInfo readInfoFrame(const uint8_t* f, const Header& h) {
    GaplessInfo g;
    if (h.layer != 3) return g;
    int side = h.lsf() ? (h.channels == 1 ? 9 : 17) : (h.channels == 1 ? 17 : 32);
    for (int crc = 0; crc < 2; ++crc) {
        size_t off = 4 + side + crc * 2;
        if (off + 8 > (size_t)h.frameBytes) continue;
        const uint8_t* p = f + off;
        if (memcmp(p, "Xing", 4) == 0 || memcmp(p, "Info", 4) == 0) {
            g.infoFrame = true;
            uint32_t flags = be32(p + 4);
            size_t q = off + 8;
            if (flags & 1) q += 4;
            if (flags & 2) q += 4;
            if (flags & 4) q += 100;
            if (flags & 8) q += 4;
            if (q + 24 <= (size_t)h.frameBytes) {
                const uint8_t* l = f + q;
                if (memcmp(l, "LAME", 4) == 0 || memcmp(l, "Lavc", 4) == 0 || memcmp(l, "Lavf", 4) == 0 ||
                    memcmp(l, "GOGO", 4) == 0) {
                    g.lame = true;
                    g.delay = l[21] << 4 | l[22] >> 4;
                    g.padding = (l[22] & 15) << 8 | l[23];
                }
            }
            return g;
        }
    }
    if (4 + 32 + 26 <= h.frameBytes && memcmp(f + 4 + 32, "VBRI", 4) == 0) g.infoFrame = true;
    return g;
}

}  // namespace

// ---------------------------------------------------------------- public interface
bool looksLikeMp3(const uint8_t* data, size_t size) {
    if (size >= 10 && data[0] == 'I' && data[1] == 'D' && data[2] == '3') return true;
    size_t end = std::min(size, (size_t)65536);
    Header h;
    size_t p = findFrame(data, 0, end, nullptr, h, 2);
    if (p == SIZE_MAX) return false;
    if (p == 0) return true;
    return p < 8192;  // some junk before the first frame is fine
}

bool decodeMp3(const uint8_t* data, size_t size, SoundData& out, std::string& error, int64_t maxFrames) {
    out = SoundData();
    if (!data || size < 4) {
        error = "not an MP3 file (too short)";
        return false;
    }
    size_t pos = skipId3v2(data, size, 0);
    size_t end = trimTrailingTags(data, pos, size);
    Header ref;
    size_t first = findFrame(data, pos, end, nullptr, ref, 2);
    if (first == SIZE_MAX) first = findFrame(data, pos, end, nullptr, ref, 0);
    if (first == SIZE_MAX) {
        error = "no MPEG audio frames found";
        return false;
    }
    out.sampleRate = ref.sampleRate;
    out.channels = ref.channels;
    int outCh = ref.channels;

    GaplessInfo gap = readInfoFrame(data + first, ref);
    pos = first;
    if (gap.infoFrame) pos += ref.frameBytes;
    int64_t skip = gap.lame ? gap.delay + 529 : 0;
    int64_t want = maxFrames >= 0 ? skip + maxFrames : -1;

    Mp3Decoder dec;
    std::vector<float> pcm(1152 * 2);
    std::vector<float>& s = out.samples;
    if (maxFrames < 0) s.reserve((size_t)((double)(end - pos) / std::max(ref.frameBytes, 1) * ref.samples * outCh) + 4096);
    int64_t decodedFrames = 0;
    int frames = 0;
    while (pos + 4 <= end) {
        Header h;
        if (!parseHeader(data + pos, h) || !compatible(h, ref) || pos + h.frameBytes > end) {
            size_t next = findFrame(data, pos + 1, end, &ref, h, 1);
            if (next == SIZE_MAX) break;
            pos = next;  // junk between frames (the bit reservoir is kept: it's usually an inserted tag)
            continue;
        }
        const uint8_t* f = data + pos;
        pos += h.frameBytes;
        if (h.layer == 3) dec.layer3(f, h, pcm.data());
        else dec.layer12(f, h, pcm.data());
        ++frames;
        int n = h.samples, nc = h.channels;
        if (nc == outCh) {
            s.insert(s.end(), pcm.begin(), pcm.begin() + (size_t)n * nc);
        } else if (outCh == 2) {  // mono frame in a stereo stream
            for (int i = 0; i < n; ++i) {
                s.push_back(pcm[i]);
                s.push_back(pcm[i]);
            }
        } else {
            for (int i = 0; i < n; ++i) s.push_back(0.5f * (pcm[2 * i] + pcm[2 * i + 1]));
        }
        decodedFrames += n;
        if (want >= 0 && decodedFrames >= want) break;
    }
    if (frames == 0) {
        error = "no decodable MPEG audio frames";
        out = SoundData();
        return false;
    }
    // gapless trimming (LAME tag): skip encoder delay + decoder delay, drop the padding
    int64_t total = decodedFrames;
    int64_t begin = 0, stop = total;
    if (gap.lame) {
        begin = std::min<int64_t>(skip, total);
        bool reachedEnd = !(want >= 0 && decodedFrames >= want);
        if (reachedEnd) stop = std::max<int64_t>(begin, std::min<int64_t>(total, total - gap.padding + 529));
    }
    if (maxFrames >= 0) stop = std::min<int64_t>(stop, begin + maxFrames);
    if (begin > 0 || stop < total) {
        std::vector<float> t(s.begin() + begin * outCh, s.begin() + stop * outCh);
        s.swap(t);
    }
    for (float& v : s) {
        if (!(v == v)) v = 0;  // NaN
        v = v > 1.f ? 1.f : v < -1.f ? -1.f : v;
    }
    return true;
}

// ---------------------------------------------------------------- hooks for the unit tests
// (tests/test_codecs.cpp declares these itself; they're not part of the program's interface)
void mp3TestImdct36(const float* in18, float* out36) { imdct36(in18, out36); }
void mp3TestImdct12(const float* in6, float* out12) { imdct12(in6, out12); }
void mp3TestSynth(const float* subbands, int slots, float* pcm) {  // subbands: slots x 32, fresh state
    Synth s;
    for (int t = 0; t < slots; ++t) s.run(subbands + 32 * t, pcm + 32 * t, 1);
}
void mp3TestSynthWindow(float* d512) { memcpy(d512, synthTables().D, sizeof(float) * 512); }
// table: 0..31 = table_select (pair tables, sym = x << 4 | y), 32 = count1 table A, 33 = table B (sym = vwxy)
bool mp3TestHuffCode(int table, int sym, uint32_t& code, int& len) {
    if (table == 33) {
        if (sym < 0 || sym > 15) return false;
        code = 15 - sym;
        len = 4;
        return true;
    }
    const HuffSet& H = huff();
    const HuffTable* t = table == 32 ? &H.t[32] : (table >= 0 && table < 32) ? H.forSelect[table] : nullptr;
    if (!t) return false;
    int i = table == 32 ? sym : (sym >> 4) * t->dim + (sym & 15);
    if (table != 32 && ((sym >> 4) >= t->dim || (sym & 15) >= t->dim)) return false;
    if (i < 0 || i >= t->dim * t->dim) return false;
    code = t->codes[i];
    len = t->lens[i];
    return true;
}
int mp3TestHuffLinbits(int table) { return table >= 0 && table < 32 ? kLinbits[table] : 0; }
int mp3TestHuffDimension(int table) {
    if (table == 32 || table == 33) return 4;
    const HuffTable* t = (table >= 0 && table < 32) ? huff().forSelect[table] : nullptr;
    return t ? t->dim : 0;
}
// decodes one symbol at *bitpos (buffer padded with 8 zero bytes); returns the symbol, -1 on error
int mp3TestHuffDecode(int table, const uint8_t* buf, size_t bytes, size_t* bitpos) {
    BitsBE br;
    br.init(buf, bytes);
    br.pos = *bitpos;
    int sym;
    if (table == 33) sym = (int)(br.get(4) ^ 15);
    else if (table == 32) sym = huff().t[32].decode(br);
    else if (table >= 0 && table < 32 && huff().forSelect[table]) sym = huff().forSelect[table]->decode(br);
    else return -1;
    *bitpos = br.pos;
    return sym;
}
