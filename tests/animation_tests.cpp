#include <atomic>
#include <cstring>

#include "config.h"
#include "imaging/animation.h"
#include "imaging/decoder.h"
#include "test.h"

// Each frame is drawn over the last, after undoing it as its disposal asks: 2 clears to the background, 3
// restores what was there, and transparent pixels leave the canvas as it is.
void TestAnimationDisposal() {
    constexpr UINT kW = 4, kH = 2, kStride = 12;
    constexpr UINT32 kRed = 0xFF0000, kGreen = 0x00FF00, kBlue = 0x0000FF;
    BYTE canvas[kStride * kH];
    Animation a;
    a.width = kW;
    a.height = kH;
    a.stride = kStride;
    a.bits = canvas;
    const auto frame = [](RECT rect, BYTE disposal, int transparent, size_t offset) {
        return Frame{rect, 100, disposal, transparent, offset, 0};
    };
    a.palettes.resize(1);
    a.palettes[0][1] = kRed;
    a.palettes[0][2] = kGreen;
    a.palettes[0][3] = kBlue;
    a.frames = {frame({0, 0, 4, 2}, 0, -1, 0), frame({1, 0, 3, 1}, 3, -1, 8), frame({0, 1, 1, 2}, 2, -1, 10),
                frame({3, 1, 4, 2}, 0, 0, 11)};
    a.pixels = AllocPages<BYTE>(12);
    const BYTE indices[12] = {1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 3, 0};
    memcpy(a.pixels.get(), indices, sizeof(indices));
    a.saved.resize(2 * 3);

    const auto at = [&](UINT x, UINT y) {
        const BYTE* p = canvas + y * kStride + x * 3;
        return UINT32(p[0] | p[1] << 8 | p[2] << 16);
    };
    const auto expect = [&](const UINT32 (&want)[kH][kW], int step) {
        for (UINT y = 0; y < kH; ++y) {
            for (UINT x = 0; x < kW; ++x) {
                if (at(x, y) != want[y][x]) return Fail("animation frame drawn wrong", step);
            }
        }
    };
    memset(canvas, 0x55, sizeof(canvas));
    ShowFrame(a, 0);
    expect({{kRed, kRed, kRed, kRed}, {kRed, kRed, kRed, kRed}}, 0);
    ShowFrame(a, 1);
    expect({{kRed, kGreen, kGreen, kRed}, {kRed, kRed, kRed, kRed}}, 1);
    ShowFrame(a, 2);  // restores under frame 1
    expect({{kRed, kRed, kRed, kRed}, {kBlue, kRed, kRed, kRed}}, 2);
    ShowFrame(a, 3);  // clears under frame 2, and draws nothing itself
    expect({{kRed, kRed, kRed, kRed}, {0, kRed, kRed, kRed}}, 3);
    ShowFrame(a, 0);  // a new loop starts over
    expect({{kRed, kRed, kRed, kRed}, {kRed, kRed, kRed, kRed}}, 4);
}

namespace {

std::vector<UINT32> RandomColors(std::mt19937& rng, UINT n) {
    std::vector<UINT32> colors(n);
    for (UINT32& c : colors) c = rng() & 0xFFFFFF;
    return colors;
}

// Plays every frame of a decoded animation onto a canvas of its own, as the viewer does, for two loops, and checks
// each against the spec.
void CheckPlayback(Animation& a, const Gif& gif, int id) {
    const std::vector<Pixels> want = ComposeGif(gif);
    const UINT stride = (a.width * 3 + 3) & ~3u;
    std::vector<BYTE> canvas(size_t(stride) * a.height, 0x55);
    a.bits = canvas.data();
    a.stride = stride;
    for (int loop = 0; loop < 2; ++loop) {
        for (size_t i = 0; i < a.frames.size(); ++i) {
            ShowFrame(a, i);
            const Pixels& w = want[i];
            for (UINT y = 0; y < a.height; ++y) {
                for (UINT x = 0; x < a.width; ++x) {
                    const BYTE* p = canvas.data() + size_t(y) * stride + x * 3;
                    if (UINT32(p[0] | p[1] << 8 | p[2] << 16) != (w.at(x, y) & 0xFFFFFF)) {
                        Fail("played frame differs from the GIF spec", id * 1000 + int(i));
                        return;
                    }
                }
            }
        }
    }
}

HRESULT ReadGif(const std::vector<BYTE>& data, Animation& a, bool stopped = false) {
    const std::atomic<bool> stop{stopped};
    return DecodeAnimation(Source{.stream = MemoryStream(data)}, a, stop);
}

}  // namespace

// Real GIF files, written byte by byte, read and played as the viewer does: frame placement and clipping, every
// disposal, transparency, local palettes, timing and loop counts like browsers, and the limits.
void TestGifFiles() {
    std::mt19937 rng(59);
    const std::vector<UINT32> global = RandomColors(rng, 256);

    // Frames placed anywhere, including past the canvas edges, over every disposal, with and without transparency
    // and with palettes of their own.
    std::vector<Gif> gifs;
    {
        Gif gif{60, 40, global, 0};
        gif.frames.push_back(RandomGifFrame(rng, {0, 0, 60, 40}, 256));
        gif.frames.push_back(RandomGifFrame(rng, {10, 5, 30, 25}, 256));
        gif.frames.back().disposal = 3;
        gif.frames.push_back(RandomGifFrame(rng, {20, 10, 50, 35}, 256));
        gif.frames.back().disposal = 2;
        gif.frames.back().transparent = 7;
        gif.frames.push_back(RandomGifFrame(rng, {45, 30, 75, 55}, 16));  // past the right and bottom
        gif.frames.back().palette = RandomColors(rng, 16);
        gif.frames.back().disposal = 1;
        gif.frames.push_back(RandomGifFrame(rng, {0, 0, 60, 40}, 4));
        gif.frames.back().transparent = 0;
        gif.frames.back().palette = RandomColors(rng, 4);
        gif.frames.back().disposal = 3;
        gif.frames.push_back(RandomGifFrame(rng, {59, 39, 60, 40}, 256));  // one pixel in the corner
        gif.frames.back().disposal = 2;
        gif.frames.push_back(RandomGifFrame(rng, {70, 50, 80, 60}, 256));  // entirely off the canvas
        gifs.push_back(gif);
    }
    {
        // Mostly transparent frames, each building on the last.
        Gif gif{33, 17, global, 3};
        for (int i = 0; i < 6; ++i) {
            GifFrame f = RandomGifFrame(rng, {LONG(i * 3), LONG(i), LONG(i * 3 + 15), LONG(i + 12)}, 8);
            f.transparent = int(i % 8);
            f.disposal = BYTE(i % 4);
            gif.frames.push_back(f);
        }
        gifs.push_back(gif);
    }
    {
        // No global table: every frame brings its own.
        Gif gif{8, 8, {}, -1};
        for (int i = 0; i < 3; ++i) {
            GifFrame f = RandomGifFrame(rng, {0, 0, 8, 8}, 2);
            f.palette = RandomColors(rng, 2);
            gif.frames.push_back(f);
        }
        gifs.push_back(gif);
    }
    for (int id = 0; id < int(gifs.size()); ++id) {
        const Gif& gif = gifs[id];
        const std::vector<BYTE> data = WriteGif(gif);
        Animation a;
        if (FAILED(ReadGif(data, a))) {
            Fail("animated GIF didn't read", id);
            continue;
        }
        if (a.width != gif.width || a.height != gif.height || a.frames.size() != gif.frames.size()) {
            Fail("animated GIF read with the wrong size or frame count", id);
            continue;
        }
        for (size_t i = 0; i < a.frames.size(); ++i) {
            const RECT& r = gif.frames[i].rect;
            const RECT clipped{std::min(r.left, LONG(gif.width)), std::min(r.top, LONG(gif.height)),
                               std::min(r.right, LONG(gif.width)), std::min(r.bottom, LONG(gif.height))};
            if (!EqualRect(&a.frames[i].rect, &clipped)) Fail("frame not placed and clipped to the canvas", int(i));
            if (a.frames[i].disposal != gif.frames[i].disposal) Fail("frame disposal read wrong", int(i));
            if (a.frames[i].transparent != gif.frames[i].transparent) Fail("frame transparency read wrong", int(i));
        }
        CheckPlayback(a, gif, id);

        // The viewer first shows frame 0 on the canvas, then plays the rest.
        Image image;
        bool animated = false;
        if (FAILED(DecodeImage(Source{.stream = MemoryStream(data)}, image, &animated)) || !animated) {
            Fail("animated GIF not decoded as animated", id);
        } else {
            int bpp = 0;
            if (DibPixels(image.bitmap, &bpp) != ComposeGif(gif)[0] || bpp != 24) Fail("first frame shown wrong", id);
            if (image.exact) Fail("animation canvas counts as exact", id);
            if (image.fullWidth != gif.width || image.fullHeight != gif.height) Fail("animation full size wrong", id);
        }
        if (image.bitmap) DeleteObject(image.bitmap);
    }

    // Frames that share a color table share one palette, so a long GIF doesn't keep a copy per frame.
    {
        Gif gif{4, 4, global, 0};
        const std::vector<UINT32> local = RandomColors(rng, 256);
        for (int i = 0; i < 40; ++i) {
            gif.frames.push_back(RandomGifFrame(rng, {0, 0, 4, 4}, 256));
            if (i % 4 == 1) gif.frames.back().palette = local;
        }
        Animation a;
        if (FAILED(ReadGif(WriteGif(gif), a)) || a.palettes.size() > 2) {
            Fail("frames with the same colors don't share a palette", int(a.palettes.size()));
        }
        CheckPlayback(a, gif, 100);
    }

    // Delays as browsers treat them, in ms: 0 and 1 hundredths mean 100 ms.
    {
        Gif gif{2, 2, global, 0};
        const UINT delays[] = {0, 1, 2, 5, 50, 65535};
        const UINT want[] = {100, 100, 20, 50, 500, 655350};
        for (const UINT d : delays) {
            gif.frames.push_back(RandomGifFrame(rng, {0, 0, 2, 2}, 256));
            gif.frames.back().delay = d;
        }
        Animation a;
        if (FAILED(ReadGif(WriteGif(gif), a))) Fail("GIF with delays didn't read", 0);
        for (size_t i = 0; i < a.frames.size() && i < std::size(want); ++i) {
            if (a.frames[i].delay != want[i]) Fail("frame delay wrong", int(a.frames[i].delay));
        }
        // ShowFrame returns how long the frame stays.
        std::vector<BYTE> canvas(2 * 8);
        a.bits = canvas.data();
        a.stride = 8;
        if (!a.frames.empty() && ShowFrame(a, 2) != 20) Fail("ShowFrame returned the wrong delay", 0);
    }

    // Loop counts: once without the NETSCAPE2.0 extension, otherwise once more than it repeats, 0 for ever.
    for (const auto& [loops, plays] : {std::pair{-1, 1u}, std::pair{0, 0u}, std::pair{1, 2u}, std::pair{3, 4u},
                                       std::pair{65535, 65536u}}) {
        Gif gif{2, 2, global, loops};
        gif.frames = {RandomGifFrame(rng, {0, 0, 2, 2}, 256), RandomGifFrame(rng, {0, 0, 2, 2}, 256)};
        Animation a;
        if (FAILED(ReadGif(WriteGif(gif), a)) || a.plays != plays) Fail("GIF loop count read wrong", loops);
    }

    // A single frame is a still image: no animation, decoded like any other.
    {
        Gif gif{20, 10, global, 0};
        gif.frames = {RandomGifFrame(rng, {0, 0, 20, 10}, 256)};
        gif.frames[0].transparent = 3;  // shown over the background
        const std::vector<BYTE> data = WriteGif(gif);
        Animation a;
        if (ReadGif(data, a) != WINCODEC_ERR_UNSUPPORTEDOPERATION) Fail("still GIF read as an animation", 0);
        Image image;
        bool animated = true;
        if (FAILED(DecodeImage(Source{.stream = MemoryStream(data)}, image, &animated)) || animated) {
            Fail("still GIF decoded as animated", 0);
        } else if (DibPixels(image.bitmap) != ComposeGif(gif)[0]) {
            Fail("still GIF with transparency shown wrong", 0);
        }
        if (image.bitmap) DeleteObject(image.bitmap);
    }

    // Other formats are never read as animations.
    {
        const std::vector<BYTE> png =
            EncodeBytes(GUID_ContainerFormatPng, RandomBitmap(rng, GUID_WICPixelFormat24bppBGR, 4, 4).Get());
        Animation a;
        if (ReadGif(png, a) != WINCODEC_ERR_UNSUPPORTEDOPERATION) Fail("PNG read as an animation", 0);
    }

    // Too many frames to play: the first frame still shows.
    {
        Gif gif{1, 1, {0, 0xFFFFFF}, 0};
        const GifFrame dot = RandomGifFrame(rng, {0, 0, 1, 1}, 2);
        gif.frames.assign(kMaxGifFrames + 1, dot);
        const std::vector<BYTE> data = WriteGif(gif);
        Animation a;
        if (ReadGif(data, a) != WINCODEC_ERR_IMAGESIZEOUTOFRANGE) Fail("GIF with too many frames was read", 0);
        Image image;
        bool animated = false;
        if (FAILED(DecodeImage(Source{.stream = MemoryStream(data)}, image, &animated)) || !animated) {
            Fail("first frame of a GIF with too many frames not shown", 0);
        }
        if (image.bitmap) DeleteObject(image.bitmap);
    }

    // A canvas larger than a bitmap can be isn't played.
    {
        Gif gif{kMaxBitmapSize + 1, 2, global, 0};
        gif.frames = {RandomGifFrame(rng, {0, 0, 2, 2}, 256), RandomGifFrame(rng, {0, 0, 2, 2}, 256)};
        Animation a;
        if (ReadGif(WriteGif(gif), a) != WINCODEC_ERR_IMAGESIZEOUTOFRANGE) Fail("oversized GIF canvas was played", 0);
    }

    // Stopped, reading gives up.
    {
        Gif gif{8, 8, global, 0};
        gif.frames = {RandomGifFrame(rng, {0, 0, 8, 8}, 256), RandomGifFrame(rng, {0, 0, 8, 8}, 256)};
        Animation a;
        if (ReadGif(WriteGif(gif), a, true) != E_ABORT) Fail("stopped GIF read didn't stop", 0);
    }
}
