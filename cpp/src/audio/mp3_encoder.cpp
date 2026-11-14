// MP3 encoder (see mp3_encoder.h). Written from ISO 11172-3 / 13818-3 and the usual practice of
// encoders (LAME's frame layout, Info tag, reservoir limits and block-switching rules). The decoder
// it's checked against is ours (mp3.cpp) and mpg123.
//
// Signal path, per granule (576 samples) and channel:
//   input ─ polyphase analysis (32 subbands × 18) ─ MDCT (long 36-point, or 3 × 12-point short
//   windows; start/stop windows between) ─ alias-reduction butterflies (long blocks) ─ lowpass
//   ─ [mid/side] ─ quantization loops against the psychoacoustic model's allowed noise per band
//   ─ Huffman coding ─ frames, with the bit reservoir (main data may begin in earlier frames).
// Timing: the polyphase filter delays by 481 samples and the hybrid by 576, so a decoder gives the
// input back 1057 samples late; 48 zeros go in front (as LAME does), so the stored encoder delay is
// 576 and gapless decoders skip 576 + 529.
#include "audio/mp3_encoder.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>

#include "audio/fft.h"
#include "audio/psy.h"

namespace {

constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------- Huffman tables (ISO 11172-3 B.7)
// (the same data as the decoder's, mp3.cpp: codes MSB first, row = x, column = y; lengths without signs)
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

struct HTab {
    const uint16_t* c = nullptr;
    const uint8_t* l = nullptr;
    int dim = 0, lin = 0;
};
struct HTabs {
    HTab t[32];
    uint8_t bits[32][256];  // code length + sign bits of (x, y), x and y up to 15 (ESC tables: without linbits)
    HTabs() {
        struct Src { int num; const uint16_t* c; const uint8_t* l; int dim; };
        const Src src[] = {{1, kHc1, kHl1, 2},     {2, kHc2, kHl2, 3},     {3, kHc3, kHl3, 3},
                           {5, kHc5, kHl5, 4},     {6, kHc6, kHl6, 4},     {7, kHc7, kHl7, 6},
                           {8, kHc8, kHl8, 6},     {9, kHc9, kHl9, 6},     {10, kHc10, kHl10, 8},
                           {11, kHc11, kHl11, 8},  {12, kHc12, kHl12, 8},  {13, kHc13, kHl13, 16},
                           {15, kHc15, kHl15, 16}};
        for (auto& s : src) t[s.num] = HTab{s.c, s.l, s.dim, 0};
        for (int i = 16; i < 24; ++i) t[i] = HTab{kHc16, kHl16, 16, kLinbits[i]};
        for (int i = 24; i < 32; ++i) t[i] = HTab{kHc24, kHl24, 16, kLinbits[i]};
        memset(bits, 255, sizeof(bits));
        for (int i = 1; i < 32; ++i) {
            if (!t[i].c) continue;
            for (int x = 0; x < t[i].dim; ++x)
                for (int y = 0; y < t[i].dim; ++y) bits[i][x * 16 + y] = (uint8_t)(t[i].l[x * t[i].dim + y] + (x != 0) + (y != 0));
        }
    }
};
const HTabs& htabs() {
    static const HTabs h;
    return h;
}

// ---------------------------------------------------------------- MPEG tables
const int kBitratesV1[15] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
const int kBitratesV2[15] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
const int kRates[9] = {44100, 48000, 32000, 22050, 24000, 16000, 11025, 12000, 8000};

// scalefactor band widths per sample rate (index as kRates): long (22 bands), short (13 bands)
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
const uint8_t kSlen[2][16] = {{0, 0, 0, 0, 3, 1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4},
                              {0, 1, 2, 3, 0, 1, 2, 3, 1, 2, 3, 1, 2, 3, 2, 3}};

// the polyphase prototype window (ISO 11172-3 table B.3, as mp3.cpp): D[i] = h[i] (-1)^(i/64) / 65536,
// h symmetric; the analysis window is C[i] = D[i] / 32
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

enum BlockType { NORMAL = 0, START = 1, SHORT = 2, STOP = 3 };

// ---------------------------------------------------------------- transforms
struct Tables {
    double C[512];
    double M[32][32];         // analysis matrixing cos((2i+1)(k-16) pi/64), folded (see Analysis::run)
    double winL[4][36];       // block types 0, 1, 3
    double win12[12];
    double cosL[18][36];      // [k][n] cos(pi/72 (2n+19)(2k+1)) times the forward scale
    double cosS[6][12];       // [k][n] cos(pi/24 (2n+7)(2k+1)) times the forward scale
    double cs[8], ca[8];
    float ipow34[512];        // 2^(-0.1875 (e - 210)), e = index - 256: quantizer step (in the 3/4 domain)
    double pow25[512];        // 2^(0.25 (e - 210)): the decoder's step
    double pow43[8207];
    Tables() {
        for (int i = 0; i < 512; ++i) {
            int k = i <= 256 ? i : 512 - i;
            C[i] = kWindowHalf[k] / 65536.0 * (((i >> 6) & 1) ? -1 : 1) / 32.0;
        }
        for (int i = 0; i < 32; ++i)
            for (int j = 0; j < 32; ++j) {
                int k = j <= 16 ? j : j + 16;
                M[i][j] = std::cos((2 * i + 1) * (k - 16) * kPi / 64);
            }
        for (int i = 0; i < 36; ++i) {
            double longw = std::sin(kPi / 36 * (i + 0.5));
            winL[0][i] = longw;
            winL[1][i] = i < 18 ? longw : i < 24 ? 1.0 : i < 30 ? std::sin(kPi / 12 * (i - 18 + 0.5)) : 0.0;
            winL[3][i] = i < 6 ? 0.0 : i < 12 ? std::sin(kPi / 12 * (i - 6 + 0.5)) : i < 18 ? 1.0 : longw;
            winL[2][i] = longw;
        }
        for (int i = 0; i < 12; ++i) win12[i] = std::sin(kPi / 12 * (i + 0.5));
        // forward scales: the decoder's IMDCTs are unscaled, so the MDCTs carry 1/9 and 1/3 (2/N)
        for (int k = 0; k < 18; ++k)
            for (int n = 0; n < 36; ++n) cosL[k][n] = std::cos(kPi / 72 * (2 * n + 19) * (2 * k + 1)) / 9.0;
        for (int k = 0; k < 6; ++k)
            for (int n = 0; n < 12; ++n) cosS[k][n] = std::cos(kPi / 24 * (2 * n + 7) * (2 * k + 1)) / 3.0;
        const double c[8] = {-0.6, -0.535, -0.33, -0.185, -0.095, -0.041, -0.0142, -0.0037};
        for (int i = 0; i < 8; ++i) {
            double sq = std::sqrt(1.0 + c[i] * c[i]);
            cs[i] = 1.0 / sq;
            ca[i] = c[i] / sq;
        }
        for (int i = 0; i < 512; ++i) {
            int e = i - 256;
            ipow34[i] = (float)std::pow(2.0, -0.1875 * (e - 210));
            pow25[i] = std::pow(2.0, 0.25 * (e - 210));
        }
        for (int i = 0; i < 8207; ++i) pow43[i] = std::pow((double)i, 4.0 / 3.0);
    }
};
const Tables& tables() {
    static const Tables t;
    return t;
}

// analysis polyphase filterbank (ISO 11172-3 C.1.3)
struct Analysis {
    double ring[512] = {0};
    int pos = 0;
    // 32 new samples (in time order) in, one sample of each of the 32 subbands out
    void run(const float* in, double* S) {
        const Tables& T = tables();
        for (int j = 0; j < 32; ++j) {
            pos = (pos + 1) & 511;
            ring[pos] = in[j];
        }
        double Y[64] = {0};
        for (int i = 0; i < 512; ++i) Y[i & 63] += T.C[i] * ring[(pos - i) & 511];
        // cos((2i+1)(k-16) pi/64) is the same for k and 32 - k, opposite for k and 96 - k, 0 for k = 48
        double V[32];
        for (int j = 0; j < 16; ++j) V[j] = Y[j] + Y[32 - j];
        V[16] = Y[16];
        for (int j = 17; j < 32; ++j) V[j] = Y[j + 16] - Y[80 - j];
        for (int i = 0; i < 32; ++i) {
            const double* m = T.M[i];
            double s = 0;
            for (int j = 0; j < 32; ++j) s += m[j] * V[j];
            S[i] = s;
        }
    }
};

// MDCT of one granule of one channel: prev / cur = [subband][18] (frequency inversion done);
// out in bitstream order (long: subband-major; short: band, window, line)
void mdct(const double (*prev)[18], const double (*cur)[18], int blockType, const int* sfbS, float* out) {
    const Tables& T = tables();
    if (blockType != SHORT) {
        const double* w = T.winL[blockType];
        for (int s = 0; s < 32; ++s) {
            double z[36];
            for (int n = 0; n < 18; ++n) {
                z[n] = prev[s][n] * w[n];
                z[n + 18] = cur[s][n] * w[n + 18];
            }
            for (int k = 0; k < 18; ++k) {
                const double* c = T.cosL[k];
                double a = 0;
                for (int n = 0; n < 36; ++n) a += c[n] * z[n];
                out[18 * s + k] = (float)a;
            }
        }
        for (int s = 0; s < 31; ++s) {  // the inverse of the decoder's alias reduction
            float* a = out + 18 * s + 17;
            float* b = out + 18 * (s + 1);
            for (int i = 0; i < 8; ++i) {
                double u = a[-i], d = b[i];
                a[-i] = (float)(u * T.cs[i] + d * T.ca[i]);
                b[i] = (float)(d * T.cs[i] - u * T.ca[i]);
            }
        }
        return;
    }
    float tmp[3][192];  // [window][6 * subband + k]
    for (int s = 0; s < 32; ++s) {
        double z36[36];
        for (int n = 0; n < 18; ++n) {
            z36[n] = prev[s][n];
            z36[n + 18] = cur[s][n];
        }
        for (int w = 0; w < 3; ++w) {
            double z[12];
            for (int n = 0; n < 12; ++n) z[n] = z36[6 + 6 * w + n] * T.win12[n];
            for (int k = 0; k < 6; ++k) {
                double a = 0;
                for (int n = 0; n < 12; ++n) a += T.cosS[k][n] * z[n];
                tmp[w][6 * s + k] = (float)a;
            }
        }
    }
    for (int b = 0; b < 13; ++b) {
        int a = sfbS[b], wd = sfbS[b + 1] - a;
        for (int w = 0; w < 3; ++w)
            for (int j = 0; j < wd; ++j) out[3 * a + w * wd + j] = tmp[w][a + j];
    }
}

// ---------------------------------------------------------------- bits
struct BitWriter {
    std::vector<uint8_t> buf;
    uint64_t acc = 0;
    int n = 0;
    size_t bits = 0;
    void put(uint32_t v, int len) {
        if (len <= 0) return;
        acc = (acc << len) | (v & (len >= 32 ? 0xffffffffu : ((1u << len) - 1)));
        n += len;
        bits += len;
        while (n >= 8) {
            n -= 8;
            buf.push_back((uint8_t)(acc >> n));
        }
        acc &= (1u << n) - 1;
    }
    void align() {
        if (n) put(0, 8 - n);
    }
};

uint16_t crc16(const uint8_t* p, size_t n, uint16_t crc) {  // CRC-16/ARC (as the LAME tag uses)
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int k = 0; k < 8; ++k) crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
    }
    return crc;
}

// ---------------------------------------------------------------- band layouts
// The bands in bitstream order: long blocks 22 bands, short blocks 13 bands × 3 windows (band-major).
struct Layout {
    int n = 0;
    int start[40], width[40], win[40], maxSf[40], part[40];
    int end() const { return start[n - 1] + width[n - 1]; }
};

// ---------------------------------------------------------------- psychoacoustics
// (FFT bin, weight) lists of each band's frequency range
struct BandBins {
    std::vector<int> bin;
    std::vector<float> w;
    std::vector<int> off;  // band b: entries [off[b], off[b + 1])
    // band b = MDCT lines [edges[b], edges[b+1]) of `lines` over 0 .. sr/2; FFT of N points
    void build(const int* edges, int bands, int lines, int N) {
        off.push_back(0);
        double perBin = (double)lines / (N / 2);  // MDCT lines per FFT bin
        for (int b = 0; b < bands; ++b) {
            double lo = edges[b] / perBin, hi = edges[b + 1] / perBin;  // in bins (bin j covers [j-.5, j+.5))
            int j0 = std::max(0, (int)std::floor(lo + 0.5)), j1 = std::min(N / 2, (int)std::floor(hi + 0.5));
            bool any = false;
            for (int j = j0; j <= j1; ++j) {
                double a = std::max(lo, j - 0.5), e = std::min(hi, j + 0.5);
                if (e - a > 1e-9) {
                    bin.push_back(j);
                    w.push_back((float)(e - a));
                    any = true;
                }
            }
            if (!any) {
                bin.push_back(std::min(N / 2, j0));
                w.push_back(1.0f);
            }
            off.push_back((int)bin.size());
        }
    }
};

struct PsyChannel {
    std::vector<fft::cd> X1, X2;  // the long FFTs of the two granules before
    std::vector<double> nbPrev;    // the threshold per partition last granule (pre-echo control)
    int history = 0;
};

// ---------------------------------------------------------------- the encoder
struct Coded {  // one granule of one channel, quantized
    int blockType = 0;
    const Layout* L = nullptr;
    int gg = 210, sfScale = 0, sfc = 0, part2 = 0, part23 = 0;
    int bigValues = 0, count1Table = 0, region0 = 0, region1 = 0;
    int tables[3] = {0, 0, 0};
    int sf[40];
    int ix[576];
};

struct GrCh {  // one granule of one channel before quantizing
    float xr[576];
    float xr34[576];  // |xr|^(3/4)
    double en[40], xmin[40];
    double pe = 0;
};

class Encoder {
public:
    // config
    int sr = 44100, table = 0, version = 0, nch = 2, ngr = 2, kbps = 320, brIdx = 14;
    bool lsf = false;
    int sideBytes = 32;
    int sfbL[23], sfbS[14];
    Layout layoutL, layoutS;
    int lowpassLine = 576, lowpassStart = 576;  // long lines; zero from lowpassLine
    double lowpassHz = 20000;
    int resvMaxBits = 0;
    double maxRatio = 0.5;
    // psychoacoustics
    psy::Partitions partL, partS;
    BandBins binsL, binsS;
    std::vector<double> hannL, hannS;
    std::unique_ptr<fft::Plan> planL, planS;
    PsyChannel psy[2];
    std::vector<fft::cd> work;
    double marginDb = 3.0;
    double peAvg = 500;

    bool setup(int rate, int channels, int wantKbps, std::string& error);
    bool encode(const float* in, int64_t frames, int inStride, std::vector<uint8_t>& out, std::string& error,
                const std::function<bool(double)>& progress);

private:
    // input access: y = 48 zeros, the input, zeros
    const float* src = nullptr;
    int64_t srcFrames = 0;
    int stride = 1;
    static constexpr int kPad = 48;
    float sample(int c, int64_t i) const {
        int64_t j = i - kPad;
        if (j < 0 || j >= srcFrames) return 0.0f;
        float v = src[j * stride + c];
        return v == v ? std::clamp(v, -8.0f, 8.0f) : 0.0f;  // (no NaN; overloads kept codable)
    }

    void attackFlags(int granules, std::vector<uint8_t>& flags);
    void psyLong(int g, double ratio[2][22], double pe[2]);
    void psyShort(int g, double ratio[2][13][3]);
    void partitionThreshold(const psy::Partitions& P, const double* energy, const double* unpred, double snrTonal,
                            double snrNoise, double* thrBin, PsyChannel* st);
    // quantization
    int quantize(const GrCh& G, Coded& q) const;  // max |ix| (> 8206: too big)
    int countBits(Coded& q, bool best) const;
    int part2Bits(Coded& q) const;
    void outerLoop(const GrCh& G, Coded& q, int target, double& worstNmr) const;
    void writeGranule(BitWriter& bw, const Coded& q) const;
};

bool Encoder::setup(int rate, int channels, int wantKbps, std::string& error) {
    sr = rate;
    table = -1;
    for (int i = 0; i < 9; ++i)
        if (kRates[i] == rate) table = i;
    if (table < 0) {
        error = "MP3 has no frames for " + std::to_string(rate) + " Hz";
        return false;
    }
    version = table / 3;  // 0 MPEG-1, 1 MPEG-2, 2 MPEG-2.5
    lsf = version != 0;
    ngr = lsf ? 1 : 2;
    nch = channels >= 2 ? 2 : 1;
    sideBytes = lsf ? (nch == 1 ? 9 : 17) : (nch == 1 ? 17 : 32);
    const int* rates = lsf ? kBitratesV2 : kBitratesV1;
    brIdx = 1;
    for (int i = 1; i < 15; ++i)
        if (std::abs(rates[i] - wantKbps) < std::abs(rates[brIdx] - wantKbps)) brIdx = i;
    kbps = rates[brIdx];

    sfbL[0] = 0;
    for (int b = 0; b < 22; ++b) sfbL[b + 1] = sfbL[b] + kLongWidths[table][b];
    sfbS[0] = 0;
    for (int b = 0; b < 13; ++b) sfbS[b + 1] = sfbS[b] + kShortWidths[table][b];
    // layouts and scalefactor limits (MPEG-1: slen1 up to 4 bits for bands 0-10 / 0-5, slen2 up to 3;
    // MPEG-2: partitions of 6, 5, 5, 5 long bands / 3, 3, 3, 3 short bands with 4, 4, 3, 3 bits)
    Layout& L = layoutL;
    L.n = 22;
    for (int b = 0; b < 22; ++b) {
        L.start[b] = sfbL[b];
        L.width[b] = sfbL[b + 1] - sfbL[b];
        L.win[b] = 0;
        int part = b < 6 ? 0 : b < 11 ? 1 : b < 16 ? 2 : b < 21 ? 3 : -1;
        L.part[b] = part;
        L.maxSf[b] = part < 0 ? 0 : part < 2 ? 15 : 7;
    }
    Layout& S = layoutS;
    S.n = 39;
    for (int b = 0; b < 13; ++b) {
        int wd = sfbS[b + 1] - sfbS[b];
        for (int w = 0; w < 3; ++w) {
            int f = 3 * b + w;
            S.start[f] = 3 * sfbS[b] + w * wd;
            S.width[f] = wd;
            S.win[f] = w;
            int part = b < 3 ? 0 : b < 6 ? 1 : b < 9 ? 2 : b < 12 ? 3 : -1;
            S.part[f] = part;
            S.maxSf[f] = part < 0 ? 0 : part < 2 ? 15 : 7;
        }
    }
    // lowpass by the bitrate per channel (about what LAME uses)
    double perCh = (double)kbps / nch;
    struct LP { double kbpsCh, hz; };
    const LP lp[] = {{8, 3000},    {16, 5500},   {24, 8000},   {32, 11000},  {40, 13000}, {48, 15000},
                     {56, 16000},  {64, 16500},  {80, 17500},  {96, 18500},  {112, 19500}, {128, 20000},
                     {160, 20500}, {1000, 20500}};
    lowpassHz = lp[0].hz;
    for (int i = 1; i < (int)(sizeof(lp) / sizeof(lp[0])); ++i) {
        if (perCh <= lp[i].kbpsCh) {
            double t = (perCh - lp[i - 1].kbpsCh) / (lp[i].kbpsCh - lp[i - 1].kbpsCh);
            lowpassHz = lp[i - 1].hz + t * (lp[i].hz - lp[i - 1].hz);
            break;
        }
    }
    lowpassHz = std::min(lowpassHz, 0.48 * sr);
    lowpassLine = std::min(576, (int)std::ceil(lowpassHz / (sr / 2.0) * 576));
    lowpassStart = std::max(0, (int)(lowpassLine - 0.03 * 576));
    maxRatio = perCh >= 96 ? 0.5 : 4.0;
    // the reservoir: main_data_begin has 9 bits (MPEG-1) / 8 bits, and a decoder holds 1440 bytes
    // at most of main data (LAME's limit, a little above ISO's 960 bytes)
    {
        int frameBits = 8 * ((lsf ? 72000 : 144000) * kbps / sr);
        int lim = std::min(8 * (lsf ? 255 : 511), 8 * 1440 - frameBits);
        resvMaxBits = std::max(0, lim) & ~7;
    }
    partL.build(1024, sr);
    partS.build(256, sr);
    binsL.build(sfbL, 22, 576, 1024);
    binsS.build(sfbS, 13, 192, 256);
    hannL.resize(1024);
    for (int i = 0; i < 1024; ++i) hannL[i] = 0.5 - 0.5 * std::cos(2 * kPi * (i + 0.5) / 1024);
    hannS.resize(256);
    for (int i = 0; i < 256; ++i) hannS[i] = 0.5 - 0.5 * std::cos(2 * kPi * (i + 0.5) / 256);
    planL = std::make_unique<fft::Plan>(1024);
    planS = std::make_unique<fft::Plan>(256);
    for (auto& p : psy) {
        p.X1.assign(513, 0.0);
        p.X2.assign(513, 0.0);
        p.nbPrev.assign(partL.n, 1e30);
        p.history = 0;
    }
    return true;
}

// ---------------------------------------------------------------- psychoacoustic model
// The threshold per partition from the energies (and unpredictability, 0 tonal .. 1 noise), spread
// in Bark; returned per bin (spread evenly over its partition's bins).
void Encoder::partitionThreshold(const psy::Partitions& P, const double* energy, const double* unpred, double snrTonal,
                                 double snrNoise, double* thrBin, PsyChannel* st) {
    double e[160], c[160];
    for (int p = 0; p < P.n; ++p) {
        double s = 0, sc = 0;
        for (int j = P.first[p]; j < P.last[p]; ++j) {
            s += energy[j];
            if (unpred) sc += energy[j] * unpred[j];
        }
        e[p] = s;
        c[p] = sc;
    }
    for (int q = 0; q < P.n; ++q) {
        double ecb = 0, ct = 0;
        const double* sp = &P.spread[(size_t)q * P.n];
        for (int p = P.spreadLo[q]; p < P.spreadHi[q]; ++p) {
            ecb += sp[p] * e[p];
            ct += sp[p] * c[p];
        }
        double snr = snrNoise;
        if (unpred) {
            double cb = ecb > 0 ? ct / ecb : 1.0;
            double tb = std::clamp(-0.299 - 0.43 * std::log(std::max(cb, 1e-6)), 0.0, 1.0);
            double tonal = snrTonal + P.z[q];  // (tones mask less, more so up high: Johnston's 14.5 + z)
            snr = tb * tonal + (1 - tb) * snrNoise;
        }
        double nb = ecb * P.rnorm[q] * std::pow(10.0, -snr / 10.0);
        if (st) {  // pre-echo control: no more than twice last granule's threshold
            double raw = nb;
            nb = std::min(nb, 2.0 * st->nbPrev[q]);
            st->nbPrev[q] = raw;
        }
        nb = std::max(nb, P.athE[q]);
        double perBin = nb / (P.last[q] - P.first[q]);
        for (int j = P.first[q]; j < P.last[q]; ++j) thrBin[j] = perBin;
    }
}

// granule g's long-block analysis: the allowed-noise ratio (threshold / energy) of each long band
void Encoder::psyLong(int g, double ratio[2][22], double pe[2]) {
    const int N = 1024, nb = 513;
    const int64_t c0 = (int64_t)576 * g - 240 - N / 2;  // (the long MDCT window's centre is at 576 g - 240)
    std::vector<double> a(N), b(N);
    std::vector<fft::cd> A(nb), B(nb);
    for (int i = 0; i < N; ++i) {
        a[i] = sample(0, c0 + i) * hannL[i];
        b[i] = nch > 1 ? sample(1, c0 + i) * hannL[i] : 0.0;
    }
    fft::realPair(*planL, a.data(), b.data(), A.data(), nch > 1 ? B.data() : nullptr, work);
    for (int ch = 0; ch < nch; ++ch) {
        std::vector<fft::cd>& X = ch == 0 ? A : B;
        PsyChannel& st = psy[ch];
        double energy[513], unpred[513], thr[513];
        for (int j = 0; j < nb; ++j) {
            energy[j] = std::norm(X[j]);
            // unpredictability: the distance from the value predicted from the two granules before
            double r = std::abs(X[j]);
            double r1 = std::abs(st.X1[j]), r2 = std::abs(st.X2[j]);
            double c = 1.0;
            if (st.history >= 2 && r + std::abs(2 * r1 - r2) > 0 && r1 > 0 && r2 > 0) {
                fft::cd u1 = st.X1[j] / r1, u2 = st.X2[j] / r2;
                fft::cd pred = (2 * r1 - r2) * (u1 * u1 * std::conj(u2));
                c = std::abs(X[j] - pred) / (r + std::abs(2 * r1 - r2));
            }
            unpred[j] = std::min(1.0, c);
        }
        st.X2 = st.X1;
        st.X1 = X;
        st.history++;
        partitionThreshold(partL, energy, unpred, 14.5, 6.0, thr, &st);
        double p = 0;
        for (int sb = 0; sb < 22; ++sb) {
            double en = 0, th = 0;
            for (int k = binsL.off[sb]; k < binsL.off[sb + 1]; ++k) {
                en += binsL.w[k] * energy[binsL.bin[k]];
                th += binsL.w[k] * thr[binsL.bin[k]];
            }
            ratio[ch][sb] = en > 0 ? th / en : 1.0;
            if (en > th && en > 0) p += (sfbL[sb + 1] - sfbL[sb]) * std::log2(en / th);
        }
        pe[ch] = p;
    }
}

// granule g's short-block analysis: the ratio of each short band in each of the three windows
void Encoder::psyShort(int g, double ratio[2][13][3]) {
    const int N = 256, nb = 129;
    std::vector<double> a(N), b(N);
    std::vector<fft::cd> A(nb), B(nb);
    for (int w = 0; w < 3; ++w) {
        const int64_t c0 = (int64_t)576 * g - 240 + 192 * (w - 1) - N / 2;
        for (int i = 0; i < N; ++i) {
            a[i] = sample(0, c0 + i) * hannS[i];
            b[i] = nch > 1 ? sample(1, c0 + i) * hannS[i] : 0.0;
        }
        fft::realPair(*planS, a.data(), b.data(), A.data(), nch > 1 ? B.data() : nullptr, work);
        for (int ch = 0; ch < nch; ++ch) {
            std::vector<fft::cd>& X = ch == 0 ? A : B;
            double energy[129], thr[129];
            for (int j = 0; j < nb; ++j) energy[j] = std::norm(X[j]);
            partitionThreshold(partS, energy, nullptr, 0, 10.0, thr, nullptr);
            for (int sb = 0; sb < 13; ++sb) {
                double en = 0, th = 0;
                for (int k = binsS.off[sb]; k < binsS.off[sb + 1]; ++k) {
                    en += binsS.w[k] * energy[binsS.bin[k]];
                    th += binsS.w[k] * thr[binsS.bin[k]];
                }
                ratio[ch][sb][w] = en > 0 ? th / en : 1.0;
            }
        }
    }
}

// granules with a transient in their window: the high-passed signal's energy in 128-sample pieces
// jumps tenfold over the three pieces before
void Encoder::attackFlags(int granules, std::vector<uint8_t>& flags) {
    const int seg = 128;
    int64_t total = (int64_t)576 * (granules + 2) + 1024;
    int64_t nseg = total / seg + 1;
    std::vector<uint8_t> attack((size_t)nseg, 0);
    for (int c = 0; c < nch; ++c) {
        std::vector<double> e((size_t)nseg, 0.0);
        float prev = 0;
        for (int64_t s = 0; s < nseg; ++s) {
            double acc = 0;
            for (int i = 0; i < seg; ++i) {
                float x = sample(c, s * seg + i);
                double d = x - prev;
                prev = x;
                acc += d * d;
            }
            e[(size_t)s] = acc;
        }
        for (int64_t s = 4; s < nseg; ++s) {  // (the piece just before is skipped: attacks may take ~5 ms)
            double before = std::max(std::max(e[(size_t)s - 2], e[(size_t)s - 3]), e[(size_t)s - 4]);
            if (e[(size_t)s] > 10.0 * before && e[(size_t)s] > seg * 1e-6) attack[(size_t)s] = 1;
        }
    }
    flags.assign((size_t)granules + 1, 0);
    for (int g = 0; g <= granules; ++g) {
        int64_t centre = (int64_t)576 * g - 240;
        for (int64_t t = std::max<int64_t>(0, centre - 400); t < centre + 400; t += seg)
            if (t / seg < nseg && attack[(size_t)(t / seg)]) flags[(size_t)g] = 1;
    }
}

// ---------------------------------------------------------------- quantization
int Encoder::quantize(const GrCh& G, Coded& q) const {
    const Tables& T = tables();
    const Layout& L = *q.L;
    int shift = 1 + q.sfScale;
    int maxv = 0;
    for (int f = 0; f < L.n; ++f) {
        int e = q.gg - (q.sf[f] << shift);
        float step = T.ipow34[std::clamp(e + 256, 0, 511)];
        int a = L.start[f], end = a + L.width[f];
        for (int i = a; i < end; ++i) {
            float v = G.xr34[i] * step;  // |xr|^(3/4) / step
            if (v > 8206.0f) return 8207;
            int iv = (int)(v + 0.4054f);
            q.ix[i] = G.xr[i] < 0 ? -iv : iv;
            maxv = std::max(maxv, iv);
        }
    }
    return maxv;
}

// bits of the pairs [a, b) with the tables sel[0..n) at once (all without linbits, or all ESC tables)
void pairBits(const int* ix, int a, int b, const int* sel, int n, int* out) {
    const HTabs& H = htabs();
    const uint8_t* t0 = H.bits[sel[0]];
    const uint8_t* t1 = H.bits[sel[n > 1 ? 1 : 0]];
    const uint8_t* t2 = H.bits[sel[n > 2 ? 2 : 0]];
    int b0 = 0, b1 = 0, b2 = 0, esc = 0;
    for (int i = a; i < b; i += 2) {
        int x = std::abs(ix[i]), y = std::abs(ix[i + 1]);
        if (x >= 15) {
            x = 15;
            ++esc;
        }
        if (y >= 15) {
            y = 15;
            ++esc;
        }
        int k = x * 16 + y;
        b0 += t0[k];
        b1 += t1[k];
        b2 += t2[k];
    }
    out[0] = b0 + esc * H.t[sel[0]].lin;
    if (n > 1) out[1] = b1 + esc * H.t[sel[1]].lin;
    if (n > 2) out[2] = b2 + esc * H.t[sel[2]].lin;
}

// the cheapest table for the pairs [a, b): bits, and the table in `sel`
int chooseTable(const int* ix, int a, int b, int& sel) {
    sel = 0;
    if (a >= b) return 0;
    int m = 0;
    for (int i = a; i < b; ++i) m = std::max(m, std::abs(ix[i]));
    if (m == 0) return 0;
    static const int groups[16][4] = {{0},         {1, -1},    {2, 3, -1},     {5, 6, -1},     {7, 8, 9, -1},
                                      {7, 8, 9, -1}, {10, 11, 12, -1}, {10, 11, 12, -1}, {13, 15, -1},
                                      {13, 15, -1}, {13, 15, -1}, {13, 15, -1}, {13, 15, -1}, {13, 15, -1},
                                      {13, 15, -1}, {13, 15, -1}};
    int cand[3] = {1, 1, 1}, n = 0, bits[3];
    if (m <= 15) {
        for (const int* g = groups[m]; *g >= 0; ++g) cand[n++] = *g;
    } else {
        int esc = m - 15;
        for (int base : {16, 24})
            for (int s = base; s < base + 8; ++s)
                if ((1 << kLinbits[s]) - 1 >= esc) {
                    cand[n++] = s;
                    break;
                }
    }
    pairBits(ix, a, b, cand, n, bits);
    int best = 0;
    for (int k = 1; k < n; ++k)
        if (bits[k] < bits[best]) best = k;
    sel = cand[best];
    return bits[best];
}

// Huffman bits of a quantized granule (big values, regions, tables, count1); `best` searches the
// region split of long blocks (else LAME's usual split by the big values' extent)
int Encoder::countBits(Coded& q, bool best) const {
    const int* ix = q.ix;
    int i = 576;
    while (i > 1 && ix[i - 1] == 0 && ix[i - 2] == 0) i -= 2;
    int c1a = 0, c1b = 0;
    while (i > 3) {
        int v = std::abs(ix[i - 4]), w = std::abs(ix[i - 3]), x = std::abs(ix[i - 2]), y = std::abs(ix[i - 1]);
        if ((v | w | x | y) > 1) break;
        int p = v * 8 + w * 4 + x * 2 + y, signs = v + w + x + y;
        c1a += kHlA[p] + signs;
        c1b += 4 + signs;
        i -= 4;
    }
    q.bigValues = i / 2;
    q.count1Table = c1b < c1a ? 1 : 0;
    int bits = std::min(c1a, c1b);
    int big = i;
    if (q.blockType != NORMAL) {
        // window switching: region 0 is 8 short bands-windows / 7 + 1 long bands, region 1 the rest
        int r1 = 0;
        if (q.blockType == SHORT) {
            for (int f = 0; f < 9; ++f) r1 += layoutS.width[f];
        } else {
            r1 = sfbL[8];
        }
        r1 = std::min(r1, big);
        q.region0 = q.blockType == SHORT ? 8 : 7;
        q.region1 = 36;
        bits += chooseTable(ix, 0, r1, q.tables[0]);
        bits += chooseTable(ix, r1, big, q.tables[1]);
        q.tables[2] = 0;
        return bits;
    }
    if (!best) {
        static const int subdv[23][2] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 1}, {1, 1}, {1, 1},
                                          {1, 2}, {2, 2}, {2, 3}, {2, 3}, {3, 4}, {3, 4}, {3, 4}, {4, 5},
                                          {4, 5}, {4, 6}, {5, 6}, {5, 6}, {5, 7}, {6, 7}, {6, 7}};
        int idx = 0;
        while (idx < 22 && sfbL[idx] < big) ++idx;
        int r0 = subdv[idx][0], r1c = subdv[idx][1];
        int a1 = std::min(sfbL[std::min(22, r0 + 1)], big), a2 = std::min(sfbL[std::min(22, r0 + r1c + 2)], big);
        q.region0 = r0;
        q.region1 = r1c;
        bits += chooseTable(ix, 0, a1, q.tables[0]);
        bits += chooseTable(ix, a1, a2, q.tables[1]);
        bits += chooseTable(ix, a2, big, q.tables[2]);
        return bits;
    }
    // every split: the best table for each region between band edges (or the big values' end)
    int bestBits = 1 << 30;
    int cost[23][24];  // [from edge][to edge or 23 = big]: bits, table
    int sel[23][24];
    auto edge = [&](int e) { return e >= 23 ? big : std::min(sfbL[e], big); };
    for (int a = 0; a <= 22; ++a)
        for (int b = 0; b <= 23; ++b) cost[a][b] = -1;
    auto region = [&](int ea, int eb, int& s) {
        if (cost[ea][eb] < 0) cost[ea][eb] = chooseTable(ix, edge(ea), edge(eb), sel[ea][eb]);
        s = sel[ea][eb];
        return cost[ea][eb];
    };
    for (int r0 = 0; r0 < 16; ++r0) {
        int e1 = r0 + 1;
        if (e1 > 22) break;
        for (int r1 = 0; r1 < 8; ++r1) {
            int e2 = r0 + r1 + 2;
            if (e2 > 22) break;
            int s0, s1, s2;
            int b0 = region(0, e1, s0);
            int b1 = region(e1, e2, s1);
            int b2 = region(e2, 23, s2);
            int t = b0 + b1 + b2;
            if (t < bestBits) {
                bestBits = t;
                q.region0 = r0;
                q.region1 = r1;
                q.tables[0] = s0;
                q.tables[1] = s1;
                q.tables[2] = s2;
            }
            if (edge(e2) >= big) break;
        }
        if (edge(e1) >= big) break;
    }
    return bits + bestBits;
}

int bitLen(int v) {
    int n = 0;
    while (v > 0) {
        ++n;
        v >>= 1;
    }
    return n;
}

// the scalefactors' bits (and scalefac_compress); -1 if they can't be coded
int Encoder::part2Bits(Coded& q) const {
    const Layout& L = *q.L;
    int maxp[4] = {0, 0, 0, 0}, cnt[4] = {0, 0, 0, 0};
    for (int f = 0; f < L.n; ++f) {
        if (L.part[f] < 0) continue;
        maxp[L.part[f]] = std::max(maxp[L.part[f]], q.sf[f]);
        cnt[L.part[f]]++;
    }
    if (!lsf) {
        int need1 = bitLen(std::max(maxp[0], maxp[1])), need2 = bitLen(std::max(maxp[2], maxp[3]));
        int n1 = cnt[0] + cnt[1], n2 = cnt[2] + cnt[3];
        int best = -1, bestBits = 1 << 30;
        for (int s = 0; s < 16; ++s) {
            if (kSlen[0][s] < need1 || kSlen[1][s] < need2) continue;
            int bits = n1 * kSlen[0][s] + n2 * kSlen[1][s];
            if (bits < bestBits) {
                bestBits = bits;
                best = s;
            }
        }
        if (best < 0) return -1;
        q.sfc = best;
        return bestBits;
    }
    int sl[4];
    for (int p = 0; p < 4; ++p) sl[p] = bitLen(maxp[p]);
    if (sl[0] > 4 || sl[1] > 4 || sl[2] > 3 || sl[3] > 3) return -1;
    q.sfc = ((sl[0] * 5 + sl[1]) << 4) + (sl[2] << 2) + sl[3];
    return cnt[0] * sl[0] + cnt[1] * sl[1] + cnt[2] * sl[2] + cnt[3] * sl[3];
}

// The quantization loops: the global gain for the bits (inner), and scalefactors amplifying the
// bands whose noise is over the allowed (outer); the best try is kept (lowest worst noise-to-mask).
void Encoder::outerLoop(const GrCh& G, Coded& q, int target, double& worstNmr) const {
    const Tables& T = tables();
    const Layout& L = *q.L;
    std::fill(q.sf, q.sf + 40, 0);
    q.sfScale = 0;
    bool silent = true;
    for (int i = 0; i < 576; ++i)
        if (G.xr[i] != 0) {
            silent = false;
            break;
        }
    if (silent) {
        std::fill(q.ix, q.ix + 576, 0);
        q.gg = 210;
        q.part2 = part2Bits(q);
        q.part23 = q.part2 + countBits(q, true);
        worstNmr = 0;
        return;
    }
    auto fits = [&](int gg, int p2) {
        q.gg = gg;
        if (quantize(G, q) > 8206) return false;
        return p2 + countBits(q, false) <= target;
    };
    int p2 = part2Bits(q);
    // inner loop: the lowest global gain that fits (binary search)
    int lo = 0, hi = 255;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (fits(mid, p2)) hi = mid;
        else lo = mid + 1;
    }
    int gg = lo;
    Coded best;
    double bestScore = 1e300, bestWorst = 1e300;
    double nmr[40];
    for (int iter = 0; iter < 40; ++iter) {
        q.gg = gg;
        quantize(G, q);
        // noise per band against the allowed
        int shift = 1 + q.sfScale;
        double worst = 0, over = 0;
        for (int f = 0; f < L.n; ++f) {
            int e = q.gg - (q.sf[f] << shift);
            double step = T.pow25[std::clamp(e + 256, 0, 511)];
            double noise = 0;
            for (int i = L.start[f], end = i + L.width[f]; i < end; ++i) {
                double d = std::fabs(G.xr[i]) - T.pow43[std::abs(q.ix[i])] * step;
                noise += d * d;
            }
            nmr[f] = noise / G.xmin[f];
            if (L.maxSf[f] == 0) continue;  // (bands without a scalefactor: only the global gain)
            worst = std::max(worst, nmr[f]);
            if (nmr[f] > 1) over += 10 * std::log10(nmr[f]);
        }
        double score = (worst > 1 ? 10 * std::log10(worst) : 0.0) + 0.05 * over;
        if (iter == 0 || score < bestScore - 1e-9 || (score <= 0 && bestScore <= 0 && worst < bestWorst)) {
            bestScore = score;
            bestWorst = worst;
            best = q;
        }
        if (worst <= 1) break;
        // amplify the bands over their allowed noise
        bool all = true, any = false, stuck = false;
        for (int f = 0; f < L.n; ++f) {
            if (L.maxSf[f] == 0 || G.en[f] <= 0) continue;
            if (nmr[f] > 1) {
                if (q.sf[f] + 1 > L.maxSf[f]) stuck = true;
                any = true;
            } else {
                all = false;
            }
        }
        if (!any || all) break;
        if (stuck) {
            if (q.sfScale) break;
            q.sfScale = 1;  // coarser scalefactor steps: half the values (rounded up)
            for (int f = 0; f < L.n; ++f) q.sf[f] = (q.sf[f] + 1) / 2;
        }
        for (int f = 0; f < L.n; ++f)
            if (L.maxSf[f] && G.en[f] > 0 && nmr[f] > 1 && q.sf[f] < L.maxSf[f]) q.sf[f]++;
        p2 = part2Bits(q);
        if (p2 < 0 || p2 >= target) break;
        while (gg < 255 && !fits(gg, p2)) ++gg;
        if (gg >= 255) break;
    }
    q = best;
    quantize(G, q);
    q.part2 = part2Bits(q);
    q.part23 = q.part2 + countBits(q, true);
    worstNmr = bestWorst;
}

void putPair(BitWriter& bw, int sel, int x, int y) {
    const HTab& t = htabs().t[sel];
    int ax = std::abs(x), ay = std::abs(y);
    int ex = ax, ey = ay;
    if (t.lin) {
        ex = std::min(ax, 15);
        ey = std::min(ay, 15);
    }
    int i = ex * t.dim + ey;
    bw.put(t.c[i], t.l[i]);
    if (t.lin && ax >= 15) bw.put((uint32_t)(ax - 15), t.lin);
    if (ax) bw.put(x < 0, 1);
    if (t.lin && ay >= 15) bw.put((uint32_t)(ay - 15), t.lin);
    if (ay) bw.put(y < 0, 1);
}

void Encoder::writeGranule(BitWriter& bw, const Coded& q) const {
    const Layout& L = *q.L;
    // scalefactors
    if (!lsf) {
        int s1 = kSlen[0][q.sfc], s2 = kSlen[1][q.sfc];
        for (int f = 0; f < L.n; ++f) {
            if (L.part[f] < 0) continue;
            bw.put((uint32_t)q.sf[f], L.part[f] < 2 ? s1 : s2);
        }
    } else {
        int sfc = q.sfc;
        int sl[4] = {(sfc >> 4) / 5, (sfc >> 4) % 5, (sfc & 15) >> 2, sfc & 3};
        for (int f = 0; f < L.n; ++f) {
            if (L.part[f] < 0) continue;
            bw.put((uint32_t)q.sf[f], sl[L.part[f]]);
        }
    }
    // big values
    const int* ix = q.ix;
    int big = q.bigValues * 2;
    int r1, r2;
    if (q.blockType != NORMAL) {
        r1 = 0;
        if (q.blockType == SHORT) {
            for (int f = 0; f < 9; ++f) r1 += layoutS.width[f];
        } else {
            r1 = sfbL[8];
        }
        r1 = std::min(r1, big);
        r2 = big;
    } else {
        r1 = std::min(sfbL[std::min(22, q.region0 + 1)], big);
        r2 = std::min(sfbL[std::min(22, q.region0 + q.region1 + 2)], big);
    }
    int bounds[3] = {r1, r2, big};
    int i = 0;
    for (int r = 0; r < 3; ++r) {
        int sel = q.tables[r];
        if (sel == 0) {
            i = std::max(i, bounds[r]);
            continue;
        }
        for (; i < bounds[r]; i += 2) putPair(bw, sel, ix[i], ix[i + 1]);
    }
    i = big;
    // count1: up to the last nonzero
    int last = 576;
    while (last > i && ix[last - 1] == 0) --last;
    for (; i < last; i += 4) {
        int v = std::abs(ix[i]), w = std::abs(ix[i + 1]), x = std::abs(ix[i + 2]), y = std::abs(ix[i + 3]);
        int p = v * 8 + w * 4 + x * 2 + y;
        if (q.count1Table) bw.put((uint32_t)(15 - p), 4);
        else bw.put(kHcA[p], kHlA[p]);
        for (int k = 0; k < 4; ++k)
            if (ix[i + k]) bw.put(ix[i + k] < 0, 1);
    }
}

// ---------------------------------------------------------------- frames
bool Encoder::encode(const float* in, int64_t frames, int inStride, std::vector<uint8_t>& out, std::string& error,
                     const std::function<bool(double)>& progress) {
    src = in;
    srcFrames = frames;
    stride = inStride;
    const int frameSamples = 576 * ngr;
    const int64_t codecDelay = 1057;  // analysis + synthesis filterbank 481, hybrid 576
    const int encDelay = (int)(kPad + codecDelay - 529);
    // frames so that every input sample comes out of the decoder (and the padding is at least 529)
    int64_t nFrames = (kPad + codecDelay + frames + frameSamples - 1) / frameSamples;
    const int64_t total = nFrames * frameSamples;
    const int padding = (int)(total - encDelay - frames);
    const int granules = (int)(nFrames * ngr);

    std::vector<uint8_t> flags;
    attackFlags(granules, flags);
    std::vector<uint8_t> types((size_t)granules);
    for (int g = 0; g < granules; ++g) {
        int prev = g > 0 ? (int)types[(size_t)g - 1] : (int)NORMAL;
        if (flags[(size_t)g]) types[(size_t)g] = SHORT;
        else if (flags[(size_t)g + 1]) types[(size_t)g] = prev == SHORT ? SHORT : START;
        else types[(size_t)g] = prev == SHORT ? STOP : NORMAL;
    }

    // the Info frame first (filled in at the end), at a bitrate that holds the tag
    const int* rates = lsf ? kBitratesV2 : kBitratesV1;
    const int bytesPerKbps = lsf ? 72000 : 144000;
    int infoBr = brIdx;
    while (infoBr < 14 && bytesPerKbps * rates[infoBr] / sr < 4 + sideBytes + 120 + 36) ++infoBr;
    const int infoBytes = bytesPerKbps * rates[infoBr] / sr;
    out.assign((size_t)infoBytes, 0);

    Analysis ana[2];
    double sbPrev[2][32][18], sbCur[2][32][18];
    memset(sbPrev, 0, sizeof(sbPrev));
    struct Hole { size_t pos; int len; };
    std::deque<Hole> holes;
    int holeBytes = 0;
    int64_t padAcc = 0;
    const int64_t padRem = (int64_t)bytesPerKbps * kbps % sr;
    std::vector<GrCh> grc((size_t)ngr * 2);
    std::vector<Coded> coded((size_t)ngr * 2);
    int g = 0;
    std::vector<float> pcm(32);
    for (int64_t fr = 0; fr < nFrames; ++fr) {
        if (progress && (fr & 63) == 0 && !progress((double)fr / nFrames)) {
            error = "cancelled";
            return false;
        }
        int pad = 0;
        if (padRem) {
            padAcc += padRem;
            if (padAcc >= sr) {
                padAcc -= sr;
                pad = 1;
            }
        }
        const int frameBytes = bytesPerKbps * kbps / sr + pad;
        const int slotBytes = frameBytes - 4 - sideBytes;
        // analysis of the frame's granules
        for (int gr = 0; gr < ngr; ++gr, ++g) {
            const int bt = types[(size_t)g];
            for (int c = 0; c < nch; ++c) {
                for (int t = 0; t < 18; ++t) {
                    int64_t at = (int64_t)576 * g + 32 * t;
                    for (int j = 0; j < 32; ++j) pcm[j] = sample(c, at + j);
                    double S[32];
                    ana[c].run(pcm.data(), S);
                    for (int s = 0; s < 32; ++s) sbCur[c][s][t] = (s & 1) && (t & 1) ? -S[s] : S[s];
                }
            }
            double ratioL[2][22], pe[2];
            psyLong(g, ratioL, pe);
            double ratioS[2][13][3];
            if (bt == SHORT) psyShort(g, ratioS);
            for (int c = 0; c < nch; ++c) {
                GrCh& G = grc[(size_t)(gr * 2 + c)];
                mdct(sbPrev[c], sbCur[c], bt, sfbS, G.xr);
                memcpy(sbPrev[c], sbCur[c], sizeof(sbPrev[c]));
                // lowpass
                if (bt != SHORT) {
                    for (int i = lowpassStart; i < 576; ++i) {
                        double t = i >= lowpassLine ? 0.0 : 0.5 + 0.5 * std::cos(kPi * (i - lowpassStart + 0.5) / (lowpassLine - lowpassStart));
                        G.xr[i] = (float)(G.xr[i] * t);
                    }
                } else {
                    int lineS = (lowpassLine + 2) / 3, startS = lowpassStart / 3;
                    for (int b = 0; b < 13; ++b) {
                        int a = sfbS[b], wd = sfbS[b + 1] - a;
                        for (int w = 0; w < 3; ++w)
                            for (int j = 0; j < wd; ++j) {
                                int line = a + j;
                                if (line < startS) continue;
                                double t = line >= lineS ? 0.0 : 0.5 + 0.5 * std::cos(kPi * (line - startS + 0.5) / std::max(1, lineS - startS));
                                float& v = G.xr[3 * a + w * wd + j];
                                v = (float)(v * t);
                            }
                    }
                }
                // allowed noise per band: the model's ratio of the band's energy (with the margin)
                const Layout& L = bt == SHORT ? layoutS : layoutL;
                double m = std::pow(10.0, -marginDb / 10.0);
                for (int f = 0; f < L.n; ++f) {
                    double en = 0;
                    for (int i = L.start[f], e = i + L.width[f]; i < e; ++i) en += (double)G.xr[i] * G.xr[i];
                    G.en[f] = en;
                    double r = bt == SHORT ? ratioS[c][f / 3][f % 3] : ratioL[c][f];
                    r = std::min(r * m, maxRatio);
                    G.xmin[f] = std::max(r * en, 1e-12 * L.width[f]);
                }
                G.pe = pe[c];
            }
        }
        // mid/side for this frame, when it needs fewer bits
        bool ms = false;
        if (nch == 2) {
            double costLR = 0, costMS = 0;
            for (int gr = 0; gr < ngr; ++gr) {
                GrCh& A = grc[(size_t)gr * 2];
                GrCh& B = grc[(size_t)gr * 2 + 1];
                const Layout& L = types[(size_t)(g - ngr + gr)] == SHORT ? layoutS : layoutL;
                for (int f = 0; f < L.n; ++f) {
                    double em = 0, es = 0;
                    for (int i = L.start[f], e = i + L.width[f]; i < e; ++i) {
                        double mm = (A.xr[i] + B.xr[i]), ss = (A.xr[i] - B.xr[i]);
                        em += mm * mm;
                        es += ss * ss;
                    }
                    em *= 0.5;
                    es *= 0.5;
                    double xm = std::min(A.xmin[f], B.xmin[f]);
                    int w = L.width[f];
                    costLR += w * (std::log2(std::max(1.0, A.en[f] / A.xmin[f])) + std::log2(std::max(1.0, B.en[f] / B.xmin[f])));
                    costMS += w * (std::log2(std::max(1.0, em / xm)) + std::log2(std::max(1.0, es / xm)));
                }
            }
            ms = costMS < costLR;
            if (ms) {
                const float r2 = 0.70710678118654752f;
                for (int gr = 0; gr < ngr; ++gr) {
                    GrCh& A = grc[(size_t)gr * 2];
                    GrCh& B = grc[(size_t)gr * 2 + 1];
                    const Layout& L = types[(size_t)(g - ngr + gr)] == SHORT ? layoutS : layoutL;
                    for (int i = 0; i < 576; ++i) {
                        float l = A.xr[i], r = B.xr[i];
                        A.xr[i] = (l + r) * r2;
                        B.xr[i] = (l - r) * r2;
                    }
                    for (int f = 0; f < L.n; ++f) {
                        double xm = std::min(A.xmin[f], B.xmin[f]);
                        A.xmin[f] = B.xmin[f] = xm;
                        double em = 0, es = 0;
                        for (int i = L.start[f], e = i + L.width[f]; i < e; ++i) {
                            em += (double)A.xr[i] * A.xr[i];
                            es += (double)B.xr[i] * B.xr[i];
                        }
                        A.en[f] = em;
                        B.en[f] = es;
                    }
                    double pa = 0, pb = 0;
                    for (int f = 0; f < L.n; ++f) {
                        pa += L.width[f] * std::log2(std::max(1.0, A.en[f] / A.xmin[f]));
                        pb += L.width[f] * std::log2(std::max(1.0, B.en[f] / B.xmin[f]));
                    }
                    A.pe = pa;
                    B.pe = pb;
                }
            }
        }
        // bits: this frame's slot and what the reservoir holds
        const int meanBits = 8 * slotBytes;
        int resv = 8 * holeBytes;
        int avail = resv + meanBits;
        int used = 0;
        bool distorted = false;
        for (int gr = 0; gr < ngr; ++gr) {
            int meanGr = meanBits / ngr;
            double pe = 0;
            for (int c = 0; c < nch; ++c) pe += grc[(size_t)(gr * 2 + c)].pe;
            peAvg = 0.95 * peAvg + 0.05 * pe;
            int resvNow = resv + (gr * meanGr) - used;  // (what's left of the reservoir)
            int overflow = std::max(0, resvNow - (int)(0.9 * resvMaxBits));
            int extra = std::max(0, std::min(resvNow, (int)(0.6 * resvMaxBits)) - overflow);
            double hard = std::clamp(pe / std::max(peAvg, 1.0) - 1.0, 0.0, 1.5);
            int budget = meanGr + overflow + (int)(extra * std::min(1.0, hard));
            budget = std::min(budget, avail - used - (ngr - 1 - gr) * meanGr);
            double share[2] = {1, 0};
            if (nch == 2) {
                double p0 = std::max(grc[(size_t)gr * 2].pe, 0.15 * pe + 1), p1 = std::max(grc[(size_t)gr * 2 + 1].pe, 0.15 * pe + 1);
                share[0] = p0 / (p0 + p1);
                share[1] = 1 - share[0];
            }
            int left = budget;
            for (int c = 0; c < nch; ++c) {
                Coded& q = coded[(size_t)(gr * 2 + c)];
                q.blockType = types[(size_t)(g - ngr + gr)];
                q.L = q.blockType == SHORT ? &layoutS : &layoutL;
                int target = c == nch - 1 ? left : (int)(budget * share[c]);
                target = std::min(std::max(target, 0), 4095);
                double worst;
                GrCh& G = grc[(size_t)(gr * 2 + c)];
                for (int i = 0; i < 576; ++i) {
                    float x = std::fabs(G.xr[i]);
                    G.xr34[i] = std::sqrt(x * std::sqrt(x));
                }
                outerLoop(G, q, target, worst);
                if (worst > 1) distorted = true;
                left -= q.part23;
                used += q.part23;
            }
        }
        // the frame: main data (to whole bytes), then header and side info
        BitWriter md;
        for (int gr = 0; gr < ngr; ++gr)
            for (int c = 0; c < nch; ++c) {
                size_t before = md.bits;
                writeGranule(md, coded[(size_t)(gr * 2 + c)]);
                coded[(size_t)(gr * 2 + c)].part23 = (int)(md.bits - before);
            }
        md.align();
        if ((int)md.buf.size() > holeBytes + slotBytes) {
            error = "MP3 encoder: bit budget exceeded";
            return false;
        }
        BitWriter hs;
        hs.put(0x7FF, 11);
        hs.put(version == 0 ? 3 : version == 1 ? 2 : 0, 2);
        hs.put(1, 2);  // layer III
        hs.put(1, 1);  // no CRC
        hs.put((uint32_t)brIdx, 4);
        hs.put((uint32_t)(table % 3), 2);
        hs.put((uint32_t)pad, 1);
        hs.put(0, 1);
        hs.put(nch == 2 ? 1 : 3, 2);  // joint stereo / mono
        hs.put(ms ? 2 : 0, 2);
        hs.put(0, 1);
        hs.put(1, 1);  // original
        hs.put(0, 2);
        // side info
        hs.put((uint32_t)holeBytes, lsf ? 8 : 9);
        hs.put(0, lsf ? (nch == 1 ? 1 : 2) : (nch == 1 ? 5 : 3));
        if (!lsf)
            for (int c = 0; c < nch; ++c) hs.put(0, 4);  // scfsi
        for (int gr = 0; gr < ngr; ++gr)
            for (int c = 0; c < nch; ++c) {
                const Coded& q = coded[(size_t)(gr * 2 + c)];
                hs.put((uint32_t)q.part23, 12);
                hs.put((uint32_t)q.bigValues, 9);
                hs.put((uint32_t)q.gg, 8);
                hs.put((uint32_t)q.sfc, lsf ? 9 : 4);
                if (q.blockType != NORMAL) {
                    hs.put(1, 1);
                    hs.put((uint32_t)q.blockType, 2);
                    hs.put(0, 1);  // not mixed
                    hs.put((uint32_t)q.tables[0], 5);
                    hs.put((uint32_t)q.tables[1], 5);
                    for (int w = 0; w < 3; ++w) hs.put(0, 3);  // subblock gains
                } else {
                    hs.put(0, 1);
                    for (int r = 0; r < 3; ++r) hs.put((uint32_t)q.tables[r], 5);
                    hs.put((uint32_t)q.region0, 4);
                    hs.put((uint32_t)q.region1, 3);
                }
                if (!lsf) hs.put(0, 1);  // preflag
                hs.put((uint32_t)q.sfScale, 1);
                hs.put((uint32_t)q.count1Table, 1);
            }
        size_t start = out.size();
        out.resize(start + frameBytes, 0);
        memcpy(out.data() + start, hs.buf.data(), hs.buf.size());
        holes.push_back({start + 4 + sideBytes, slotBytes});
        holeBytes += slotBytes;
        for (uint8_t byte : md.buf) {
            Hole& h = holes.front();
            out[h.pos++] = byte;
            if (--h.len == 0) holes.pop_front();
        }
        holeBytes -= (int)md.buf.size();
        int stuffed = 0;
        while (holeBytes * 8 > resvMaxBits) {  // what the reservoir can't hold stays zero (ancillary data)
            Hole& h = holes.front();
            int drop = std::min(h.len, holeBytes - resvMaxBits / 8);
            h.pos += drop;
            h.len -= drop;
            holeBytes -= drop;
            stuffed += drop;
            if (h.len == 0) holes.pop_front();
        }
        // the margin under the masking threshold follows the bits: spare bits make it stricter
        if (stuffed > 0) marginDb = std::min(marginDb + 1.0, 30.0);
        else if (distorted) marginDb = std::max(marginDb - 0.5, 0.0);
    }
    if (progress && !progress(1.0)) {
        error = "cancelled";
        return false;
    }

    // the Info frame: Xing header (frames, bytes, TOC), LAME tag with the delay and padding
    {
        uint8_t* f = out.data();
        BitWriter hs;
        hs.put(0x7FF, 11);
        hs.put(version == 0 ? 3 : version == 1 ? 2 : 0, 2);
        hs.put(1, 2);
        hs.put(1, 1);
        hs.put((uint32_t)infoBr, 4);
        hs.put((uint32_t)(table % 3), 2);
        hs.put(0, 1);
        hs.put(0, 1);
        hs.put(nch == 2 ? 1 : 3, 2);
        hs.put(0, 2);
        hs.put(0, 1);
        hs.put(1, 1);
        hs.put(0, 2);
        memcpy(f, hs.buf.data(), 4);
        uint8_t* p = f + 4 + sideBytes;
        auto be32 = [](uint8_t* d, uint32_t v) {
            d[0] = (uint8_t)(v >> 24);
            d[1] = (uint8_t)(v >> 16);
            d[2] = (uint8_t)(v >> 8);
            d[3] = (uint8_t)v;
        };
        memcpy(p, "Info", 4);
        be32(p + 4, 0x0F);  // frames, bytes, TOC, quality
        be32(p + 8, (uint32_t)nFrames);
        be32(p + 12, (uint32_t)out.size());
        for (int i = 0; i < 100; ++i) p[16 + i] = (uint8_t)(i * 256 / 100);
        be32(p + 116, 0);
        uint8_t* l = p + 120;
        memcpy(l, "LAMETDAW ", 9);  // ("LAME": the tag format's signature, what gapless decoders look for)
        l[9] = 0x01;                 // tag revision 0, CBR
        l[10] = (uint8_t)std::min(255.0, lowpassHz / 100 + 0.5);
        // l[11..18]: peak and replay gains (none); l[19] flags; l[20] bitrate
        l[20] = (uint8_t)std::min(255, kbps);
        l[21] = (uint8_t)(encDelay >> 4);
        l[22] = (uint8_t)((encDelay & 15) << 4 | (padding >> 8));
        l[23] = (uint8_t)(padding & 255);
        be32(l + 28, (uint32_t)out.size());
        uint16_t music = crc16(out.data() + infoBytes, out.size() - infoBytes, 0);
        l[32] = (uint8_t)(music >> 8);
        l[33] = (uint8_t)music;
        size_t tagEnd = (size_t)(l + 34 - f);
        uint16_t tc = crc16(f, tagEnd, 0);
        l[34] = (uint8_t)(tc >> 8);
        l[35] = (uint8_t)tc;
    }
    return true;
}

// ---------------------------------------------------------------- resampling (rates MP3 doesn't have)
// windowed sinc (Blackman-Harris, 32 zero crossings of the lower rate's Nyquist), linear between
// 256 phases a crossing
std::vector<float> resampleSinc(const float* in, int64_t frames, int ch, int stride, int from, int to, int64_t& outFrames) {
    double ratio = (double)to / from;
    double cutoff = std::min(1.0, ratio) * 0.97;
    const int zc = 32, phases = 256;
    int half = (int)std::ceil(zc / cutoff);
    std::vector<float> kernel((size_t)(zc * phases + 2));
    for (int i = 0; i <= zc * phases + 1; ++i) {
        double x = (double)i / phases;  // in units of the cutoff's period
        double s = x == 0 ? 1.0 : std::sin(kPi * x) / (kPi * x);
        double t = std::min(1.0, x / zc);
        double w = 0.35875 + 0.48829 * std::cos(kPi * t) + 0.14128 * std::cos(2 * kPi * t) + 0.01168 * std::cos(3 * kPi * t);
        kernel[(size_t)i] = (float)(s * w * (x >= zc ? 0 : 1));
    }
    outFrames = (int64_t)std::llround((double)frames * ratio);
    std::vector<float> out((size_t)(outFrames * ch));
    for (int64_t m = 0; m < outFrames; ++m) {
        double pos = m / ratio;
        int64_t c0 = (int64_t)std::floor(pos);
        for (int c = 0; c < ch; ++c) {
            double acc = 0, wsum = 0;
            for (int64_t k = c0 - half + 1; k <= c0 + half; ++k) {
                double x = std::fabs(pos - k) * cutoff * phases;
                int xi = (int)x;
                if (xi >= zc * phases) continue;
                double fr = x - xi;
                double w = kernel[(size_t)xi] + (kernel[(size_t)xi + 1] - kernel[(size_t)xi]) * fr;
                wsum += w;
                if (k >= 0 && k < frames) acc += w * in[k * stride + c];
            }
            out[(size_t)(m * ch + c)] = (float)(wsum > 0 ? acc / wsum : 0.0);
        }
    }
    return out;
}

}  // namespace

int mp3EncodedRate(int sampleRate) {
    for (int r : kRates)
        if (r == sampleRate) return sampleRate;
    if (sampleRate > 48000) return sampleRate % 11025 == 0 ? 44100 : 48000;
    int best = 48000;
    for (int r : kRates)
        if (r >= sampleRate && r < best) best = r;
    return best;
}

int mp3EncodedBitrate(int sampleRate, int kbps) {
    int rate = mp3EncodedRate(sampleRate);
    bool lsf = rate < 32000;
    const int* rates = lsf ? kBitratesV2 : kBitratesV1;
    int best = 1;
    for (int i = 1; i < 15; ++i)
        if (std::abs(rates[i] - kbps) < std::abs(rates[best] - kbps)) best = i;
    return rates[best];
}

bool encodeMp3ToMemory(std::vector<uint8_t>& out, const float* interleaved, size_t frames, int channels,
                       int sampleRate, int kbps, std::string& error, std::function<bool(double)> progress) {
    out.clear();
    if (channels <= 0 || sampleRate <= 0 || (!interleaved && frames)) {
        error = "MP3 encoder: no audio";
        return false;
    }
    int rate = mp3EncodedRate(sampleRate);
    int nch = channels >= 2 ? 2 : 1;
    const float* data = interleaved;
    int64_t n = (int64_t)frames;
    int stride = channels;
    std::vector<float> resampled;
    if (rate != sampleRate) {
        int64_t m = 0;
        resampled = resampleSinc(interleaved, n, nch, channels, sampleRate, rate, m);
        data = resampled.data();
        n = m;
        stride = nch;
    }
    auto enc = std::make_unique<Encoder>();
    if (!enc->setup(rate, nch, kbps, error)) return false;
    return enc->encode(data, n, stride, out, error, progress);
}

bool encodeMp3(const std::string& path, const float* interleaved, size_t frames, int channels, int sampleRate,
               int kbps, std::string& error, std::function<bool(double)> progress) {
    std::vector<uint8_t> data;
    if (!encodeMp3ToMemory(data, interleaved, frames, channels, sampleRate, kbps, error, progress)) return false;
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        error = "can't write " + path;
        return false;
    }
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = fclose(f) == 0 && ok;
    if (!ok) {
        error = "can't write " + path;
        remove(path.c_str());
    }
    return ok;
}
