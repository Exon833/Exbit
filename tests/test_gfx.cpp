// Tests of the graphics layer: colours, PNG/zlib, the rasterizer, fonts, SVG icons. Also writes
// sample renderings to build/gfx-samples/*.png for looking at.
#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "core/util.h"
#include "gfx/font.h"
#include "gfx/geom.h"
#include "gfx/image.h"
#include "gfx/painter.h"
#include "gfx/svg.h"

// PNG fixtures written by Python's zlib (all filter types, interlacing, bit depths, palette+tRNS)
static const char* k_rgba16i = "89504e470d0a1a0a0000000d4948445200000007000000051006000001ae0d1a0d000000e44944415478da636060e0de215ea0acc1d0b36046c41281f52718fc14226fa44e28f4b875e059c52783bf2f18dc028238620f64565cfa706fc5ab846f128c530a1668ac7eb07d46ef829e05bd40c8a02e6074c2aec1cb2237a152a2e5c2848ecd1bf6679c56b876e31703eb0ec102590d06650dbd075633dc02322b4a0d1a5ef42c587f6277c3718b4b1f18bd2c423f24aec84d700b700f7083427720645217d010508742044b5d8041b843de41eb87d906b30d4e1920a7421c5bcb50cbd0b103e43088d30e071c0e38cf01f202c4139c0b1841f6833ca2ac810c55805059035d0c99070087f26bc9d0d3f3770000000049454e44ae426082";
static const char* k_pal4 = "89504e470d0a1a0a0000000d4948445200000009000000040403000000ae2108fd00000030504c544500ff0010ef2820df5030cf7840bfa050afc8609ff0708f18807f40906f68a05f90b04fb8c03fe0d02f08e01f30f00f5859d02d890000001074524e5300112233445566778899aabbccddeeff769501150000001e4944415478da636054764d6f6034515252126332060203e6746d6d690d00319e040323a8c2470000000049454e44ae426082";
static const char* k_grey2i = "89504e470d0a1a0a0000000d494844520000000500000003020000000143eab213000000154944415478da6360008206860e8602c60b0c390e00104c02f6b95012450000000049454e44ae426082";
static const char* k_ga8 = "89504e470d0a1a0a0000000d4948445200000006000000040804000000070d86c8000000264944415478da636060d0600de0aae05f2072429291d14683150699186d109099a94254110601be8606be4918e0240000000049454e44ae426082";
static const char* k_rgb8key = "89504e470d0a1a0a0000000d494844520000000a000000090802000001f3c31ad20000000674524e5300280032004db0956f870000008a4944415478da758db10dc3300c042f4e2ad76a388155b3f606c1031a820ba4d310ae536b080fe1b12237466c24c401249e781ca0866e6c7d41e967d904a1126a2132aa3dcc3d80816007577615577535efedb7321c90d08c022d68edede4a7ff80f3cd7d8c2919078f3d3e8d2999b26936155398aa693135d36a5dff54daa5bfb9daae725ed368fce3036283247437a2bfee0000000049454e44ae426082";
static const char* k_zlibdyn = "78da0bc9ccab5470710c57c8cc4bcb492c495528492d2eb152484c4a06a39451c6106530303231b3b0b2b173707271f3f0f2f10b080a098b888a894b484a49cbc8cac92b282a29aba8aaa96b686a69ebe8eae91b181a199b989a995b585a59dbd8dad93b383a39bbb8bab97b787a79fbf8faf9070406058784868547444645c7c4c6c52700ad49494d4bcfc8cccacec9cdcb2f282c2a2e292d2bafa8acaaaea9adab6f686c6a6e696d6befe8eceaeee9edeb9f3071d2e42953a74d9f3173d6ec3973e7cd5fb070d1e2254b972d5fb172d5ea356bd7addfb071d3e62d5bb76ddfb173d7ee3d7bf7ed3f70f0d0e123478f1d3f71f2d4e93367cf9dbf70f1d2e52b57af5dbf71f3d6ed3b77efdd7ff0f0d1e3274f9f3d7ff1f2d5eb376fdfbdfff0f1d3e72f5fbf7dfff1f3d7ef3f7ffffd0700308333e3";

static std::vector<uint8_t> unhex(const char* h) {
    std::vector<uint8_t> v;
    for (size_t i = 0; h[i] && h[i + 1]; i += 2) {
        auto d = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
        v.push_back((uint8_t)(d(h[i]) * 16 + d(h[i + 1])));
    }
    return v;
}

static std::string sampleDir() {
    mkdir("build", 0755);
    mkdir("build/gfx-samples", 0755);
    return "build/gfx-samples/";
}

static double ms(std::chrono::steady_clock::time_point a) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
}

static void testColors() {
    bool ok = false;
    Color c = Color::fromHex("#12abEF", &ok);
    CHECK(ok);
    CHECK(c == Color(0x12, 0xab, 0xef, 255));
    CHECK(c.hex() == "#12abef");
    Color t = Color::fromHex("#11223380", &ok);
    CHECK(ok && t.a == 0x80);
    CHECK(t.hex() == "#11223380");
    CHECK(t.hexRgb() == "#112233");
    Color::fromHex("12abef", &ok);
    CHECK(!ok);
    Color::fromHex("#12abeg", &ok);
    CHECK(!ok);
    // HSV / HSL
    float h, s, v;
    Color(255, 0, 0).toHsv(h, s, v);
    CHECK_NEAR(h, 0, 0.01);
    CHECK_NEAR(s, 1, 0.001);
    CHECK_NEAR(v, 1, 0.001);
    Color(0, 0, 255).toHsv(h, s, v);
    CHECK_NEAR(h, 240, 0.01);
    Color(128, 128, 128).toHsv(h, s, v);
    CHECK(h == -1);
    CHECK(Color::fromHsv(120, 1, 1) == Color(0, 255, 0));
    CHECK(Color::fromHsv(-1, 0, 0.5f) == Color(128, 128, 128));
    for (int r = 0; r < 256; r += 51)
        for (int g = 0; g < 256; g += 51)
            for (int b = 0; b < 256; b += 51) {
                Color o(r, g, b);
                o.toHsv(h, s, v);
                CHECK(Color::fromHsv(h, s, v) == o);
                float hl, sl, l;
                o.toHsl(hl, sl, l);
                CHECK(Color::fromHsl(hl, sl, l) == o);
            }
    float l;
    Color(255, 0, 0).toHsl(h, s, l);
    CHECK_NEAR(l, 0.5, 0.001);
    CHECK_NEAR(Color(255, 255, 255).lightnessF(), 1, 1e-6);
    // QColor::lighter / darker
    CHECK(Color(255, 0, 0).lighter(150) == Color(255, 128, 128));
    CHECK(Color(100, 150, 200).darker(200) == Color(50, 75, 100));
    CHECK(Color(100, 150, 200).lighter(100) == Color(100, 150, 200));
    CHECK(Color(0, 0, 0).lighter(150) == Color(0, 0, 0));
    // mix (Python round: halves to even), blending, contrast
    CHECK(mixColor(Color(0, 0, 0), Color(255, 255, 255), 0.5f) == Color(128, 128, 128));
    CHECK(mixColor(Color(0, 0, 0), Color(1, 3, 5), 0.5f) == Color(0, 2, 2));
    CHECK(mixColor(Color(10, 20, 30, 40), Color(10, 20, 30), 0) == Color(10, 20, 30, 255));
    CHECK(blendOver(Color(255, 255, 255, 128), Color(0, 0, 0)) == Color(128, 128, 128));
    CHECK_NEAR(Color(255, 255, 255).luminance(), 1, 1e-6);
    CHECK_NEAR(Color(0, 0, 0).luminance(), 0, 1e-6);
    CHECK_NEAR(contrastRatio(Color(0, 0, 0), Color(255, 255, 255)), 21, 1e-4);
    CHECK_NEAR(contrastRatio(Color(118, 118, 118), Color(255, 255, 255)), 4.54, 0.01);
    // premultiplied pixels
    CHECK(Color(255, 0, 0, 128).premul() == 0x80800000u);
    CHECK(Color::fromPremul(0x80800000u) == Color(255, 0, 0, 128));
}

static void testZlib() {
    // a stored block, built by hand: "hello"
    {
        std::vector<uint8_t> z = {0x78, 0x01, 0x01, 0x05, 0x00, 0xfa, 0xff, 'h', 'e', 'l', 'l', 'o'};
        const uint8_t* d = (const uint8_t*)"hello";
        uint32_t a = 1, b = 0;
        for (int i = 0; i < 5; i++) { a = (a + d[i]) % 65521; b = (b + a) % 65521; }
        uint32_t ad = b << 16 | a;
        for (int k = 3; k >= 0; k--) z.push_back((uint8_t)(ad >> (8 * k)));
        std::vector<uint8_t> out;
        CHECK(zlibInflate(z.data(), z.size(), out));
        CHECK(std::string(out.begin(), out.end()) == "hello");
        z[9] ^= 1;  // corrupt: the checksum no longer matches
        CHECK(!zlibInflate(z.data(), z.size(), out));
    }
    // a fixed-Huffman stream built by hand: "a" (literal 97 = code 0x30+97 in 8 bits), end of block
    {
        // bits LSB first: BFINAL=1, BTYPE=01, then codes MSB first
        std::vector<uint8_t> bits;
        auto put = [&](uint32_t v, int n, bool msbFirst) {
            for (int i = 0; i < n; i++) bits.push_back(msbFirst ? (v >> (n - 1 - i)) & 1 : (v >> i) & 1);
        };
        put(1, 1, false);
        put(1, 2, false);
        put(0x30 + 'a', 8, true);
        put(0x30 + 'b', 8, true);
        // a match: length 4 (code 258 = 7-bit 0000010), distance 2 (code 1 = 5-bit 00001): "abab" -> "ababab"
        put(2, 7, true);
        put(1, 5, true);
        put(0, 7, true);  // end of block (256)
        std::vector<uint8_t> z = {0x78, 0x9c};
        for (size_t i = 0; i < bits.size(); i += 8) {
            uint8_t byte = 0;
            for (size_t k = 0; k < 8 && i + k < bits.size(); k++) byte |= bits[i + k] << k;
            z.push_back(byte);
        }
        const char* expect = "ababab";
        uint32_t a = 1, b = 0;
        for (int i = 0; i < 6; i++) { a = (a + (uint8_t)expect[i]) % 65521; b = (b + a) % 65521; }
        uint32_t ad = b << 16 | a;
        for (int k = 3; k >= 0; k--) z.push_back((uint8_t)(ad >> (8 * k)));
        std::vector<uint8_t> out;
        CHECK(zlibInflate(z.data(), z.size(), out));
        CHECK(std::string(out.begin(), out.end()) == expect);
    }
    // Python's zlib, dynamic Huffman
    {
        std::vector<uint8_t> z = unhex(k_zlibdyn), out;
        CHECK(zlibInflate(z.data(), z.size(), out));
        std::string expect = "Tiny DAW inflate test: ";
        for (int i = 0; i < 50; i++) expect += "abcabcabd";
        for (int i = 0; i < 256; i++) expect += (char)i;
        CHECK(std::string(out.begin(), out.end()) == expect);
    }
    // round trips of our deflate at every level, on text, noise, runs and nothing
    {
        std::vector<std::vector<uint8_t>> inputs;
        inputs.push_back({});
        inputs.push_back({42});
        std::vector<uint8_t> text;
        const char* words[] = {"track ", "clip ", "mixer ", "tempo ", "plugin ", "automation ", "\n"};
        uint32_t seed = 1;
        for (int i = 0; i < 20000; i++) {
            seed = seed * 1103515245 + 12345;
            const char* w = words[(seed >> 16) % 7];
            text.insert(text.end(), w, w + strlen(w));
        }
        inputs.push_back(text);
        std::vector<uint8_t> noise(100000);
        for (auto& b : noise) { seed = seed * 1103515245 + 12345; b = (uint8_t)(seed >> 16); }
        inputs.push_back(noise);
        inputs.push_back(std::vector<uint8_t>(300000, 7));
        for (const auto& in : inputs)
            for (int level : {0, 1, 6, 9}) {
                std::vector<uint8_t> z = zlibDeflate(in.data(), in.size(), level), out;
                CHECK(zlibInflate(z.data(), z.size(), out));
                CHECK(out == in);
                if (in.size() == 300000 && level > 0) CHECK(z.size() < 2000);
                if (in.size() == text.size() && level >= 6) CHECK(z.size() < text.size() / 4);
                if (in.size() == noise.size()) CHECK(z.size() < noise.size() + noise.size() / 100 + 64);
            }
        printf("deflate: text %zu -> %zu bytes (level 6)\n", text.size(), zlibDeflate(text.data(), text.size()).size());
    }
    CHECK(crc32((const uint8_t*)"123456789", 9) == 0xCBF43926u);
}

static void testPng() {
    // encode -> decode
    Image img(37, 23);
    for (int y = 0; y < 23; y++)
        for (int x = 0; x < 37; x++)
            img.setPixel(x, y, Color((x * 7) & 255, (y * 11) & 255, (x * y) & 255, x < 30 ? 255 : (x * 9 + y * 3) & 255).premul());
    std::vector<uint8_t> png = encodePng(img);
    Image back;
    std::string err;
    CHECK(decodePng(png.data(), png.size(), back, &err));
    CHECK(back.width() == 37 && back.height() == 23);
    bool same = true;
    for (int y = 0; y < 23 && same; y++)
        for (int x = 0; x < 37; x++)
            if (back.pixel(x, y) != img.pixel(x, y)) same = false;
    CHECK(same);
    // a smooth image compresses well
    Image grad(256, 256);
    for (int y = 0; y < 256; y++)
        for (int x = 0; x < 256; x++) grad.setPixel(x, y, Color(x, y, 128).premul());
    CHECK(encodePng(grad).size() < 256 * 256 * 4 / 20);
    // fixtures
    Image f;
    {
        std::vector<uint8_t> d = unhex(k_rgba16i);
        CHECK(decodePng(d.data(), d.size(), f, &err));
        bool ok = f.width() == 7 && f.height() == 5;
        for (int y = 0; y < 5 && ok; y++)
            for (int x = 0; x < 7; x++) {
                int c[4];
                for (int k = 0; k < 4; k++) c[k] = (int)((((x * 9000 + y * 5000 + k * 3000) % 65536) * 255u + 32767) / 65535);
                if (f.pixel(x, y) != Color(c[0], c[1], c[2], c[3]).premul()) ok = false;
            }
        CHECK(ok);
    }
    {
        std::vector<uint8_t> d = unhex(k_pal4);
        CHECK(decodePng(d.data(), d.size(), f, &err));
        bool ok = f.width() == 9 && f.height() == 4;
        for (int y = 0; y < 4 && ok; y++)
            for (int x = 0; x < 9; x++) {
                int i = (x + y * 3) % 16;
                if (f.pixel(x, y) != Color(i * 16, 255 - i * 16, (i * 40) % 256, (i * 17) % 256).premul()) ok = false;
            }
        CHECK(ok);
    }
    {
        std::vector<uint8_t> d = unhex(k_grey2i);
        CHECK(decodePng(d.data(), d.size(), f, &err));
        bool ok = f.width() == 5 && f.height() == 3;
        for (int y = 0; y < 3 && ok; y++)
            for (int x = 0; x < 5; x++) {
                int g = ((x + y) % 4) * 85;
                if (f.pixel(x, y) != Color(g, g, g).premul()) ok = false;
            }
        CHECK(ok);
    }
    {
        std::vector<uint8_t> d = unhex(k_ga8);
        CHECK(decodePng(d.data(), d.size(), f, &err));
        bool ok = f.width() == 6 && f.height() == 4;
        for (int y = 0; y < 4 && ok; y++)
            for (int x = 0; x < 6; x++) {
                int g = (x * 40 + y) % 256, a = (y * 60 + x * 5) % 256;
                if (f.pixel(x, y) != Color(g, g, g, a).premul()) ok = false;
            }
        CHECK(ok);
    }
    {
        std::vector<uint8_t> d = unhex(k_rgb8key);
        CHECK(decodePng(d.data(), d.size(), f, &err));
        bool ok = f.width() == 10 && f.height() == 9;
        for (int y = 0; y < 9 && ok; y++)
            for (int x = 0; x < 10; x++) {
                Color c((x * 20) % 256, (y * 25) % 256, 77);
                if (c.r == 40 && c.g == 50) c.a = 0;
                if (f.pixel(x, y) != c.premul()) ok = false;
            }
        CHECK(ok);
    }
    // broken input is refused, not crashed on
    std::vector<uint8_t> cut(png.begin(), png.begin() + png.size() / 2);
    CHECK(!decodePng(cut.data(), cut.size(), f));
    CHECK(!decodePng((const uint8_t*)"nonsense", 8, f));
    // save / load
    std::string path = sampleDir() + "roundtrip.png";
    CHECK(savePng(path, img, &err));
    CHECK(loadPng(path, back, &err));
    CHECK(back.width() == 37 && back.pixel(5, 5) == img.pixel(5, 5));
    // scaling and tinting
    Image s = grad.scaled(64, 64);
    CHECK(s.width() == 64 && s.height() == 64);
    Color m = s.colorAt(32, 32);
    CHECK(std::abs(m.r - 130) <= 3 && std::abs(m.g - 130) <= 3);
    Image up = img.scaled(74, 46);
    CHECK(up.width() == 74);
    Image tinted = img.tinted(Color(10, 20, 30));
    CHECK(tinted.colorAt(3, 3) == Color(10, 20, 30, 255));
}

static int alphaSum(const Image& img, Rect r) {
    long s = 0;
    for (int y = r.y; y < r.bottom(); y++)
        for (int x = r.x; x < r.right(); x++) s += img.pixel(x, y) >> 24;
    return (int)(s / 255);
}

static void testPainter() {
    Image img(40, 30);
    {
        Painter p(img);
        p.fillRect(Rect(2, 3, 10, 1), Color(255, 0, 0));
    }
    CHECK(img.pixel(2, 3) == 0xFFFF0000u && img.pixel(11, 3) == 0xFFFF0000u);
    CHECK(img.pixel(12, 3) == 0 && img.pixel(1, 3) == 0 && img.pixel(5, 2) == 0 && img.pixel(5, 4) == 0);
    // translation, clip, opacity
    img.fill(Color(0, 0, 0, 0));
    {
        Painter p(img);
        p.translate(5, 5);
        p.setClip(Rect(0, 0, 4, 4));
        p.fillRect(Rect(-10, -10, 100, 100), Color(0, 255, 0));
        CHECK(p.clip() == Rect(0, 0, 4, 4));
        p.save();
        p.setOpacity(0.5f);
        p.fillRect(Rect(10, 10, 2, 2), Color(255, 255, 255));  // clipped away
        p.restore();
        CHECK(p.opacity() == 1);
    }
    CHECK(alphaSum(img, img.rect()) == 16);
    CHECK(img.pixel(5, 5) == 0xFF00FF00u && img.pixel(8, 8) == 0xFF00FF00u && img.pixel(9, 9) == 0);
    // a 1 px line at y + 0.5 covers exactly one row; drawRect outlines inside
    img.fill(Color(0, 0, 0, 0));
    {
        Painter p(img);
        p.drawLine(PointF(0, 4.5f), PointF(10, 4.5f), Color(255, 255, 255));
        p.drawLine(PointF(20.5f, 0), PointF(20.5f, 10), Color(255, 255, 255), 1, LineCap::Round);  // AA path
        p.drawRect(Rect(25, 20, 10, 8), Color(255, 255, 255));
    }
    CHECK(alphaSum(img, Rect(0, 4, 10, 1)) == 10 && alphaSum(img, Rect(0, 0, 18, 30)) == 10);
    CHECK(img.pixel(20, 5) >> 24 == 255 && img.pixel(19, 5) == 0 && img.pixel(21, 5) == 0);
    CHECK(alphaSum(img, Rect(25, 20, 10, 8)) == 2 * 10 + 2 * 6);
    CHECK(img.pixel(26, 21) == 0);
    // anti-aliased circle: area ~ pi r^2, symmetric, partial edge pixels
    for (float r : {5.f, 10.f, 13.3f}) {
        Image c(40, 40);
        Painter p(c);
        p.fillEllipse(RectF(20 - r, 20 - r, 2 * r, 2 * r), Color(255, 255, 255));
        long sum = 0;
        int partial = 0;
        for (int y = 0; y < 40; y++)
            for (int x = 0; x < 40; x++) {
                int a = c.pixel(x, y) >> 24;
                sum += a;
                if (a > 0 && a < 255) partial++;
            }
        CHECK_NEAR(sum / 255.0, 3.14159265 * r * r, 0.01 * 3.14159265 * r * r + 1);
        CHECK(partial > 0);
        CHECK(c.pixel(20 - (int)r + 1, 20) == c.pixel(20 + (int)r - 2, 20));
    }
    // a fractional rectangle: exact area coverage
    {
        Image c(10, 10);
        Painter p(c);
        p.fillRect(RectF(1.5f, 1.25f, 3, 2), Color(255, 255, 255));
        CHECK_NEAR(alphaSum(c, c.rect()) + 0.0, 6.0, 0.1);
        CHECK(std::abs((int)(c.pixel(1, 1) >> 24) - 96) <= 1);  // 0.5 x 0.75 of the pixel
    }
    // a translucent fill over opaque blends; a stroke of a rounded rect stays inside its box
    {
        Image c(20, 20, Color(0, 0, 0));
        Painter p(c);
        p.fillRect(Rect(0, 0, 5, 5), Color(255, 255, 255, 128));
        CHECK(c.colorAt(2, 2) == Color(128, 128, 128));
        p.strokeRoundedRect(RectF(5.5f, 5.5f, 9, 9), 3, Color(255, 0, 0));
        CHECK(c.colorAt(10, 5).r == 255 && c.colorAt(10, 4).r == 0 && c.colorAt(10, 6).r == 0);
    }
    // non-zero winding: two overlapping squares the same way round = one union
    {
        Image c(20, 20);
        Painter p(c);
        Path path;
        path.addRect(RectF(2, 2, 8, 8));
        path.addRect(RectF(6, 6, 8, 8));
        p.fillPath(path, Color(255, 255, 255));
        CHECK(alphaSum(c, c.rect()) == 64 + 64 - 16);
        // even-odd: the overlap is a hole
        c.fill(Color(0, 0, 0, 0));
        p.fillPath(path, Color(255, 255, 255), true);
        CHECK(alphaSum(c, c.rect()) == 64 + 64 - 32);
    }
    // shapes partly outside the image (left/top/right) are clipped properly
    {
        Image c(10, 10);
        Painter p(c);
        p.fillPolygon({PointF(-5, -5), PointF(20, -5), PointF(20, 20), PointF(-5, 20)}, Color(255, 255, 255));
        CHECK(alphaSum(c, c.rect()) == 100);
        c.fill(Color(0, 0, 0, 0));
        p.fillPolygon({PointF(-10, 0), PointF(5, 0), PointF(5, 10), PointF(-10, 10)}, Color(255, 255, 255));
        CHECK(alphaSum(c, c.rect()) == 50);
    }
    // images: 1:1 blending and scaled drawing
    {
        Image src(4, 4, Color(255, 0, 0, 128));
        Image dst(10, 10, Color(0, 0, 255));
        Painter p(dst);
        p.drawImage(Point(1, 1), src);
        CHECK(dst.colorAt(2, 2) == Color(128, 0, 127));
        p.drawImage(RectF(5, 5, 4, 4), Image(2, 2, Color(0, 255, 0)));
        CHECK(dst.colorAt(6, 6) == Color(0, 255, 0) && dst.colorAt(9, 9) == Color(0, 0, 255));
    }
}

static void testFonts() {
    Font f;
    CHECK(f.pixelSize() == 12);
    float w = f.width("Hello");
    CHECK(w > 15 && w < 60);
    std::vector<Font::Run> runs;
    float adv = f.layout("Hello", 0.3f, runs);
    CHECK(runs.size() == 5);
    CHECK_NEAR(adv, w, 0.001);
    CHECK(runs[0].glyph && runs[0].glyph->w > 0);
    CHECK(f.ascent() >= 9 && f.descent() >= 2 && f.lineSpacing() >= f.height());
    CHECK(f.width("") == 0);
    CHECK(f.width("Hello world") > f.width("Hello"));
    CHECK(Font::fromPoints("", 18).width("Hello") > w * 1.4f);
    // bold is wider; the mono font has equal advances
    CHECK(f.withBold(true).width("Hello") > w);
    Font mono = Font::mono();
    CHECK_NEAR(mono.width("iiii"), mono.width("WWWW"), 0.01);
    CHECK(Font::fromPoints("Consolas", 9).width("0") == mono.width("0"));
    // fallback glyphs for the interface's symbols
    const char* symbols[] = {"✕", "▾", "▸", "♩", "⅓", "■", "⟲", "⚠", "❄", "⏮", "⤢", "●", "🎹", "🔊", "↔", "…", "→"};
    int missing = 0;
    for (const char* s : symbols) {
        size_t i = 0;
        uint32_t cp = utf8Next(s, i);
        if (!f.hasGlyph(cp)) {
            missing++;
            printf("  no glyph for U+%04X\n", cp);
        }
        CHECK(f.width(s) > 0);
    }
    CHECK(f.hasGlyph(0x2715));  // ✕
    CHECK(missing <= 2);
    // elide / hit test
    std::string longText = "A rather long track name that will not fit";
    std::string el = f.elided(longText, 80);
    CHECK(f.width(el) <= 80 && el.size() < longText.size());
    CHECK(el.size() >= 3 && el.compare(el.size() - 3, 3, "\xE2\x80\xA6") == 0);
    CHECK(f.elided("abc", 200) == "abc");
    CHECK(f.hitTest("Hello", 0) == 0 && f.hitTest("Hello", 1000) == 5);
    CHECK(f.hitTest("Hello", f.width("He") + 0.5f) == 2);
    // UTF-8
    CHECK(utf8ToU32("a\xC3\xA9\xE2\x9C\x95\xF0\x9F\x8E\xB9") == std::u32string({U'a', 0xE9, 0x2715, 0x1F3B9}));
    CHECK(u32ToUtf8(U"é\U0001F3B9") == "\xC3\xA9\xF0\x9F\x8E\xB9");
    CHECK(utf8PrevBoundary("a\xC3\xA9", 3) == 1 && utf8NextBoundary("a\xC3\xA9", 1) == 3);
    size_t i = 0;
    CHECK(utf8Next("\xFF", i) == 0xFFFD && i == 1);
    std::vector<std::string> fams = Font::families();
    CHECK(!fams.empty());
    CHECK(std::find(fams.begin(), fams.end(), "DejaVu Sans") != fams.end());
    // a family from a file (a skin's @font-face): found by its new name, and fonts that asked for that name
    // before (and got the default) look it up again
    {
        const std::string mono = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf";
        Font early("Tiny Test Face", 20);
        float before = early.width("iiii");
        CHECK(!Font::addFamily("Tiny Test Face", "/no/such/font.ttf"));
        CHECK(!Font::addFamily("Tiny Test Face", "tests/test_gfx.cpp"));  // (not a font)
        CHECK(!Font::addFamily("", mono) && !Font::addFamily("Tiny Test Face", ""));
        CHECK(util::fileExists(mono));  // (DejaVu is there: see above)
        {
            CHECK(Font::addFamily("Tiny Test Face", mono));
            CHECK(Font::addFamily("Tiny Test Face", mono));  // (again: nothing changes)
            Font later("tiny test face", 20);  // (names are matched without case)
            CHECK_NEAR(later.width("iiii"), later.width("WWWW"), 0.01);  // (monospaced: the file's)
            CHECK_NEAR(later.width("0"), Font("DejaVu Sans Mono", 20).width("0"), 0.001);
            CHECK_NEAR(early.width("iiii"), early.width("WWWW"), 0.01);
            CHECK(early.width("iiii") != before);
            std::vector<std::string> now = Font::families();
            CHECK(std::count(now.begin(), now.end(), "Tiny Test Face") == 1);
            // bold: as the stylesheet says (the file is then the family's bold face as it is: not emboldened again)
            CHECK(Font::addFamily("Tiny Test Bold", mono, true));
            CHECK_NEAR(Font("Tiny Test Bold", 20, true).width("0"), Font("DejaVu Sans Mono", 20).width("0"), 0.001);
        }
    }
    // drawing matches the layout: text is drawn inside its advance
    Image img(100, 30);
    Painter p(img);
    float a2 = p.drawText(PointF(10, 20), "Hello", f, Color(255, 255, 255));
    CHECK_NEAR(a2, w, 0.001);
    CHECK(alphaSum(img, Rect(0, 0, 9, 30)) == 0 && alphaSum(img, Rect(9, 0, (int)w + 3, 30)) > 20);
    CHECK(alphaSum(img, Rect((int)(10 + w) + 2, 0, 50, 30)) == 0);
}

static void testSvg() {
    const char* names[] = {"play", "pause", "record", "return_to_start", "follow"};
    std::string dir = sampleDir();
    for (const char* n : names) {
        for (int size : {14, 64}) {
            Image img = renderSvgFile(std::string("data/icons/") + n + ".svg", size, size);
            CHECK(img.width() == size);
            int cov = alphaSum(img, img.rect());
            CHECK(cov > size * size / 8 && cov < size * size);
            savePng(dir + "icon-" + n + "-" + std::to_string(size) + ".png", img);
        }
    }
    // record is a ring: hollow in the middle
    Image rec = renderSvgFile("data/icons/record.svg", 64, 64);
    CHECK((rec.pixel(32, 32) >> 24) == 0 && (rec.pixel(32, 4) >> 24) == 255);
    // inline SVG: shapes, transforms, styles
    const char* svg =
        "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 10 10'>"
        "<rect x='1' y='1' width='3' height='3' fill='#ff0000'/>"
        "<g transform='translate(5,0)' style='fill:rgb(0,0,255)'><circle cx='2.5' cy='2.5' r='2'/></g>"
        "<polyline points='1,8 9,8' stroke='lime' stroke-width='1' fill='none'/>"
        "<path d='M6 6h3v3h-3z' fill='none' stroke='white' stroke-width='.5' opacity='0.5'/>"
        "</svg>";
    Image s = renderSvg(svg, 100, 100);
    CHECK(s.colorAt(25, 25) == Color(255, 0, 0));
    CHECK(s.colorAt(75, 25) == Color(0, 0, 255));
    CHECK(s.colorAt(50, 80) == Color(0, 255, 0));
    CHECK(s.pixel(60, 65) == 0x80808080u && s.pixel(75, 60) == 0x80808080u && s.pixel(75, 70) == 0);
    CHECK(renderSvg("not svg", 10, 10).empty());
    Image t = renderSvgTinted(svg, 20, 20, Color(1, 2, 3));
    CHECK(t.colorAt(5, 5) == Color(1, 2, 3));
}

// preserveAspectRatio: none stretches, the alignments and meet / slice place (the root's and a nested <svg>'s)
static void testSvgAspect() {
    auto svg = [](const std::string& par, const std::string& body) {
        return "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 10 10'" +
               (par.empty() ? std::string() : " preserveAspectRatio='" + par + "'") + ">" + body + "</svg>";
    };
    const std::string red = "<rect width='10' height='10' fill='#ff0000'/>";
    auto isRed = [](const Image& img, int x, int y) { return img.colorAt(x, y) == Color(255, 0, 0); };
    auto empty = [](const Image& img, int x, int y) { return img.pixel(x, y) == 0; };
    // 20 x 100: as written (xMidYMid meet) the 20 x 20 square in the middle; none: all of it
    Image meet = renderSvg(svg("", red), 20, 100);
    CHECK(empty(meet, 10, 10) && isRed(meet, 10, 40) && isRed(meet, 10, 59) && empty(meet, 10, 60) && empty(meet, 10, 90));
    CHECK(meet.pixel(10, 50) == renderSvg(svg("xMidYMid meet", red), 20, 100).pixel(10, 50));
    Image none = renderSvg(svg("none", red), 20, 100);
    bool all = true;
    for (int y = 0; y < 100; y++)
        for (int x = 0; x < 20; x++) all = all && isRed(none, x, y);
    CHECK(all);
    CHECK(isRed(renderSvg(svg(" defer none ", red), 20, 100), 0, 99));
    Image top = renderSvg(svg("xMinYMin meet", red), 20, 100), bottom = renderSvg(svg("xMaxYMax", red), 20, 100);
    CHECK(isRed(top, 10, 0) && isRed(top, 10, 19) && empty(top, 10, 20));
    CHECK(empty(bottom, 10, 79) && isRed(bottom, 10, 80) && isRed(bottom, 10, 99));
    // slice: covers it all, cut; left half red, right half blue (scaled 10 times: 2 view units across, centred)
    const std::string halves = "<rect width='5' height='10' fill='#ff0000'/><rect x='5' width='5' height='10' fill='#0000ff'/>";
    Image slice = renderSvg(svg("xMidYMid slice", halves), 20, 100);
    CHECK(isRed(slice, 5, 0) && isRed(slice, 9, 99) && slice.colorAt(10, 50) == Color(0, 0, 255) &&
          slice.colorAt(19, 0) == Color(0, 0, 255));
    Image sliceLeft = renderSvg(svg("xMinYMid slice", halves), 20, 100);
    CHECK(isRed(sliceLeft, 19, 50));
    Image sliceRight = renderSvg(svg("xMaxYMax slice", halves), 20, 100);
    CHECK(sliceRight.colorAt(0, 50) == Color(0, 0, 255));
    // wider than tall: none stretches across
    Image wide = renderSvg(svg("none", halves), 100, 10);
    CHECK(isRed(wide, 49, 5) && wide.colorAt(50, 5) == Color(0, 0, 255) && isRed(wide, 0, 0));
    // a nested <svg>: its viewBox onto its width x height
    Image nested = renderSvg(svg("", "<svg width='10' height='5' viewBox='0 0 1 1' preserveAspectRatio='none'>"
                                     "<rect width='1' height='1' fill='#00ff00'/></svg>"), 10, 10);
    CHECK(nested.colorAt(0, 0) == Color(0, 255, 0) && nested.colorAt(9, 4) == Color(0, 255, 0) && nested.pixel(5, 5) == 0);
    Image nestedMeet = renderSvg(svg("", "<svg width='10' height='5' viewBox='0 0 1 1'>"
                                         "<rect width='1' height='1' fill='#00ff00'/></svg>"), 10, 10);
    CHECK(nestedMeet.pixel(0, 2) == 0 && nestedMeet.colorAt(5, 2) == Color(0, 255, 0) && nestedMeet.pixel(9, 2) == 0);
}

// linearGradient / radialGradient fills and strokes: stops, units, gradientTransform, spreadMethod, href, fallbacks
static void testSvgGradients() {
    auto doc = [](const std::string& viewBox, const std::string& body) {
        return "<svg xmlns='http://www.w3.org/2000/svg' xmlns:xlink='http://www.w3.org/1999/xlink' viewBox='" + viewBox +
               "'>" + body + "</svg>";
    };
    auto near = [](Color c, int r, int g, int b, int tol) {
        return std::abs(c.r - r) <= tol && std::abs(c.g - g) <= tol && std::abs(c.b - b) <= tol && c.a == 255;
    };
    const std::string bw = "<linearGradient id='g'><stop offset='0' stop-color='#000'/><stop offset='1' stop-color='#fff'/></linearGradient>";
    // objectBoundingBox (the default): black at the left of the box to white at its right, each row the same
    {
        Image img = renderSvg(doc("0 0 100 10", "<defs>" + bw + "</defs><rect width='100' height='10' fill='url(#g)'/>"), 100, 10);
        bool rising = true, rows = true;
        for (int x = 1; x < 100; x++) rising = rising && img.colorAt(x, 5).r >= img.colorAt(x - 1, 5).r;
        for (int y = 0; y < 10; y++)
            for (int x = 0; x < 100; x++) rows = rows && img.pixel(x, y) == img.pixel(x, 0);
        CHECK(rising && rows);
        CHECK(near(img.colorAt(0, 5), 1, 1, 1, 2) && near(img.colorAt(99, 5), 254, 254, 254, 2) &&
              near(img.colorAt(50, 5), 129, 129, 129, 2));
        // the box of the shape, not of the drawing: a rect on the right half
        Image half = renderSvg(doc("0 0 100 10", bw + "<rect x='50' width='50' height='10' fill='url(#g)'/>"), 100, 10);
        CHECK(half.pixel(25, 5) == 0 && near(half.colorAt(50, 5), 3, 3, 3, 2) && near(half.colorAt(75, 5), 130, 130, 130, 3) &&
              near(half.colorAt(99, 5), 252, 252, 252, 2));
        // % positions, a gradient that ends half way (pad: white after it)
        Image pct = renderSvg(doc("0 0 100 10", "<linearGradient id='p' x1='0%' x2='50%'><stop offset='0' stop-color='#000'/>"
                                               "<stop offset='100%' stop-color='#fff'/></linearGradient>"
                                               "<rect width='100' height='10' fill='url(#p)'/>"), 100, 10);
        CHECK(near(pct.colorAt(25, 5), 130, 130, 130, 3) && pct.colorAt(50, 5) == Color(255, 255, 255) &&
              pct.colorAt(99, 5) == Color(255, 255, 255));
    }
    // spreadMethod: x2 at the middle; at x 60 (t 1.21) repeat starts again (dark), reflect comes back (light)
    {
        auto spread = [&](const char* method) {
            return renderSvg(doc("0 0 100 10", std::string("<linearGradient id='s' x2='0.5' spreadMethod='") + method +
                                                   "'><stop offset='0' stop-color='#000'/><stop offset='1' stop-color='#fff'/>"
                                                   "</linearGradient><rect width='100' height='10' fill='url(#s)'/>"),
                             100, 10);
        };
        Image pad = spread("pad"), repeat = spread("repeat"), reflect = spread("reflect");
        CHECK(pad.colorAt(60, 5) == Color(255, 255, 255));
        CHECK(near(repeat.colorAt(60, 5), 54, 54, 54, 3) && near(reflect.colorAt(60, 5), 201, 201, 201, 3));
        CHECK(near(repeat.colorAt(25, 5), 130, 130, 130, 3) && near(reflect.colorAt(25, 5), 130, 130, 130, 3));
    }
    // radial (the default circle: centre .5 .5, radius .5): red in the middle, blue at the edge and beyond (pad)
    {
        Image img = renderSvg(doc("0 0 100 100", "<radialGradient id='r'><stop offset='0' stop-color='#f00'/>"
                                                 "<stop offset='1' stop-color='#00f'/></radialGradient>"
                                                 "<rect width='100' height='100' fill='url(#r)'/>"), 100, 100);
        CHECK(near(img.colorAt(50, 50), 252, 0, 3, 3) && img.colorAt(0, 0) == Color(0, 0, 255) &&
              near(img.colorAt(75, 50), 125, 0, 130, 4) && near(img.colorAt(50, 24), 125, 0, 130, 4));
        CHECK(img.pixel(30, 70) == img.pixel(70, 30) && img.pixel(30, 30) == img.pixel(69, 69));  // (round)
        // an ellipse in a wide box (objectBoundingBox): t 0.5 at a quarter of the width and of the height
        Image wide = renderSvg(doc("0 0 200 100", "<radialGradient id='r'><stop offset='0' stop-color='#f00'/>"
                                                  "<stop offset='1' stop-color='#00f'/></radialGradient>"
                                                  "<rect width='200' height='100' fill='url(#r)'/>"), 200, 100);
        CHECK(near(wide.colorAt(150, 50), 125, 0, 130, 4) && near(wide.colorAt(100, 24), 125, 0, 130, 4));
        // a focal point at the left: red there, the colours closer together on its left than on its right
        Image focal = renderSvg(doc("0 0 100 100", "<radialGradient id='f' fx='0.2'><stop offset='0' stop-color='#f00'/>"
                                                   "<stop offset='1' stop-color='#00f'/></radialGradient>"
                                                   "<rect width='100' height='100' fill='url(#f)'/>"), 100, 100);
        CHECK(near(focal.colorAt(20, 50), 250, 0, 5, 6) && focal.colorAt(10, 50).b > focal.colorAt(30, 50).b);
        CHECK(near(focal.colorAt(60, 50), 128, 0, 127, 6));  // (half way from the focus to the edge at the right)
        // r = 0: the last stop's colour
        Image zero = renderSvg(doc("0 0 10 10", "<radialGradient id='z' r='0'><stop offset='0' stop-color='#f00'/>"
                                                "<stop offset='1' stop-color='#0f0'/></radialGradient>"
                                                "<rect width='10' height='10' fill='url(#z)'/>"), 10, 10);
        CHECK(zero.colorAt(5, 5) == Color(0, 255, 0));
    }
    // userSpaceOnUse + gradientTransform: a horizontal line turned 90° (top to bottom across the drawing); the
    // shape's box doesn't matter
    {
        const std::string u = "<linearGradient id='u' gradientUnits='userSpaceOnUse' x1='0' y1='0' x2='10' y2='0' "
                               "gradientTransform='rotate(90)'><stop offset='0' stop-color='#000'/>"
                               "<stop offset='1' stop-color='#fff'/></linearGradient>";
        Image img = renderSvg(doc("0 0 10 10", u + "<rect width='10' height='10' fill='url(#u)'/>"), 100, 100);
        CHECK(img.pixel(10, 3) == img.pixel(90, 3) && near(img.colorAt(50, 0), 1, 1, 1, 2) &&
              near(img.colorAt(50, 99), 254, 254, 254, 2) && near(img.colorAt(50, 50), 129, 129, 129, 2));
        Image part = renderSvg(doc("0 0 10 10", u + "<rect y='5' width='10' height='5' fill='url(#u)'/>"), 100, 100);
        CHECK(part.pixel(50, 40) == 0 && part.pixel(50, 50) == img.pixel(50, 50) && part.pixel(50, 99) == img.pixel(50, 99));
        // % of the viewport, and a transform on the shape (the gradient goes with it)
        Image pc = renderSvg(doc("0 0 10 10", "<linearGradient id='q' gradientUnits='userSpaceOnUse' x2='100%'>"
                                              "<stop offset='0' stop-color='#000'/><stop offset='1' stop-color='#fff'/>"
                                              "</linearGradient><g transform='scale(0.5)'><rect width='20' height='20' "
                                              "fill='url(#q)'/></g>"), 100, 100);
        CHECK(near(pc.colorAt(0, 50), 1, 1, 1, 2) && near(pc.colorAt(25, 50), 128, 128, 128, 3) &&
              pc.colorAt(60, 50) == Color(255, 255, 255));
    }
    // href / xlink:href: the stops of one gradient, the direction of another; attributes of the nearest
    {
        const std::string base = "<linearGradient id='base' x2='1'><stop offset='0' stop-color='red'/>"
                                 "<stop offset='1' stop-color='blue'/></linearGradient>";
        for (const char* attr : {"href", "xlink:href"}) {
            Image img = renderSvg(doc("0 0 10 10", base + "<linearGradient id='v' " + attr +
                                                       "='#base' x2='0' y2='1'/><rect width='10' height='10' fill='url(#v)'/>"),
                                  100, 100);
            CHECK(near(img.colorAt(50, 0), 254, 0, 1, 2) && near(img.colorAt(50, 99), 1, 0, 254, 2) &&
                  img.pixel(0, 50) == img.pixel(99, 50));
        }
        // a radial gradient taking a linear one's stops (not its x1… attributes)
        Image rad = renderSvg(doc("0 0 10 10", base + "<radialGradient id='rv' href='#base'/>"
                                                    "<rect width='10' height='10' fill='url(#rv)'/>"), 100, 100);
        CHECK(near(rad.colorAt(50, 50), 252, 0, 3, 3) && rad.colorAt(0, 0) == Color(0, 0, 255));
        // a loop of hrefs: no stops, nothing drawn (and no hang)
        Image loop = renderSvg(doc("0 0 10 10", "<linearGradient id='a' href='#b'/><linearGradient id='b' href='#a'/>"
                                                "<rect width='10' height='10' fill='url(#a)'/>"), 10, 10);
        CHECK(loop.pixel(5, 5) == 0);
    }
    // stops: style="stop-color…; stop-opacity…", out of order offsets (moved up to the one before: a hard stop)
    {
        Image img = renderSvg(doc("0 0 100 10", "<linearGradient id='o'><stop offset='0' style='stop-color: #ff0000; stop-opacity: 0.5'/>"
                                                "<stop offset='0.5' stop-color='#ff0000' stop-opacity='50%'/>"
                                                "<stop offset='0.2' stop-color='#0000ff'/></linearGradient>"
                                                "<rect width='100' height='10' fill='url(#o)'/>"), 100, 10);
        CHECK(img.pixel(10, 5) == 0x80800000u && img.colorAt(75, 5) == Color(0, 0, 255) && img.pixel(40, 5) == 0x80800000u);
        // one stop: its colour; fill-opacity and opacity on top
        Image one = renderSvg(doc("0 0 10 10", "<linearGradient id='one'><stop stop-color='#00ff00'/></linearGradient>"
                                               "<rect width='10' height='10' fill='url(#one)' fill-opacity='0.5'/>"), 10, 10);
        CHECK(one.pixel(5, 5) == 0x80008000u);
        Image faded = renderSvg(doc("0 0 100 10", bw + "<rect width='100' height='10' fill='url(#g)' opacity='0.5'/>"), 100, 10);
        CHECK((faded.pixel(99, 5) >> 24) == 128 && (faded.pixel(0, 5) >> 24) == 128);
    }
    // fallbacks: a missing reference takes the colour after it, else draws nothing; a gradient wins over it
    {
        auto fill = [&](const std::string& paint) {
            return renderSvg(doc("0 0 10 10", bw + "<pattern id='pat'/><rect width='10' height='10' fill=\"" + paint + "\"/>"), 10, 10);
        };
        CHECK(fill("url(#missing) #00ff00").colorAt(5, 5) == Color(0, 255, 0));
        CHECK(fill("url(#missing)").pixel(5, 5) == 0);
        CHECK(fill("url(#pat) lime").colorAt(5, 5) == Color(0, 255, 0));  // (patterns aren't drawn)
        CHECK(fill("url('#g') lime").colorAt(9, 5).r > 200 && fill("url(#g) lime").colorAt(0, 5).r < 30);
        CHECK(fill("url(other.svg#g) #0000ff").colorAt(5, 5) == Color(0, 0, 255));
        // a box with no height (a horizontal line) and objectBoundingBox: not drawn
        Image line = renderSvg(doc("0 0 10 10", bw + "<line x1='0' y1='5' x2='10' y2='5' stroke='url(#g)' stroke-width='2'/>"), 10, 10);
        CHECK(line.pixel(5, 5) == 0);
    }
    // strokes: the gradient across the shape's box (its geometry, not its stroke), only where the stroke is
    {
        Image img = renderSvg(doc("0 0 100 10", bw + "<rect x='10' y='2' width='80' height='6' fill='none' stroke='url(#g)' stroke-width='2'/>"), 100, 10);
        CHECK(img.pixel(50, 5) == 0 && img.pixel(50, 0) == 0);
        CHECK(near(img.colorAt(12, 2), 8, 8, 8, 4) && near(img.colorAt(87, 2), 247, 247, 247, 4) &&
              near(img.colorAt(50, 7), 128, 128, 128, 4));
        const std::string u = "<linearGradient id='u' gradientUnits='userSpaceOnUse' x1='0' x2='100'><stop offset='0' stop-color='#f00'/>"
                              "<stop offset='1' stop-color='#00f'/></linearGradient>";
        Image line = renderSvg(doc("0 0 100 10", u + "<line x1='0' y1='5' x2='100' y2='5' stroke='url(#u)' stroke-width='2' "
                                                     "stroke-opacity='0.5'/>"), 100, 10);
        CHECK(line.pixel(50, 2) == 0 && (line.pixel(5, 5) >> 24) == 128 && line.colorAt(5, 5).r > 230 &&
              line.colorAt(95, 5).b > 230);
    }
}

// nearest-neighbour enlargement, to look at single pixels
static Image zoom(const Image& src, const Rect& r, int k) {
    Image out(r.w * k, r.h * k);
    for (int y = 0; y < out.height(); y++)
        for (int x = 0; x < out.width(); x++) out.setPixel(x, y, src.pixel(r.x + x / k, r.y + y / k));
    return out;
}

// sample renderings to look at
static void samples() {
    std::string dir = sampleDir();
    const Color bg(0x2b, 0x2d, 0x31), fg(0xe6, 0xe6, 0xe6), accent(0x3d, 0x8e, 0xf0);
    {
        Image img(560, 300, bg);
        Painter p(img);
        float y = 20;
        for (float pt : {9.f, 12.f}) {
            for (bool bold : {false, true}) {
                Font f = Font::fromPoints("DejaVu Sans", pt, bold);
                p.drawText(PointF(8, y), "Tiny DAW: Track 1 - Audio, Mixer, 120.00 BPM 4/4 (Qq gjpy)", f, fg);
                y += f.lineSpacing() + 2;
            }
        }
        Font m = Font::mono();
        p.drawText(PointF(8, y), "0001.01.000  00:00:12.345  -14.2 LUFS", m, fg);
        y += m.lineSpacing() + 2;
        Font m12 = Font::fromPoints("Consolas", 12);
        p.drawText(PointF(8, y), "0001.01.000  00:00:12.345", m12, fg);
        y += m12.lineSpacing() + 4;
        Font ui = Font::ui();
        p.drawText(PointF(8, y), "Symbols: \xE2\x9C\x95 \xE2\x96\xBE \xE2\x96\xB8 \xE2\x99\xA9 \xE2\x85\x93 \xE2\x96\xA0 \xE2\x9F\xB2 \xE2\x9A\xA0 \xE2\x9D\x84 \xE2\x8F\xAE \xE2\xA4\xA2 \xE2\x97\x8F \xF0\x9F\x8E\xB9 \xF0\x9F\x94\x8A \xE2\x86\x94 \xE2\x80\xA6 \xE2\x86\x92", ui, fg);
        y += ui.lineSpacing() + 4;
        // buttons with centred text
        const char* labels[] = {"Play", "Stop", "Record", "Loop", "Metronome"};
        float x = 8;
        for (const char* l : labels) {
            float w = ui.width(l) + 20;
            p.fillRoundedRect(RectF(x, y, w, 24), 3, l[0] == 'R' ? accent : Color(0x3c, 0x3f, 0x44));
            p.drawText(RectF(x, y, w, 24), AlignCenter, l, ui, fg);
            x += w + 6;
        }
        y += 32;
        Image light(560, 60, Color(0xf0, 0xf0, 0xf0));
        p.drawImage(Point(0, (int)y), light);
        p.drawText(PointF(8, y + 18), "Light background: The quick brown fox jumps over the lazy dog. 0123456789", ui, Color(20, 20, 20));
        p.drawText(PointF(8, y + 40), "Elided: ", ui, Color(20, 20, 20));
        p.drawText(RectF(60, y + 28, 150, 16), AlignLeft | AlignVCenter, "A very long clip name that is elided at the end", ui, Color(20, 20, 20), true);
        p.drawTextBlock(RectF(330, y + 24, 220, 36), AlignLeft | AlignTop, "Wrapped text block: several words that wrap at spaces.\nAnd a new line.", ui, Color(20, 20, 20));
        savePng(dir + "text.png", img);
        // zoomed 4x for close inspection
        savePng(dir + "text-zoom.png", zoom(img, Rect(4, 8, 100, 30), 8));
        savePng(dir + "text-zoom-symbols.png", zoom(img, Rect(0, 120, 300, 26), 4));
    }
    {
        Image img(400, 260, bg);
        Painter p(img);
        // rounded rects, small and large
        for (int i = 0; i < 6; i++) p.fillRoundedRect(RectF(10 + i * 40, 10, 32, 22), (float)i, accent);
        for (int i = 0; i < 6; i++) p.strokeRoundedRect(RectF(10.5f + i * 40, 40.5f, 31, 21), (float)i + 0.5f, fg);
        // lines at angles
        for (int i = 0; i < 12; i++) {
            float a = i * 3.14159265f / 12;
            p.drawLine(PointF(80, 150), PointF(80 + 60 * std::cos(a), 150 - 60 * std::sin(a)), fg, 1);
        }
        for (int i = 0; i < 6; i++) p.drawLine(PointF(160, 90.5f + i * 6), PointF(240, 95.5f + i * 6 + i * 3), fg, 1 + i * 0.5f);
        // circles of various sizes
        for (int i = 0; i < 6; i++) {
            float r = 2.f + i * 2;
            p.fillEllipse(RectF(260 + i * 22 - r / 2, 100 - r / 2, r, r), fg);
            p.strokeEllipse(RectF(260 + i * 22 - r / 2, 130 - r / 2, r, r), accent, 1);
        }
        // a polyline with joins and caps
        std::vector<PointF> zig;
        for (int i = 0; i < 8; i++) zig.push_back(PointF(170 + i * 25, i % 2 ? 230 : 180));
        p.drawPolyline(zig, Color(240, 120, 60), 6, LineCap::Round, LineJoin::Round);
        std::vector<PointF> zig2;
        for (int i = 0; i < 5; i++) zig2.push_back(PointF(20 + i * 25, i % 2 ? 250 : 225));
        p.drawPolyline(zig2, Color(120, 200, 90), 5, LineCap::Square, LineJoin::Miter);
        p.drawDashedLine(PointF(160, 160.5f), PointF(390, 160.5f), fg, 1, 4, 3);
        // icons tinted, at 14 and 20 px
        const char* names[] = {"play", "pause", "record", "return_to_start", "follow"};
        for (int i = 0; i < 5; i++) {
            FILE* fh = fopen((std::string("data/icons/") + names[i] + ".svg").c_str(), "rb");
            std::string text;
            if (fh) {
                char buf[4096];
                size_t n;
                while ((n = fread(buf, 1, sizeof buf, fh)) > 0) text.append(buf, n);
                fclose(fh);
            }
            p.drawImage(Point(260 + i * 24, 160 - 140), renderSvgTinted(text, 14, 14, fg));
            p.drawImage(Point(260 + i * 24, 180 - 140), renderSvgTinted(text, 20, 20, accent));
        }
        savePng(dir + "shapes.png", img);
        savePng(dir + "shapes-zoom.png", zoom(img, Rect(0, 0, 200, 70), 4));
        savePng(dir + "shapes-zoom2.png", zoom(img, Rect(250, 88, 140, 52), 6));
        savePng(dir + "shapes-zoom3.png", zoom(img, Rect(150, 170, 120, 70), 5));
    }
}

static void bench() {
    Image img(2560, 1440);
    Painter p(img);
    auto t = std::chrono::steady_clock::now();
    for (int i = 0; i < 20; i++) p.fillRect(Rect(0, 0, 2560, 1440), Color(40, 40, 40));
    printf("bench: fillRect full window %.2f ms\n", ms(t) / 20);
    t = std::chrono::steady_clock::now();
    for (int i = 0; i < 2000; i++) p.fillRoundedRect(RectF((float)(i % 50) * 50, (float)(i / 50) * 30, 46, 26), 3, Color(60, 90, 200));
    printf("bench: 2000 rounded rects 46x26 %.2f ms\n", ms(t));
    t = std::chrono::steady_clock::now();
    Font f = Font::ui();
    for (int i = 0; i < 2000; i++) p.drawText(PointF((float)(i % 20) * 120, 12.f + (i / 20) * 14), "Track 12 Audio", f, Color(230, 230, 230));
    printf("bench: 2000 text runs %.2f ms\n", ms(t));
    t = std::chrono::steady_clock::now();
    for (int i = 0; i < 2000; i++) p.drawLine(PointF(0, (float)i * 0.7f), PointF(2560, (float)i * 0.7f + 40), Color(200, 200, 200), 1);
    printf("bench: 2000 long diagonal lines %.2f ms\n", ms(t));
    t = std::chrono::steady_clock::now();
    for (int i = 0; i < 2000; i++) p.drawVLine(i, 0, 1440, Color(200, 200, 200, 100));
    printf("bench: 2000 translucent vlines %.2f ms\n", ms(t));
}

int main() {
    auto t = std::chrono::steady_clock::now();
    Font::families();
    printf("font scan: %.1f ms, %zu families\n", ms(t), Font::families().size());
    testColors();
    testZlib();
    testPng();
    testPainter();
    testFonts();
    testSvg();
    testSvgAspect();
    testSvgGradients();
    samples();
    bench();
    TEST_MAIN_END();
}
