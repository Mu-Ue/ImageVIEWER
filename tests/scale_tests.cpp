#include <cstring>

#include "test.h"
#include "ui/scale.h"

namespace {

// A top-down DIB section of random pixels: 24bpp, or palette indices with a random color table.
HBITMAP RandomDib(std::mt19937& rng, int w, int h, int bpp, RGBQUAD (&colors)[256]) {
    struct {
        BITMAPINFOHEADER header;
        RGBQUAD colors[256];
    } bmi{{sizeof(BITMAPINFOHEADER), w, -h, 1, WORD(bpp), BI_RGB}};
    const int count = bpp <= 8 ? 1 << bpp : 0;
    for (int i = 0; i < count; ++i) colors[i] = bmi.colors[i] = {BYTE(rng()), BYTE(rng()), BYTE(rng()), 0};
    bmi.header.biClrUsed = count;
    void* bits = nullptr;
    const HBITMAP dib = CreateDIBSection(nullptr, reinterpret_cast<BITMAPINFO*>(&bmi), DIB_RGB_COLORS, &bits, nullptr, 0);
    const size_t stride = (size_t(w) * bpp + 31) / 32 * 4;
    for (size_t i = 0; i < stride * h; ++i) static_cast<BYTE*>(bits)[i] = BYTE(rng());
    return dib;
}

// Source pixel (x, y) of a DIB as 0x00RRGGBB.
UINT32 Pixel(const DIBSECTION& ds, const RGBQUAD* colors, int x, int y) {
    const BYTE* row = static_cast<const BYTE*>(ds.dsBm.bmBits) + size_t(y) * ds.dsBm.bmWidthBytes;
    const int bpp = ds.dsBm.bmBitsPixel;
    if (bpp == 24) return row[x * 3] | row[x * 3 + 1] << 8 | row[x * 3 + 2] << 16;
    const int bit = x * bpp;
    const RGBQUAD& q = colors[(row[bit / 8] >> (8 - bpp - bit % 8)) & ((1 << bpp) - 1)];
    return q.rgbBlue | q.rgbGreen << 8 | q.rgbRed << 16;
}

// Output pixel (i, j) of a w x h source shown as ow x oh: the exact average of the area it covers, rounded
// half up, computed the slow way.
UINT32 Reference(const DIBSECTION& ds, const RGBQUAD* colors, int ow, int oh, int i, int j) {
    const UINT64 w = ds.dsBm.bmWidth, h = ds.dsBm.bmHeight;
    UINT64 sum[3] = {};
    // In units of 1/ow (1/oh) source pixel, output pixel i spans [i * w, (i + 1) * w).
    for (UINT64 y = j * h / oh; y < h && y * oh < (j + 1) * h; ++y) {
        const UINT64 wy = std::min((y + 1) * oh, (j + 1) * h) - std::max(y * oh, j * h);
        for (UINT64 x = i * w / ow; x < w && x * ow < (i + 1) * w; ++x) {
            const UINT64 wx = std::min((x + 1) * ow, (i + 1) * w) - std::max(x * ow, i * w);
            const UINT32 c = Pixel(ds, colors, int(x), int(y));
            for (int k = 0; k < 3; ++k) sum[k] += wx * wy * (c >> (8 * k) & 0xFF);
        }
    }
    const UINT64 area = w * h;
    return UINT32((sum[0] + area / 2) / area | (sum[1] + area / 2) / area << 8 | (sum[2] + area / 2) / area << 16);
}

// A 32bpp top-down target to draw into, filled with a marker.
struct Target {
    int w, h;
    UINT32* bits = nullptr;
    HBITMAP bitmap = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);

    Target(int width, int height) : w(width), h(height) {
        BITMAPINFO bi{{sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB}};
        bitmap = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, reinterpret_cast<void**>(&bits), nullptr, 0);
        SelectObject(dc, bitmap);
        Clear();
    }
    ~Target() {
        DeleteDC(dc);
        DeleteObject(bitmap);
    }
    void Clear() { memset(bits, 0xAB, size_t(w) * h * 4); }
    UINT32 at(int x, int y) const { return bits[size_t(y) * w + x] & 0xFFFFFF; }
};

// Pixels of the drawn area at (x, y) that differ from the reference, checking every step-th row and column.
int Wrong(const Target& t, const DIBSECTION& ds, const RGBQUAD* colors, int x, int y, int ow, int oh, int step = 1) {
    int wrong = 0;
    for (int j = std::max(0, y); j < std::min(t.h, y + oh); j += step) {
        for (int i = std::max(0, x); i < std::min(t.w, x + ow); i += step) {
            wrong += t.at(i, j) != Reference(ds, colors, ow, oh, i - x, j - y);
        }
    }
    return wrong;
}

}  // namespace

// DrawScaled against the reference for random sizes, zooms, positions, clips and bit depths.
void TestScaling() {
    std::mt19937 rng(7);
    const int outW = 640, outH = 480;
    BITMAPINFO target{{sizeof(BITMAPINFOHEADER), outW, -outH, 1, 32, BI_RGB}};
    void* out = nullptr;
    const HBITMAP screen = CreateDIBSection(nullptr, &target, DIB_RGB_COLORS, &out, nullptr, 0);
    const HDC dc = CreateCompatibleDC(nullptr), imageDC = CreateCompatibleDC(nullptr);
    SelectObject(dc, screen);
    constexpr int kDepths[] = {24, 24, 8, 4, 1};
    for (int t = 0; t < 400; ++t) {
        const int bpp = kDepths[rng() % 5];
        const int w = 1 + int(rng() % 300), h = 1 + int(rng() % 300);
        RGBQUAD colors[256];
        const HBITMAP src = RandomDib(rng, w, h, bpp, colors);
        SelectObject(imageDC, src);
        DIBSECTION ds;
        GetObjectW(src, sizeof(ds), &ds);
        // Zooms from 1/50 to 8x, sometimes exactly 100%.
        const double zoom = t % 10 == 0 ? 1.0 : std::exp(std::uniform_real_distribution<double>(-3.9, 2.1)(rng));
        const int ow = std::max(1, int(w * zoom + 0.5)), oh = std::max(1, int(h * zoom + 0.5));
        const int x = int(rng() % 200) - 100, y = int(rng() % 200) - 100;
        const int cl = int(rng() % outW), ct = int(rng() % outH);
        const RECT clip{cl, ct, cl + 1 + int(rng() % (outW - cl)), ct + 1 + int(rng() % (outH - ct))};
        memset(out, 0xAB, size_t(outW) * outH * 4);
        if (!DrawScaled(dc, imageDC, x, y, ow, oh, clip)) Fail("DrawScaled refused a DIB", bpp);
        GdiFlush();
        int wrong = 0;
        for (int j = std::max(int(clip.top), y); j < std::min(int(clip.bottom), y + oh); ++j) {
            for (int i = std::max(int(clip.left), x); i < std::min(int(clip.right), x + ow); ++i) {
                const UINT32 got = static_cast<const UINT32*>(out)[size_t(j) * outW + i] & 0xFFFFFF;
                wrong += got != Reference(ds, colors, ow, oh, i - x, j - y);
            }
        }
        if (wrong) Fail("scaled pixels differ from the exact average", wrong);
        DeleteObject(src);
    }
    DeleteDC(imageDC);
    DeleteDC(dc);
    DeleteObject(screen);
}

// What DrawScaled refuses, what it leaves alone, and the extremes: thousands of pixels averaged into one, one
// pixel shown 64 times as large, and draws large enough to be split across threads, which must come out the
// same every time.
void TestScalingEdges() {
    std::mt19937 rng(17);
    Target t(1024, 768);
    const HDC imageDC = CreateCompatibleDC(nullptr);
    const RECT all{0, 0, t.w, t.h};

    // Bitmaps it can't read are refused, so the viewer falls back to StretchBlt.
    for (const WORD bpp : {WORD(16), WORD(32)}) {
        BITMAPINFO bi{{sizeof(BITMAPINFOHEADER), 8, -8, 1, bpp, BI_RGB}};
        void* bits = nullptr;
        const HBITMAP dib = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        const HGDIOBJ old = SelectObject(imageDC, dib);
        if (DrawScaled(t.dc, imageDC, 0, 0, 16, 16, all)) Fail("DrawScaled accepted an unsupported DIB", bpp);
        SelectObject(imageDC, old);
        DeleteObject(dib);
    }
    const HDC screen = GetDC(nullptr);
    const HBITMAP ddb = CreateCompatibleBitmap(screen, 8, 8);
    ReleaseDC(nullptr, screen);
    HGDIOBJ old = SelectObject(imageDC, ddb);
    if (DrawScaled(t.dc, imageDC, 0, 0, 16, 16, all)) Fail("DrawScaled accepted a device bitmap", 0);
    SelectObject(imageDC, old);
    DeleteObject(ddb);

    RGBQUAD colors[256];
    const HBITMAP tiny = RandomDib(rng, 3, 2, 24, colors);
    old = SelectObject(imageDC, tiny);
    if (DrawScaled(t.dc, imageDC, 0, 0, 0, 10, all) || DrawScaled(t.dc, imageDC, 0, 0, 10, -1, all)) {
        Fail("DrawScaled accepted an empty size", 0);
    }
    // A clip that misses the image draws nothing, and is no failure.
    t.Clear();
    if (!DrawScaled(t.dc, imageDC, 100, 100, 30, 20, {0, 0, 50, 50}) ||
        !DrawScaled(t.dc, imageDC, 100, 100, 30, 20, {200, 200, 300, 300})) {
        Fail("DrawScaled failed for a clip outside the image", 0);
    }
    GdiFlush();
    for (int i = 0; i < t.w * t.h; ++i) {
        if (t.bits[i] != 0xABABABAB) {
            Fail("DrawScaled drew outside its clip", i);
            break;
        }
    }
    // 64x, the largest zoom, partly off the target.
    DIBSECTION ds;
    GetObjectW(tiny, sizeof(ds), &ds);
    t.Clear();
    DrawScaled(t.dc, imageDC, -10, -7, 192, 128, all);
    GdiFlush();
    if (const int wrong = Wrong(t, ds, colors, -10, -7, 192, 128)) Fail("64x enlargement differs", wrong);
    SelectObject(imageDC, old);
    DeleteObject(tiny);

    // Thousands of source pixels in each output pixel, at every depth.
    for (const int bpp : {24, 8, 4, 1}) {
        const HBITMAP big = RandomDib(rng, 4000, 3000, bpp, colors);
        old = SelectObject(imageDC, big);
        GetObjectW(big, sizeof(ds), &ds);
        t.Clear();
        DrawScaled(t.dc, imageDC, 5, 3, 7, 5, all);
        GdiFlush();
        if (const int wrong = Wrong(t, ds, colors, 5, 3, 7, 5)) Fail("extreme reduction differs", bpp);
        SelectObject(imageDC, old);
        DeleteObject(big);
    }

    // A full-window draw is split across threads; repeating it must give the same pixels, all of them exact.
    const HBITMAP photo = RandomDib(rng, 3000, 2000, 24, colors);
    old = SelectObject(imageDC, photo);
    GetObjectW(photo, sizeof(ds), &ds);
    t.Clear();
    DrawScaled(t.dc, imageDC, 0, 0, 1024, 683, all);
    GdiFlush();
    if (const int wrong = Wrong(t, ds, colors, 0, 0, 1024, 683)) Fail("large threaded reduction differs", wrong);
    const std::vector<UINT32> first(t.bits, t.bits + size_t(t.w) * t.h);
    for (int run = 0; run < 20; ++run) {
        t.Clear();
        DrawScaled(t.dc, imageDC, 0, 0, 1024, 683, all);
        GdiFlush();
        if (!std::equal(first.begin(), first.end(), t.bits)) {
            Fail("threaded draw gave different pixels when repeated", run);
            break;
        }
    }
    // Enlarged on many threads too, panned to odd offsets.
    t.Clear();
    DrawScaled(t.dc, imageDC, -1234, -567, 4500, 3000, all);
    GdiFlush();
    if (const int wrong = Wrong(t, ds, colors, -1234, -567, 4500, 3000, 3)) Fail("large threaded enlargement differs", wrong);
    SelectObject(imageDC, old);
    DeleteObject(photo);
    DeleteDC(imageDC);
}
