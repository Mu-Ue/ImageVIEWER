#include <algorithm>
#include <atomic>
#include <cstring>
#include <iterator>
#include <thread>

#include "config.h"
#include "imaging/decoder.h"
#include "test.h"

namespace {

// The formats whose every pixel survives conversion to 8-bit BGRA, so their decoded bitmap is exact.
bool Is8BitFormat(const WICPixelFormatGUID& f) {
    for (const GUID& g : {GUID_WICPixelFormat24bppBGR, GUID_WICPixelFormat32bppBGR, GUID_WICPixelFormat32bppBGRA,
                          GUID_WICPixelFormat8bppIndexed, GUID_WICPixelFormat4bppIndexed, GUID_WICPixelFormat2bppIndexed,
                          GUID_WICPixelFormat1bppIndexed, GUID_WICPixelFormat8bppGray, GUID_WICPixelFormat4bppGray,
                          GUID_WICPixelFormat2bppGray, GUID_WICPixelFormatBlackWhite}) {
        if (f == g) return true;
    }
    return false;
}

bool HasProfile(IWICBitmapFrameDecode* frame) {
    UINT count = 0;
    if (FAILED(frame->GetColorContexts(0, nullptr, &count)) || count == 0) return false;
    std::vector<ComPtr<IWICColorContext>> contexts(count);
    std::vector<IWICColorContext*> raw;
    for (auto& c : contexts) {
        Wic()->CreateColorContext(&c);
        raw.push_back(c.Get());
    }
    frame->GetColorContexts(count, raw.data(), &count);
    for (UINT i = 0; i < count; ++i) {
        WICColorContextType type;
        if (SUCCEEDED(contexts[i]->GetType(&type)) && type == WICColorContextProfile) return true;
    }
    return false;
}

// What decoding must give: the shown pixels, the depth of the smallest DIB that holds them, and whether they
// count as exact.
struct Want {
    Pixels pixels;
    int bpp = 0;
    bool exact = false;
    UINT fullWidth = 0, fullHeight = 0;
};

int DepthFor(const Pixels& shown) {
    const UINT colors = CountColors(shown, 257);
    return colors <= 2 ? 1 : colors <= 16 ? 4 : colors <= 256 ? 8 : 24;
}

// What WIC makes of frame 0 of a file shown upright: palettized while it has 256 colors or fewer, except a color
// JPEG, which is never counted, and exact when it is opaque 8-bit color without a color profile.
Want WantFor(const std::wstring& path, int orientation = 1) {
    Want want;
    const ComPtr<IWICBitmapFrameDecode> frame = WicFrame(path);
    if (!frame) return want;
    const Pixels premultiplied = ReadSource(Upright(frame.Get(), orientation).Get(), GUID_WICPixelFormat32bppPBGRA);
    want.pixels = OverBackground(premultiplied);
    const WICPixelFormatGUID native = FormatOf(frame.Get());
    GUID container{};
    ComPtr<IWICBitmapDecoder> decoder;
    if (SUCCEEDED(Wic()->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                   &decoder))) {
        decoder->GetContainerFormat(&container);
    }
    const bool colorJpeg = container == GUID_ContainerFormatJpeg && native == GUID_WICPixelFormat24bppBGR;
    want.bpp = colorJpeg ? 24 : DepthFor(want.pixels);
    const bool opaque =
        std::all_of(premultiplied.argb.begin(), premultiplied.argb.end(), [](UINT32 c) { return c >> 24 == 255; });
    want.exact = opaque && Is8BitFormat(native) && !HasProfile(frame.Get());
    want.fullWidth = want.pixels.width;
    want.fullHeight = want.pixels.height;
    return want;
}

// Decodes src as the viewer does and checks it against want. Returns the image, which the caller deletes.
Image CheckDecode(const Source& src, const Want& want, int id) {
    Image image;
    bool animated = true;
    const std::atomic<bool> stop{false};
    const HRESULT hr = DecodeImage(src, image, &animated, &stop);
    if (FAILED(hr) || !image.bitmap) {
        Fail("image didn't decode", id);
        return image;
    }
    int bpp = 0;
    const Pixels got = DibPixels(image.bitmap, &bpp);
    if (got.empty()) Fail("decoded bitmap isn't a top-down DIB section", id);
    if (image.width != want.pixels.width || image.height != want.pixels.height || got.width != image.width ||
        got.height != image.height) {
        Fail("decoded image has the wrong size", id);
    } else if (got != want.pixels) {
        Fail("decoded pixels differ from WIC's", id);
    }
    if (bpp != want.bpp) Fail("decoded bitmap isn't the smallest that holds it", id * 100 + bpp);
    if (image.exact != want.exact) Fail("decoded image wrongly counts as exact or not", id * 10 + image.exact);
    if (image.fullWidth != want.fullWidth || image.fullHeight != want.fullHeight) Fail("full size is wrong", id);
    if (animated) Fail("still image decoded as animated", id);
    return image;
}

}  // namespace

// DecodeImage applies every EXIF orientation like WIC does, on its 24bpp and palette paths, and when it switches
// from one to the other partway through.
void TestOrientation() {
    IWICImagingFactory* const wic = Wic();
    std::mt19937 rng(3);
    struct {
        const GUID& container;
        WICPixelFormatGUID format;
        const wchar_t* ext;
        UINT h, fewColorRows;
    } kinds[] = {{GUID_ContainerFormatJpeg, GUID_WICPixelFormat24bppBGR, L".jpg", 97, 0},
                 // Over a million pixels, so it is decoded in bands on several threads.
                 {GUID_ContainerFormatJpeg, GUID_WICPixelFormat24bppBGR, L".jpg", 7000, 0},
                 {GUID_ContainerFormatTiff, GUID_WICPixelFormat8bppIndexed, L".tif", 97, 0},
                 // Decoded in strips of 1747 rows, so the colors run out in the second one.
                 {GUID_ContainerFormatTiff, GUID_WICPixelFormat24bppBGR, L".tif", 4000, 2000}};
    for (int k = 0; k < int(std::size(kinds)); ++k) {
        const auto& kind = kinds[k];
        for (int orientation = 1; orientation <= 8; ++orientation) {
            const std::wstring path =
                TempPath((L"orientation" + std::to_wstring(k) + L"-" + std::to_wstring(orientation) + kind.ext).c_str());
            // Wider than the 64-pixel tiles sideways images are written in, and odd-sized.
            if (FAILED(WriteImage(wic, path, kind.container, kind.format, 150, kind.h, orientation, rng,
                                  kind.fewColorRows))) {
                Fail("can't write an orientation test image", orientation);
                continue;
            }
            Image image;
            UINT w = 0, h = 0;
            const std::vector<BYTE> want = WicUpright(wic, path, orientation, w, h);
            if (FAILED(DecodeImage(Source{path}, image)) || want.empty() || image.width != w || image.height != h) {
                Fail("decoded orientation has the wrong size", orientation);
            } else {
                const HDC dc = CreateCompatibleDC(nullptr);
                BITMAPINFO bi{{sizeof(BITMAPINFOHEADER), LONG(w), -LONG(h), 1, 24, BI_RGB}};
                std::vector<BYTE> got(want.size());
                GetDIBits(dc, image.bitmap, 0, h, got.data(), &bi, DIB_RGB_COLORS);
                DeleteDC(dc);
                if (got != want) Fail("decoded orientation differs from WIC's", orientation);
            }
            if (image.bitmap) DeleteObject(image.bitmap);
            DeleteFileW(path.c_str());
        }
    }
}

// Every container the viewer opens with a Windows encoder to make it, in each pixel format that container
// holds: the pixels shown, the depth stored and whether they are exact all follow WIC's own reading of the file,
// whether it is opened as a file, as pasted data, or (when exact) from the decoded bitmap in its place.
void TestFormats() {
    std::mt19937 rng(23);
    const struct {
        const GUID& container;
        const WICPixelFormatGUID& format;
        UINT w, h;
        UINT colors = 0;
        bool opaque = false, alpha = false;
    } kCases[] = {
        // PNG: every gray and palette depth, few and many colors, alpha, 16 bits per channel, and thin images.
        {GUID_ContainerFormatPng, GUID_WICPixelFormatBlackWhite, 97, 61},
        {GUID_ContainerFormatPng, GUID_WICPixelFormat2bppGray, 97, 61},
        {GUID_ContainerFormatPng, GUID_WICPixelFormat4bppGray, 97, 61},
        {GUID_ContainerFormatPng, GUID_WICPixelFormat8bppGray, 97, 61},
        {GUID_ContainerFormatPng, GUID_WICPixelFormat16bppGray, 97, 61},
        {GUID_ContainerFormatPng, GUID_WICPixelFormat1bppIndexed, 97, 61},
        {GUID_ContainerFormatPng, GUID_WICPixelFormat2bppIndexed, 97, 61},
        {GUID_ContainerFormatPng, GUID_WICPixelFormat4bppIndexed, 97, 61},
        {GUID_ContainerFormatPng, GUID_WICPixelFormat8bppIndexed, 97, 61, 200},
        {GUID_ContainerFormatPng, GUID_WICPixelFormat8bppIndexed, 97, 61, 256},
        {GUID_ContainerFormatPng, GUID_WICPixelFormat8bppIndexed, 97, 61, 40, false, true},  // tRNS
        {GUID_ContainerFormatPng, GUID_WICPixelFormat24bppBGR, 97, 61},
        {GUID_ContainerFormatPng, GUID_WICPixelFormat24bppBGR, 97, 61, 1},
        {GUID_ContainerFormatPng, GUID_WICPixelFormat24bppBGR, 97, 61, 2},
        {GUID_ContainerFormatPng, GUID_WICPixelFormat24bppBGR, 97, 61, 3},
        {GUID_ContainerFormatPng, GUID_WICPixelFormat24bppBGR, 97, 61, 16},
        {GUID_ContainerFormatPng, GUID_WICPixelFormat24bppBGR, 97, 61, 17},
        {GUID_ContainerFormatPng, GUID_WICPixelFormat24bppBGR, 197, 161, 256},
        {GUID_ContainerFormatPng, GUID_WICPixelFormat24bppBGR, 197, 161, 257},
        {GUID_ContainerFormatPng, GUID_WICPixelFormat32bppBGRA, 97, 61},
        {GUID_ContainerFormatPng, GUID_WICPixelFormat32bppBGRA, 97, 61, 0, true},
        {GUID_ContainerFormatPng, GUID_WICPixelFormat48bppRGB, 97, 61},
        {GUID_ContainerFormatPng, GUID_WICPixelFormat64bppRGBA, 97, 61},
        {GUID_ContainerFormatPng, GUID_WICPixelFormat24bppBGR, 1, 1},
        {GUID_ContainerFormatPng, GUID_WICPixelFormat24bppBGR, 1, 333},
        {GUID_ContainerFormatPng, GUID_WICPixelFormat24bppBGR, 333, 1},
        {GUID_ContainerFormatPng, GUID_WICPixelFormat1bppIndexed, 1, 333},
        // BMP: palettes, 16-bit, 24-bit, 32-bit with and without alpha.
        {GUID_ContainerFormatBmp, GUID_WICPixelFormat1bppIndexed, 97, 61},
        {GUID_ContainerFormatBmp, GUID_WICPixelFormat4bppIndexed, 97, 61},
        {GUID_ContainerFormatBmp, GUID_WICPixelFormat8bppIndexed, 97, 61},
        {GUID_ContainerFormatBmp, GUID_WICPixelFormat16bppBGR555, 97, 61},
        {GUID_ContainerFormatBmp, GUID_WICPixelFormat16bppBGR565, 97, 61},
        {GUID_ContainerFormatBmp, GUID_WICPixelFormat24bppBGR, 97, 61},
        {GUID_ContainerFormatBmp, GUID_WICPixelFormat32bppBGR, 97, 61},
        {GUID_ContainerFormatBmp, GUID_WICPixelFormat32bppBGRA, 97, 61},
        // TIFF, including CMYK and 16 bits per channel.
        {GUID_ContainerFormatTiff, GUID_WICPixelFormat8bppIndexed, 97, 61},
        {GUID_ContainerFormatTiff, GUID_WICPixelFormat8bppGray, 97, 61},
        {GUID_ContainerFormatTiff, GUID_WICPixelFormat24bppBGR, 97, 61},
        {GUID_ContainerFormatTiff, GUID_WICPixelFormat32bppBGRA, 97, 61},
        {GUID_ContainerFormatTiff, GUID_WICPixelFormat32bppCMYK, 97, 61},
        {GUID_ContainerFormatTiff, GUID_WICPixelFormat48bppRGB, 97, 61},
        {GUID_ContainerFormatTiff, GUID_WICPixelFormat64bppRGBA, 97, 61},
        // JPEG: a color one is never palettized, even with a single color, but a gray one is.
        {GUID_ContainerFormatJpeg, GUID_WICPixelFormat24bppBGR, 97, 61},
        {GUID_ContainerFormatJpeg, GUID_WICPixelFormat24bppBGR, 97, 61, 1},
        {GUID_ContainerFormatJpeg, GUID_WICPixelFormat8bppGray, 97, 61},
        {GUID_ContainerFormatJpeg, GUID_WICPixelFormat32bppCMYK, 97, 61},
        // A still GIF.
        {GUID_ContainerFormatGif, GUID_WICPixelFormat8bppIndexed, 97, 61, 2},
        {GUID_ContainerFormatGif, GUID_WICPixelFormat8bppIndexed, 97, 61, 200},
        // JPEG XR.
        {GUID_ContainerFormatWmp, GUID_WICPixelFormat24bppBGR, 97, 61},
        {GUID_ContainerFormatWmp, GUID_WICPixelFormat32bppBGRA, 97, 61},
        {GUID_ContainerFormatWmp, GUID_WICPixelFormat8bppGray, 97, 61},
    };
    for (int id = 0; id < int(std::size(kCases)); ++id) {
        const auto& c = kCases[id];
        // A 24bpp source with few colors is written as 24bpp; RandomBitmap only limits colors for 24bpp.
        const ComPtr<IWICBitmap> source = RandomBitmap(rng, c.format, c.w, c.h, c.colors, c.opaque, c.alpha);
        const std::wstring path = TempPath(L"format" + std::to_wstring(id) + L".img");
        if (!source || FAILED(EncodeFile(path, c.container, source.Get(), &c.format))) {
            Fail("can't write a format test image", id);
            continue;
        }
        if (FormatOf(WicFrame(path).Get()) != c.format && c.container != GUID_ContainerFormatGif) {
            Fail("format test image isn't in the format it is meant to test", id);
        }
        const Want want = WantFor(path);
        Image image = CheckDecode(Source{path}, want, id);
        DeleteObject(CheckDecode(Source{.stream = MemoryStream(ReadAll(path))}, want, id).bitmap);
        // An exact bitmap then stands in for the file, as the viewer reads it for Copy and Save As. It isn't a JPEG
        // any more, so its colors are counted.
        if (image.bitmap && image.exact) {
            Want counted = want;
            counted.bpp = DepthFor(want.pixels);
            DeleteObject(CheckDecode(Source{.bitmap = image.bitmap}, counted, id).bitmap);
        }
        if (image.bitmap) DeleteObject(image.bitmap);
        DeleteFileW(path.c_str());
    }

    // A color profile makes an image inexact: the bitmap would lose it.
    const std::wstring profiled = TempPath(L"profiled.png");
    const ComPtr<IWICBitmap> rgb = RandomBitmap(rng, GUID_WICPixelFormat24bppBGR, 40, 30);
    if (FAILED(EncodeFile(profiled, GUID_ContainerFormatPng, rgb.Get(), nullptr, 1, SrgbProfile().Get()))) {
        Fail("can't write an image with a color profile", 0);
    } else {
        DeleteObject(CheckDecode(Source{profiled}, WantFor(profiled), 1000).bitmap);
    }
}

// Where an image may come from, and which files are refused before any decoder sees them: devices, pipes,
// folders, missing files and those another program has locked, while files open for writing elsewhere and
// misnamed files still open.
void TestDecodeSources() {
    std::mt19937 rng(29);
    const ComPtr<IWICBitmap> rgb = RandomBitmap(rng, GUID_WICPixelFormat24bppBGR, 40, 30);
    const std::wstring png = TempPath(L"source.png"), misnamed = TempPath(L"really a png.jpg");
    const std::wstring jpgAsPng = TempPath(L"really a jpeg.png");
    EncodeFile(png, GUID_ContainerFormatPng, rgb.Get());
    EncodeFile(misnamed, GUID_ContainerFormatPng, rgb.Get());
    EncodeFile(jpgAsPng, GUID_ContainerFormatJpeg, rgb.Get());

    const auto decode = [](const Source& src) {
        Image image;
        bool animated = false;
        const HRESULT hr = DecodeImage(src, image, &animated);
        if (image.bitmap) DeleteObject(image.bitmap);
        return hr;
    };
    if (decode(Source{misnamed}) != S_OK || decode(Source{jpgAsPng}) != S_OK) Fail("misnamed image didn't open", 0);
    const Pixels shown = Shown(misnamed);
    Image image;
    if (FAILED(DecodeImage(Source{misnamed}, image)) || DibPixels(image.bitmap) != shown) {
        Fail("misnamed image decoded wrong", 0);
    }
    if (image.bitmap) DeleteObject(image.bitmap);

    // Unicode names, and names and paths as long as Windows allows when long paths are on, as the viewer's
    // manifest asks.
    std::vector<std::wstring> unusual = {TempPath(L"café 写真 \U0001F600.png")};
    if (LongPathsEnabled()) {
        unusual.push_back(TempPath(std::wstring(251, L'n') + L".png"));
        unusual.push_back(TempDir(std::wstring(200, L'd')) + std::wstring(200, L'f') + L".png");
    }
    for (const std::wstring& path : unusual) {
        if (!CopyFileW(png.c_str(), path.c_str(), FALSE)) Fail("can't make an image with an unusual name", int(path.size()));
        if (decode(Source{path}) != S_OK) Fail("image with an unusual name didn't open", int(path.size()));
        DeleteFileW(path.c_str());
    }

    // Another program writing it doesn't stop it opening; one holding it exclusively does.
    HANDLE other = CreateFileW(png.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, 0, nullptr);
    if (decode(Source{png}) != S_OK) Fail("image open for writing elsewhere didn't open", 0);
    CloseHandle(other);
    other = CreateFileW(png.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (decode(Source{png}) != HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION)) Fail("locked image wasn't refused", 0);
    CloseHandle(other);

    const HRESULT missing = decode(Source{TempPath(L"missing.png")});
    if (missing != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) Fail("missing file gave the wrong error", missing);
    const HRESULT noFolder = decode(Source{TempPath(L"no such folder\\a.png")});
    if (noFolder != HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)) Fail("missing folder gave the wrong error", noFolder);
    if (SUCCEEDED(decode(Source{TempDir(L"a folder.png")}))) Fail("a folder opened as an image", 0);
    if (SUCCEEDED(decode(Source{}))) Fail("an empty source decoded", 0);

    // Devices and pipes are never read, even when what they would send is an image.
    for (const wchar_t* device : {L"\\\\.\\NUL", L"NUL", L"CON", L"\\\\.\\CONIN$"}) {
        const HRESULT hr = decode(Source{device});
        if (SUCCEEDED(hr)) Fail("a device was read as an image", int(wcslen(device)));
    }
    const std::wstring pipeName = L"\\\\.\\pipe\\ImageVIEWER-tests-" + std::to_wstring(GetCurrentProcessId());
    const HANDLE pipe = CreateNamedPipeW(pipeName.c_str(), PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE,
                                         1, 1 << 16, 0, 0, nullptr);
    OVERLAPPED connect{.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    ConnectNamedPipe(pipe, &connect);  // waits for the decode to connect, without blocking
    if (decode(Source{pipeName}) != WINCODEC_ERR_UNKNOWNIMAGEFORMAT) Fail("a pipe was read as an image", 0);
    CancelIoEx(pipe, &connect);
    CloseHandle(pipe);
    CloseHandle(connect.hEvent);

    // Stopped before it starts, a decode gives up without leaving anything behind.
    const DWORD gdi = GdiObjects();
    const std::atomic<bool> stop{true};
    bool animated = false;
    image = {};
    if (DecodeImage(Source{png}, image, &animated, &stop) != E_ABORT || image.bitmap) Fail("stopped decode went on", 0);
    if (GdiObjects() != gdi) Fail("stopped decode leaked GDI objects", int(GdiObjects() - gdi));
    for (const std::wstring& path : {png, misnamed, jpgAsPng}) DeleteFileW(path.c_str());
}

// The format comes from the data's first bytes alone, and only data in a format the viewer opens is read.
void TestSniffing() {
    const auto sniff = [](const std::string& head, GUID& format) {
        std::vector<BYTE> data(head.begin(), head.end());
        data.resize(std::max<size_t>(data.size(), 64), 0);
        ComPtr<IStream> stream;
        return OpenEncoded(Source{.stream = MemoryStream(data)}, stream, format);
    };
    using namespace std::string_literals;
    const struct {
        std::string head;
        const GUID* format;
    } kCases[] = {
        {"\xFF\xD8\xFF\xE0", &GUID_ContainerFormatJpeg},
        {"\x89PNG\r\n\x1A\n", &GUID_ContainerFormatPng},
        {"GIF87a", &GUID_ContainerFormatGif},
        {"GIF89a", &GUID_ContainerFormatGif},
        {"BM", &GUID_ContainerFormatBmp},
        {"II*\0"s, &GUID_ContainerFormatTiff},
        {"MM\0*"s, &GUID_ContainerFormatTiff},
        {"II\xBC", &GUID_ContainerFormatWmp},
        {"\0\0\1\0"s, &GUID_ContainerFormatIco},
        {"RIFF\x10\0\0\0WEBPVP8 "s, &GUID_ContainerFormatWebp},
        {"\0\0\0\x18" "ftypheic"s, &GUID_ContainerFormatHeif},
        {"\0\0\0\x18" "ftypheix"s, &GUID_ContainerFormatHeif},
        {"\0\0\0\x18" "ftypmif1"s, &GUID_ContainerFormatHeif},
        {"\0\0\0\x18" "ftypavif"s, &GUID_ContainerFormatHeif},
        {"\0\0\0\x18" "ftypavis"s, &GUID_ContainerFormatHeif},
        // Near misses: a cursor, a video, a WAV, a text file, a truncated PNG signature, a JPEG without its
        // third byte, and nothing at all.
        {"\0\0\2\0"s, nullptr},
        {"\0\0\0\x18" "ftypisom"s, nullptr},
        {"\0\0\0\x18" "ftypmp42"s, nullptr},
        {"RIFF\x10\0\0\0WAVEfmt "s, nullptr},
        {"<svg xmlns=", nullptr},
        {"\x89PNG\r\n", nullptr},
        {"\xFF\xD8", nullptr},
        {"", nullptr},
    };
    for (int i = 0; i < int(std::size(kCases)); ++i) {
        GUID format = GUID_NULL;
        const HRESULT hr = sniff(kCases[i].head, format);
        if (kCases[i].format ? hr != S_OK || format != *kCases[i].format : hr != WINCODEC_ERR_UNKNOWNIMAGEFORMAT) {
            Fail("data sniffed as the wrong format", i);
        }
    }
    // Short data can't match a longer signature, even when its first bytes do.
    GUID format = GUID_NULL;
    ComPtr<IStream> stream;
    if (OpenEncoded(Source{.stream = MemoryStream({0x89, 'P', 'N', 'G'})}, stream, format) == S_OK ||
        OpenEncoded(Source{.stream = MemoryStream({})}, stream, format) == S_OK) {
        Fail("short data sniffed as an image", 0);
    }
    // A decoded bitmap alone has no encoded data.
    if (OpenEncoded(Source{}, stream, format) != E_INVALIDARG) Fail("empty source has encoded data", 0);

    // The stream is left at its start for whoever reads it next.
    stream.Reset();
    const ComPtr<IStream> pasted = MemoryStream({'B', 'M', 1, 2, 3, 4});
    pasted->Seek({.QuadPart = 3}, STREAM_SEEK_SET, nullptr);
    ULARGE_INTEGER at{};
    if (FAILED(OpenEncoded(Source{.stream = pasted}, stream, format)) ||
        FAILED(stream->Seek({}, STREAM_SEEK_CUR, &at)) || at.QuadPart != 0) {
        Fail("sniffed stream isn't rewound", int(at.QuadPart));
    }

    // Data reaches only the decoder for the format it starts with: a valid icon renamed a cursor isn't opened.
    std::mt19937 rng(31);
    const ComPtr<IWICBitmap> rgba = RandomBitmap(rng, GUID_WICPixelFormat32bppBGRA, 16, 16);
    std::vector<BYTE> png = EncodeBytes(GUID_ContainerFormatPng, rgba.Get());
    std::vector<BYTE> icon = {0, 0, 1, 0, 1, 0, 16, 16, 0, 0, 1, 0, 32, 0};
    const UINT size = UINT(png.size()), offset = 22;
    for (const UINT v : {size, offset}) icon.insert(icon.end(), {BYTE(v), BYTE(v >> 8), BYTE(v >> 16), BYTE(v >> 24)});
    icon.insert(icon.end(), png.begin(), png.end());
    Image image;
    if (FAILED(DecodeImage(Source{.stream = MemoryStream(icon)}, image))) Fail("icon didn't open", 0);
    if (image.bitmap) DeleteObject(image.bitmap);
    icon[2] = 2;  // a cursor
    image = {};
    if (DecodeImage(Source{.stream = MemoryStream(icon)}, image) != WINCODEC_ERR_UNKNOWNIMAGEFORMAT || image.bitmap) {
        Fail("cursor was opened", 0);
    }

    // Image names, by extension and case.
    for (const wchar_t* ext : kExtensions) {
        std::wstring upper = ext;
        for (wchar_t& ch : upper) ch = wchar_t(towupper(ch));
        if (!IsImageName((L"photo" + std::wstring(ext)).c_str()) || !IsImageName((L"a.b" + upper).c_str())) {
            Fail("image extension not recognized", int(wcslen(ext)));
        }
    }
    for (const wchar_t* name : {L"photo", L"photo.txt", L"photo.png.txt", L"png", L".pn", L"photo.jpgx", L""}) {
        if (IsImageName(name)) Fail("non-image name recognized as an image", int(wcslen(name)));
    }
}

namespace {

// A small JPEG whose frame header claims it is w x h.
std::vector<BYTE> ClaimedJpeg(std::mt19937& rng, UINT w, UINT h) {
    std::vector<BYTE> jpeg = EncodeBytes(GUID_ContainerFormatJpeg, RandomBitmap(rng, GUID_WICPixelFormat24bppBGR, 16, 16).Get());
    for (size_t i = 0; i + 9 < jpeg.size(); ++i) {
        if (jpeg[i] == 0xFF && jpeg[i + 1] == 0xC0) {  // baseline frame: length, precision, height, width
            jpeg[i + 5] = BYTE(h >> 8);
            jpeg[i + 6] = BYTE(h);
            jpeg[i + 7] = BYTE(w >> 8);
            jpeg[i + 8] = BYTE(w);
            return jpeg;
        }
    }
    return {};
}

}  // namespace

// Images too large to show are scaled down to the bitmap's largest size, as WIC's Fant scaler makes them, and keep
// their full size for the zoom shown. Files claiming more pixels than any image should have are refused at once.
void TestDecodeLimits() {
    std::mt19937 rng(37);
    const struct {
        const GUID& container;
        UINT w, h;
        int orientation;
    } kLarge[] = {
        {GUID_ContainerFormatPng, kMaxBitmapSize + 116, 5, 1},
        {GUID_ContainerFormatPng, 3, kMaxBitmapSize + 1, 1},
        {GUID_ContainerFormatTiff, 5, kMaxBitmapSize + 116, 6},  // scaled, then turned sideways
    };
    for (int id = 0; id < int(std::size(kLarge)); ++id) {
        const auto& c = kLarge[id];
        const ComPtr<IWICBitmap> source = RandomBitmap(rng, GUID_WICPixelFormat24bppBGR, c.w, c.h);
        const std::wstring path = TempPath(L"large" + std::to_wstring(id) + L".img");
        if (FAILED(EncodeFile(path, c.container, source.Get(), nullptr, c.orientation))) {
            Fail("can't write a large test image", id);
            continue;
        }
        // Scaled to fit at the decoder, then turned upright.
        const double factor = double(kMaxBitmapSize) / std::max(c.w, c.h);
        const UINT w = std::max(1u, UINT(c.w * factor)), h = std::max(1u, UINT(c.h * factor));
        ComPtr<IWICBitmapScaler> scaler;
        Wic()->CreateBitmapScaler(&scaler);
        scaler->Initialize(WicFrame(path).Get(), w, h, WICBitmapInterpolationModeFant);
        Want want;
        want.pixels = OverBackground(ReadSource(Upright(scaler.Get(), c.orientation).Get(), GUID_WICPixelFormat32bppPBGRA));
        want.bpp = DepthFor(want.pixels);
        want.exact = false;
        const bool sideways = c.orientation >= 5;
        want.fullWidth = sideways ? c.h : c.w;
        want.fullHeight = sideways ? c.w : c.h;
        scaler.Reset();
        DeleteObject(CheckDecode(Source{path}, want, id).bitmap);
        DeleteFileW(path.c_str());
    }

    // Claimed sizes past the pixel limit fail fast, without trying to allocate them.
    // (65500 is the most JPEG's decoder takes.)
    for (const auto& [w, h] : {std::pair{65500u, 65500u}, std::pair{40000u, 30000u}, std::pair{32768u, 32769u}}) {
        Image image;
        const ULONGLONG start = GetTickCount64();
        const HRESULT hr = DecodeImage(Source{.stream = MemoryStream(ClaimedJpeg(rng, w, h))}, image);
        if (hr != WINCODEC_ERR_IMAGESIZEOUTOFRANGE) Fail("huge claimed size wasn't refused as too large", hr);
        if (GetTickCount64() - start > 2000) Fail("refusing a huge claimed size took long", int(GetTickCount64() - start));
        if (image.bitmap) DeleteObject(image.bitmap);
    }
}

// Large color JPEGs are decoded in bands on several threads, upright straight into the bitmap or else through
// strips; every layout must give WIC's pixels, from a file and from a pasted stream, which each band clones.
void TestBands() {
    std::mt19937 rng(41);
    const struct {
        const WICPixelFormatGUID& format;
        UINT w, h;
        int orientation;
    } kCases[] = {
        {GUID_WICPixelFormat24bppBGR, 2001, 701, 1},   // not a multiple of the 16-row blocks
        {GUID_WICPixelFormat24bppBGR, 2001, 701, 3},
        {GUID_WICPixelFormat24bppBGR, 2001, 701, 6},
        {GUID_WICPixelFormat24bppBGR, 1024, 1024, 1},  // exactly where bands start
        {GUID_WICPixelFormat24bppBGR, 1023, 1024, 8},  // just under it
        {GUID_WICPixelFormat24bppBGR, 64, kMaxBitmapSize, 1},  // tall and thin: bands of a few blocks each
        {GUID_WICPixelFormat8bppGray, 1500, 900, 1},   // gray: counted, not banded
        {GUID_WICPixelFormat32bppCMYK, 1200, 900, 1},  // CMYK: converted, not banded
    };
    for (int id = 0; id < int(std::size(kCases)); ++id) {
        const auto& c = kCases[id];
        const ComPtr<IWICBitmap> source = RandomBitmap(rng, c.format, c.w, c.h);
        const std::wstring path = TempPath(L"band" + std::to_wstring(id) + L".jpg");
        if (FAILED(EncodeFile(path, GUID_ContainerFormatJpeg, source.Get(), &c.format, c.orientation))) {
            Fail("can't write a band test image", id);
            continue;
        }
        const Want want = WantFor(path, c.orientation);
        DeleteObject(CheckDecode(Source{path}, want, id).bitmap);
        DeleteObject(CheckDecode(Source{.stream = MemoryStream(ReadAll(path))}, want, id).bitmap);
        DeleteFileW(path.c_str());
    }

    // Stopped partway through, banded decodes end cleanly, whenever the stop comes.
    const ComPtr<IWICBitmap> big = RandomBitmap(rng, GUID_WICPixelFormat24bppBGR, 4000, 3000);
    const std::wstring path = TempPath(L"band stop.jpg");
    EncodeFile(path, GUID_ContainerFormatJpeg, big.Get());
    const DWORD gdi = GdiObjects();
    for (int delay = 0; delay < 12; delay += 2) {
        std::atomic<bool> stop{false};
        std::thread stopper([&] {
            Sleep(DWORD(delay));
            stop = true;
        });
        Image image;
        const HRESULT hr = DecodeImage(Source{path}, image, nullptr, &stop);
        stopper.join();
        if (hr != E_ABORT && hr != S_OK) Fail("stopped banded decode failed", hr);
        if ((hr == E_ABORT) != !image.bitmap) Fail("stopped banded decode left a bitmap", delay);
        if (image.bitmap) DeleteObject(image.bitmap);
    }
    if (GdiObjects() != gdi) Fail("stopped banded decodes leaked GDI objects", int(GdiObjects() - gdi));
    DeleteFileW(path.c_str());
}

namespace {

// An icon file of the given images, each a PNG or a BMP (a DIB with a doubled height and an AND mask).
std::vector<BYTE> WriteIcon(const std::vector<std::vector<BYTE>>& images, const std::vector<std::pair<UINT, UINT>>& sizes,
                            const std::vector<WORD>& bpps) {
    std::vector<BYTE> out = {0, 0, 1, 0, BYTE(images.size()), 0};
    UINT offset = UINT(6 + 16 * images.size());
    for (size_t i = 0; i < images.size(); ++i) {
        const auto [w, h] = sizes[i];
        const UINT n = UINT(images[i].size());
        out.insert(out.end(), {BYTE(w >= 256 ? 0 : w), BYTE(h >= 256 ? 0 : h), 0, 0, 1, 0, BYTE(bpps[i]), 0});
        for (const UINT v : {n, offset}) out.insert(out.end(), {BYTE(v), BYTE(v >> 8), BYTE(v >> 16), BYTE(v >> 24)});
        offset += n;
    }
    for (const auto& image : images) out.insert(out.end(), image.begin(), image.end());
    return out;
}

std::vector<BYTE> IconDib(std::mt19937& rng, UINT w, UINT h, WORD bpp) {
    BITMAPINFOHEADER header{sizeof(header), LONG(w), LONG(h * 2), 1, bpp, BI_RGB};
    std::vector<BYTE> out(sizeof(header));
    memcpy(out.data(), &header, sizeof(header));
    if (bpp <= 8) {
        for (UINT i = 0; i < (1u << bpp) * 4; ++i) out.push_back(i % 4 == 3 ? 0 : BYTE(rng()));
    }
    const UINT stride = (w * bpp + 31) / 32 * 4, mask = (w + 31) / 32 * 4;
    for (UINT i = 0; i < stride * h; ++i) out.push_back(BYTE(bpp == 32 && i % 4 == 3 ? 255 : rng()));
    out.insert(out.end(), size_t(mask) * h, 0);  // opaque
    return out;
}

}  // namespace

// Of an icon's sizes, the largest is shown, and of those the deepest, wherever it is in the file.
void TestIcons() {
    std::mt19937 rng(43);
    const auto png = [&](UINT size) {
        return EncodeBytes(GUID_ContainerFormatPng, RandomBitmap(rng, GUID_WICPixelFormat32bppBGRA, size, size).Get());
    };
    const struct {
        std::vector<std::vector<BYTE>> images;
        std::vector<std::pair<UINT, UINT>> sizes;
        std::vector<WORD> bpps;
    } kIcons[] = {
        {{png(16), png(64), png(32)}, {{16, 16}, {64, 64}, {32, 32}}, {32, 32, 32}},
        {{png(256), png(48), png(16)}, {{256, 256}, {48, 48}, {16, 16}}, {32, 32, 32}},
        {{IconDib(rng, 32, 32, 4), IconDib(rng, 32, 32, 32), IconDib(rng, 32, 32, 8), IconDib(rng, 16, 16, 32)},
         {{32, 32}, {32, 32}, {32, 32}, {16, 16}},
         {4, 32, 8, 32}},
        {{IconDib(rng, 48, 48, 1)}, {{48, 48}}, {1}},
    };
    for (int id = 0; id < int(std::size(kIcons)); ++id) {
        const auto& icon = kIcons[id];
        const std::wstring path = TempPath(L"icon" + std::to_wstring(id) + L".ico");
        WriteAll(path, WriteIcon(icon.images, icon.sizes, icon.bpps));
        // The frame to show, by WIC's own sizes and depths.
        UINT best = 0;
        UINT64 bestScore = 0;
        for (UINT i = 0; i < icon.images.size(); ++i) {
            const ComPtr<IWICBitmapFrameDecode> frame = WicFrame(path, i);
            UINT w = 0, h = 0;
            if (!frame || FAILED(frame->GetSize(&w, &h))) {
                Fail("test icon frame doesn't decode", id * 10 + int(i));
                continue;
            }
            const UINT64 score = UINT64(w) * h << 8 | BitsOf(FormatOf(frame.Get()));
            if (score > bestScore) {
                bestScore = score;
                best = i;
            }
        }
        const ComPtr<IWICBitmapFrameDecode> frame = WicFrame(path, best);
        Want want;
        want.pixels = OverBackground(ReadSource(frame.Get(), GUID_WICPixelFormat32bppPBGRA));
        want.bpp = DepthFor(want.pixels);
        const Pixels straight = ReadSource(frame.Get());
        want.exact = Is8BitFormat(FormatOf(frame.Get())) &&
                     std::all_of(straight.argb.begin(), straight.argb.end(), [](UINT32 c) { return c >> 24 == 255; });
        want.fullWidth = want.pixels.width;
        want.fullHeight = want.pixels.height;
        DeleteObject(CheckDecode(Source{path}, want, id).bitmap);
        DeleteFileW(path.c_str());
    }
}

// Damaged files, cut short or with bytes changed, fail or decode without crashing or leaking: a decoder may give
// up partway through, and nothing it made may be left behind.
void TestRobustness() {
    std::mt19937 rng(47);
    std::vector<std::vector<BYTE>> samples;
    for (const GUID* container : {&GUID_ContainerFormatPng, &GUID_ContainerFormatJpeg, &GUID_ContainerFormatBmp,
                                  &GUID_ContainerFormatTiff, &GUID_ContainerFormatGif, &GUID_ContainerFormatWmp}) {
        const WICPixelFormatGUID& format =
            *container == GUID_ContainerFormatGif ? GUID_WICPixelFormat8bppIndexed : GUID_WICPixelFormat24bppBGR;
        samples.push_back(EncodeBytes(*container, RandomBitmap(rng, format, 37, 23).Get(), &format));
    }
    samples.push_back(EncodeBytes(GUID_ContainerFormatPng, RandomBitmap(rng, GUID_WICPixelFormat8bppIndexed, 37, 23, 50).Get()));
    Gif gif{20, 10, std::vector<UINT32>(16, 0x123456)};
    gif.frames = {RandomGifFrame(rng, {0, 0, 20, 10}, 16), RandomGifFrame(rng, {3, 2, 15, 9}, 16)};
    gif.frames[0].disposal = 3;
    samples.push_back(WriteGif(gif));

    const DWORD gdi = GdiObjects();
    int decoded = 0;
    for (size_t s = 0; s < samples.size(); ++s) {
        const std::vector<BYTE>& sample = samples[s];
        if (sample.empty()) {
            Fail("can't make a robustness sample", int(s));
            continue;
        }
        const auto attempt = [&](const std::vector<BYTE>& data) {
            Image image;
            bool animated = false;
            if (SUCCEEDED(DecodeImage(Source{.stream = MemoryStream(data)}, image, &animated))) {
                ++decoded;
                if (!image.bitmap || image.width == 0 || image.height == 0 || DibPixels(image.bitmap).width != image.width) {
                    Fail("damaged image decoded to a bad bitmap", int(s));
                }
            }
            if (image.bitmap) DeleteObject(image.bitmap);
        };
        for (size_t cut = 0; cut < sample.size(); cut += std::max<size_t>(1, sample.size() / 97)) {
            attempt(std::vector<BYTE>(sample.begin(), sample.begin() + ptrdiff_t(cut)));
        }
        for (int i = 0; i < 60; ++i) {
            std::vector<BYTE> damaged = sample;
            for (int k = 0; k < 1 + i % 4; ++k) damaged[rng() % damaged.size()] ^= BYTE(1 + rng() % 255);
            attempt(damaged);
        }
    }
    if (decoded == 0) Fail("no damaged image decoded at all, so partial decodes went untested", 0);
    if (GdiObjects() != gdi) Fail("damaged images leaked GDI objects", int(GdiObjects() - gdi));
}

// Decodes running at once on many threads, as the viewer, a save and a copy can, give the same pixels as alone.
void TestConcurrentDecodes() {
    std::mt19937 rng(53);
    std::vector<std::wstring> paths;
    const struct {
        const GUID& container;
        const WICPixelFormatGUID& format;
        UINT w, h;
        UINT colors;
        int orientation;
    } kCases[] = {
        {GUID_ContainerFormatJpeg, GUID_WICPixelFormat24bppBGR, 1500, 900, 0, 1},
        {GUID_ContainerFormatJpeg, GUID_WICPixelFormat24bppBGR, 300, 200, 0, 6},
        {GUID_ContainerFormatPng, GUID_WICPixelFormat24bppBGR, 300, 200, 100, 1},
        {GUID_ContainerFormatPng, GUID_WICPixelFormat32bppBGRA, 300, 200, 0, 1},
        {GUID_ContainerFormatTiff, GUID_WICPixelFormat8bppGray, 300, 200, 0, 8},
    };
    std::vector<Pixels> want;
    for (size_t i = 0; i < std::size(kCases); ++i) {
        const auto& c = kCases[i];
        paths.push_back(TempPath(L"concurrent" + std::to_wstring(i) + L".img"));
        EncodeFile(paths.back(), c.container, RandomBitmap(rng, c.format, c.w, c.h, c.colors).Get(), &c.format,
                   c.orientation);
        want.push_back(Shown(paths.back(), c.orientation));
    }
    std::atomic<int> wrong{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&, t] {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            for (int k = 0; k < 6; ++k) {
                const size_t i = size_t(t + k) % paths.size();
                Image image;
                const Source src = k % 2 ? Source{.stream = MemoryStream(ReadAll(paths[i]))} : Source{paths[i]};
                if (FAILED(DecodeImage(src, image)) || DibPixels(image.bitmap) != want[i]) ++wrong;
                if (image.bitmap) DeleteObject(image.bitmap);
            }
            CoUninitialize();
        });
    }
    for (std::thread& t : threads) t.join();
    if (wrong) Fail("concurrent decodes gave different pixels", wrong);
    for (const std::wstring& path : paths) DeleteFileW(path.c_str());
}
