// The clipboard code, built against a fake clipboard: the clipboard belongs to the user's whole session, so the
// tests never touch the real one. Its parsers, which other programs' data reaches, are tested directly.
#include <windows.h>
#include <shlobj_core.h>
#include <wincodec.h>

#include <iterator>
#include <utility>
#include <vector>

#include "test.h"

namespace {

struct {
    std::vector<std::pair<UINT, HGLOBAL>> data;  // in the order put
    bool open = false;
    int failOpens = 0;
} fake;

BOOL FakeOpenClipboard(HWND) {
    if (fake.failOpens > 0) {
        --fake.failOpens;
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    if (fake.open) Fail("clipboard opened twice", 0);
    fake.open = true;
    return TRUE;
}

BOOL FakeCloseClipboard() {
    if (!fake.open) Fail("clipboard closed while not open", 0);
    fake.open = false;
    return TRUE;
}

BOOL FakeEmptyClipboard() {
    if (!fake.open) return FALSE;
    for (auto& [format, data] : fake.data) GlobalFree(data);
    fake.data.clear();
    return TRUE;
}

HANDLE FakeGetClipboardData(UINT format) {
    if (!fake.open) Fail("clipboard read while not open", int(format));
    for (auto& [f, data] : fake.data) {
        if (f == format) return data;
    }
    return nullptr;
}

HANDLE FakeSetClipboardData(UINT format, HANDLE data) {
    if (!fake.open) return nullptr;
    for (auto& [f, d] : fake.data) {
        if (f == format) {
            GlobalFree(d);
            d = data;
            return data;
        }
    }
    fake.data.emplace_back(format, data);
    return data;
}

BOOL FakeIsClipboardFormatAvailable(UINT format) {
    for (auto& [f, data] : fake.data) {
        if (f == format) return TRUE;
    }
    return FALSE;
}

}  // namespace

#define OpenClipboard FakeOpenClipboard
#define CloseClipboard FakeCloseClipboard
#define EmptyClipboard FakeEmptyClipboard
#define GetClipboardData FakeGetClipboardData
#define SetClipboardData FakeSetClipboardData
#define IsClipboardFormatAvailable FakeIsClipboardFormatAvailable
#include "platform/clipboard.cpp"
#undef OpenClipboard
#undef CloseClipboard
#undef EmptyClipboard
#undef GetClipboardData
#undef SetClipboardData
#undef IsClipboardFormatAvailable

#include "imaging/decoder.h"
#include "imaging/encoder.h"

void FakeClipboardClear() {
    for (auto& [format, data] : fake.data) GlobalFree(data);
    fake.data.clear();
    fake.failOpens = 0;
}

void FakeClipboardPut(UINT format, const std::vector<BYTE>& data) {
    const HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, std::max<size_t>(1, data.size()));
    memcpy(GlobalLock(memory), data.data(), data.size());
    GlobalUnlock(memory);
    fake.open = true;
    FakeSetClipboardData(format, memory);
    fake.open = false;
}

std::vector<BYTE> FakeClipboardGet(UINT format) {
    for (auto& [f, data] : fake.data) {
        if (f != format) continue;
        const BYTE* p = static_cast<const BYTE*>(GlobalLock(data));
        std::vector<BYTE> bytes(p, p + GlobalSize(data));
        GlobalUnlock(data);
        return bytes;
    }
    return {};
}

std::vector<UINT> FakeClipboardFormats() {
    std::vector<UINT> formats;
    for (auto& [f, data] : fake.data) formats.push_back(f);
    return formats;
}

void FakeClipboardFailOpens(int count) {
    fake.failOpens = count;
}

UINT FakeClipboardPngFormat() {
    return PngFormat();
}

UINT FakeClipboardHtmlFormat() {
    return HtmlFormat();
}

// A clipboard DIB is accepted only if its header is one Windows writes and its colors and pixels fit the data.
void TestClipboardDib() {
    struct Dib {
        BITMAPINFOHEADER header;
        BYTE rest[1024];
    };
    const auto measure = [](const Dib& dib, SIZE_T size, UINT64& offset) {
        return MeasureDib(reinterpret_cast<const BYTE*>(&dib), size, offset);
    };
    UINT64 offset = 0;
    // 3x2 at 24bpp: rows of 9 bytes padded to 12.
    Dib dib{{sizeof(BITMAPINFOHEADER), 3, 2, 1, 24, BI_RGB}};
    const SIZE_T size24 = sizeof(BITMAPINFOHEADER) + 24;
    if (measure(dib, size24, offset) != size24 || offset != sizeof(BITMAPINFOHEADER)) Fail("valid 24bpp DIB refused", 0);
    if (measure(dib, size24 - 1, offset)) Fail("truncated DIB accepted", 0);
    dib.header.biHeight = -2;  // top-down
    if (measure(dib, size24, offset) != size24) Fail("top-down DIB refused", 0);

    // 8bpp: a full color table unless biClrUsed says fewer.
    dib.header.biBitCount = 8;
    const SIZE_T size8 = sizeof(BITMAPINFOHEADER) + 256 * 4 + 8;
    if (measure(dib, size8, offset) != size8 || offset != sizeof(BITMAPINFOHEADER) + 1024) Fail("8bpp DIB refused", 0);
    dib.header.biClrUsed = 2;
    if (measure(dib, sizeof(BITMAPINFOHEADER) + 8 + 8, offset) != sizeof(BITMAPINFOHEADER) + 16) {
        Fail("8bpp DIB with 2 colors refused", 0);
    }
    dib.header.biClrUsed = 257;
    if (measure(dib, sizeof(dib), offset)) Fail("DIB with more colors than its depth accepted", 257);

    // 32bpp bitfields: three masks follow a plain header.
    dib.header = {sizeof(BITMAPINFOHEADER), 3, 2, 1, 32, BI_BITFIELDS};
    if (measure(dib, sizeof(BITMAPINFOHEADER) + 12 + 24, offset) != sizeof(BITMAPINFOHEADER) + 36 ||
        offset != sizeof(BITMAPINFOHEADER) + 12) {
        Fail("32bpp bitfields DIB refused", 0);
    }

    // Compressed, odd depths and sizes, and unknown headers never reach a decoder.
    const struct {
        DWORD headerSize;
        LONG width, height;
        WORD bpp;
        DWORD compression;
    } bad[] = {{sizeof(BITMAPINFOHEADER), 3, 2, 8, BI_RLE8}, {sizeof(BITMAPINFOHEADER), 3, 2, 24, BI_JPEG},
               {sizeof(BITMAPINFOHEADER), 3, 2, 24, BI_PNG},  {sizeof(BITMAPINFOHEADER), 3, 2, 24, BI_BITFIELDS},
               {sizeof(BITMAPINFOHEADER), 3, 2, 2, BI_RGB},   {sizeof(BITMAPINFOHEADER), 0, 2, 24, BI_RGB},
               {sizeof(BITMAPINFOHEADER), -3, 2, 24, BI_RGB}, {sizeof(BITMAPINFOHEADER), 3, 0, 24, BI_RGB},
               {sizeof(BITMAPINFOHEADER), 0x7FFFFFFF, 0x7FFFFFFF, 32, BI_RGB},
               {sizeof(BITMAPINFOHEADER) + 1, 3, 2, 24, BI_RGB}, {12, 3, 2, 24, BI_RGB}};
    for (int i = 0; i < int(std::size(bad)); ++i) {
        dib.header = {bad[i].headerSize, bad[i].width, bad[i].height, 1, bad[i].bpp, bad[i].compression};
        if (measure(dib, sizeof(dib), offset)) Fail("bad clipboard DIB accepted", i);
    }
    dib.header = {sizeof(BITMAPINFOHEADER), 3, 2, 2, 24, BI_RGB};  // two planes
    if (measure(dib, sizeof(dib), offset)) Fail("DIB with two planes accepted", 0);
    if (measure(dib, sizeof(BITMAPINFOHEADER) - 1, offset)) Fail("DIB shorter than its header accepted", 0);
}

// The first file of a DROPFILES list, as Explorer copies or drops it, or none if the list is malformed.
void TestClipboardFiles() {
    const auto first = [](DWORD pFiles, BOOL wide, const wchar_t* names, size_t chars, std::wstring& path) {
        const SIZE_T size = sizeof(DROPFILES) + chars * sizeof(wchar_t);
        const HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE, size);
        BYTE* const p = static_cast<BYTE*>(GlobalLock(data));
        const DROPFILES drop{pFiles, {}, FALSE, wide};
        memcpy(p, &drop, sizeof(drop));
        memcpy(p + sizeof(drop), names, chars * sizeof(wchar_t));
        GlobalUnlock(data);
        path.clear();
        const HRESULT hr = FirstFileOf(data, path);
        GlobalFree(data);
        return hr;
    };
    std::wstring path;
    const wchar_t names[] = L"C:\\a.png\0C:\\b.png\0";
    if (first(sizeof(DROPFILES), TRUE, names, std::size(names), path) != S_OK || path != L"C:\\a.png") {
        Fail("first dropped file not read", 0);
    }
    if (first(sizeof(DROPFILES), FALSE, names, std::size(names), path) != S_FALSE) Fail("ANSI file list accepted", 0);
    if (first(sizeof(DROPFILES) - 1, TRUE, names, std::size(names), path) != S_FALSE) {
        Fail("file list overlapping its header accepted", 0);
    }
    if (first(DWORD(sizeof(DROPFILES) + sizeof(names)), TRUE, names, std::size(names), path) != S_FALSE) {
        Fail("file list past its data accepted", 0);
    }
    if (first(sizeof(DROPFILES), TRUE, L"\0", 2, path) != S_FALSE) Fail("empty file name accepted", 0);
    if (FirstFileOf(nullptr, path) != S_FALSE) Fail("missing file list accepted", 0);
}

// The file name an <img src> URL gives a pasted image: decoded, but never a path, a hidden name, or one that
// reads differently than it is.
void TestClipboardName() {
    const struct {
        const char* url;
        const wchar_t* name;
    } cases[] = {
        {"https://example.com/images/cat.png", L"cat.png"},
        {"https://example.com/a%20b.JPG?size=large#top", L"a b.JPG"},
        {"https://example.com/caf%C3%A9.webp", L"caf\u00E9.webp"},
        {"cat.png", L"cat.png"},
        {"https://example.com/cat.txt", L""},
        {"https://example.com/", L""},
        {"https://example.com/.png", L""},
        {"https://example.com/a%2F..%2Fb.png", L""},
        {"https://example.com/a%5Cb.png", L""},
        {"https://example.com/a%3Ab.png", L""},
        {"https://example.com/a%0Ab.png", L""},
        {"https://example.com/a%E2%80%AEgnp.exe.png", L""},
        {"https://example.com/a%E2%81%A6b.png", L""},
        {"https://example.com/a%FFb.png", L""},
        {"https://example.com/a%zzb.png", L""},
        {"https://example.com/a%2", L""},
    };
    for (int i = 0; i < int(std::size(cases)); ++i) {
        if (ImageNameOf(cases[i].url) != cases[i].name) Fail("wrong name from an image URL", i);
    }
    const std::string longName = std::string(300, 'a') + ".png";
    if (!ImageNameOf(longName).empty()) Fail("name longer than a file name accepted", 0);
}

// A pasted PNG ends with its IEND chunk, whatever follows it in the clipboard's memory.
void TestPngLength() {
    std::vector<BYTE> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    const auto chunk = [&](const char* type, UINT length) {
        const BYTE header[8] = {BYTE(length >> 24), BYTE(length >> 16), BYTE(length >> 8), BYTE(length),
                                BYTE(type[0]), BYTE(type[1]), BYTE(type[2]), BYTE(type[3])};
        png.insert(png.end(), header, header + 8);
        png.insert(png.end(), length + 4, 0);  // data and CRC
    };
    chunk("IHDR", 13);
    chunk("IDAT", 300);
    if (PngLength(png.data(), png.size()) != png.size()) Fail("PNG without IEND not kept whole", 0);
    chunk("IEND", 0);
    const SIZE_T length = png.size();
    png.insert(png.end(), 16, 0xCC);  // slack after the data
    if (PngLength(png.data(), png.size()) != length) Fail("PNG not trimmed after IEND", int(png.size()));
    png[8 + 8 + 13 + 4 + 3] = 0xFF;  // an IDAT length that runs past the data
    if (PngLength(png.data(), png.size()) != png.size()) Fail("PNG with a bad chunk length trimmed", 0);
}

namespace {

std::vector<BYTE> Bytes(HGLOBAL memory) {
    const BYTE* p = static_cast<const BYTE*>(GlobalLock(memory));
    std::vector<BYTE> bytes(p, p + GlobalSize(memory));
    GlobalUnlock(memory);
    return bytes;
}

std::vector<BYTE> StreamBytes(IStream* stream) {
    STATSTG stat;
    std::vector<BYTE> bytes;
    if (!stream || FAILED(stream->Stat(&stat, STATFLAG_NONAME)) || FAILED(stream->Seek({}, STREAM_SEEK_SET, nullptr))) {
        return bytes;
    }
    bytes.resize(size_t(stat.cbSize.QuadPart));
    ULONG n = 0;
    stream->Read(bytes.data(), ULONG(bytes.size()), &n);
    stream->Seek({}, STREAM_SEEK_SET, nullptr);
    return bytes;
}

// The pixels of a clipboard DIB: bottom-up 24bpp rows, as opaque top-down pixels.
Pixels DibOnClipboard(const std::vector<BYTE>& dib) {
    BITMAPINFOHEADER h;
    if (dib.size() < sizeof(h)) return {};
    memcpy(&h, dib.data(), sizeof(h));
    if (h.biBitCount != 24 || h.biCompression != BI_RGB || h.biHeight <= 0 || h.biWidth <= 0) return {};
    Pixels p{UINT(h.biWidth), UINT(h.biHeight)};
    const size_t stride = (size_t(h.biWidth) * 3 + 3) & ~size_t(3);
    if (dib.size() < sizeof(h) + stride * h.biHeight) return {};
    p.argb.resize(size_t(p.width) * p.height);
    for (UINT y = 0; y < p.height; ++y) {
        const BYTE* row = dib.data() + sizeof(h) + (p.height - 1 - y) * stride;
        for (UINT x = 0; x < p.width; ++x) {
            p.argb[size_t(y) * p.width + x] = 0xFF000000 | row[x * 3] | row[x * 3 + 1] << 8 | row[x * 3 + 2] << 16;
        }
    }
    return p;
}

Pixels Opaque(Pixels p) {
    for (UINT32& c : p.argb) c |= 0xFF000000;
    return p;
}

std::vector<BYTE> Html(const std::string& body) {
    const std::string html = "Version:0.9\r\nStartHTML:0\r\n" + body;
    return std::vector<BYTE>(html.begin(), html.end());
}

}  // namespace

// Copy puts every pixel and its alpha on the clipboard as PNG, plus a plain DIB for other programs; Paste reads
// back PNG first, then a DIB, then a copied file, naming a pasted image after its <img> in the HTML beside it.
void TestClipboardRoundTrip() {
    std::mt19937 rng(73);
    const std::wstring dir = TempDir(L"clipboard");
    const std::wstring alpha = dir + L"alpha.png", indexed = dir + L"indexed.png";
    EncodeFile(alpha, GUID_ContainerFormatPng, RandomBitmap(rng, GUID_WICPixelFormat32bppBGRA, 45, 29).Get());
    EncodeFile(indexed, GUID_ContainerFormatPng, RandomBitmap(rng, GUID_WICPixelFormat8bppIndexed, 45, 29, 13).Get());

    for (const std::wstring& path : {alpha, indexed}) {
        const Pixels want = ReadSource(WicFrame(path).Get());
        Image image;
        DecodeImage(Source{path}, image);
        // As the viewer copies: from the file, or from its bitmap when that is exact.
        for (const Source& src : {Source{path}, Source{path, nullptr, image.exact ? image.bitmap : nullptr}}) {
            HGLOBAL png = nullptr, dib = nullptr;
            if (FAILED(EncodeClipboardImage(src, png, dib)) || !png || !dib) {
                Fail("image didn't encode for the clipboard", 0);
                continue;
            }
            const std::vector<BYTE> pngBytes = Bytes(png), dibBytes = Bytes(dib);
            if (ReadSource(WicFrame(pngBytes).Get()) != want) Fail("copied PNG differs from the image", 0);
            UINT64 offset = 0;
            if (MeasureDib(dibBytes.data(), dibBytes.size(), offset) != sizeof(BITMAPINFOHEADER) + ((45 * 3 + 3) & ~3) * 29) {
                Fail("copied DIB isn't one Paste accepts", 0);
            }
            if (DibOnClipboard(dibBytes) != Opaque(want)) Fail("copied DIB differs from the image's colors", 0);

            if (!SetClipboardImage(nullptr, png, dib)) Fail("image not put on the clipboard", 0);
            const std::vector<UINT> formats = FakeClipboardFormats();
            if (formats != std::vector<UINT>{PngFormat(), CF_DIB}) Fail("clipboard formats not PNG then DIB", 0);

            Source pasted;
            std::wstring name;
            UINT64 bytes = 0;
            if (ReadClipboardImage(nullptr, pasted, name, bytes) != S_OK || !pasted.stream) {
                Fail("copied image didn't paste", 0);
                continue;
            }
            if (name != L"Pasted image.png" || bytes != pngBytes.size()) Fail("pasted PNG named or sized wrong", 0);
            Image back;
            if (FAILED(DecodeImage(pasted, back)) || DibPixels(back.bitmap) != Shown(path)) Fail("pasted PNG decoded wrong", 0);
            if (back.bitmap) DeleteObject(back.bitmap);
        }
        if (image.bitmap) DeleteObject(image.bitmap);
    }

    // A DIB alone, as many programs copy, is read as a BMP file of the same pixels.
    FakeClipboardClear();
    HGLOBAL png = nullptr, dib = nullptr;
    EncodeClipboardImage(Source{alpha}, png, dib);
    const std::vector<BYTE> dibBytes = Bytes(dib);
    GlobalFree(png);
    GlobalFree(dib);
    FakeClipboardPut(CF_DIB, dibBytes);
    Source pasted;
    std::wstring name;
    UINT64 bytes = 7;
    if (ReadClipboardImage(nullptr, pasted, name, bytes) != S_OK || name != L"Pasted image.bmp" || bytes != 0) {
        Fail("pasted DIB not read as a BMP", 0);
    } else {
        const std::vector<BYTE> bmp = StreamBytes(pasted.stream.Get());
        if (bmp.size() != sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + ((45 * 3 + 3) & ~3) * 29 ||
            bmp[0] != 'B' || bmp[1] != 'M') {
            Fail("pasted DIB stream isn't a BMP file of its size", int(bmp.size()));
        }
        Image image;
        if (FAILED(DecodeImage(pasted, image)) || DibPixels(image.bitmap) != DibOnClipboard(dibBytes)) {
            Fail("pasted DIB decoded wrong", 0);
        }
        if (image.bitmap) DeleteObject(image.bitmap);
    }

    // A V5 DIB that links its color profile from a path is read as sRGB, so the path is never followed.
    BITMAPV5HEADER v5{sizeof(v5), 2, 2, 1, 24, BI_RGB};
    v5.bV5CSType = PROFILE_LINKED;
    v5.bV5ProfileData = sizeof(v5) + 16;
    v5.bV5ProfileSize = 20;
    std::vector<BYTE> linked(sizeof(v5));
    memcpy(linked.data(), &v5, sizeof(v5));
    linked.insert(linked.end(), 16, 0x80);  // two rows of 2 pixels, padded
    const char* share = "\\\\server\\share\\x.icc";
    linked.insert(linked.end(), share, share + 20);
    FakeClipboardClear();
    FakeClipboardPut(CF_DIB, linked);
    pasted = {};
    if (ReadClipboardImage(nullptr, pasted, name, bytes) != S_OK) {
        Fail("V5 DIB with a linked profile didn't paste", 0);
    } else {
        const std::vector<BYTE> bmp = StreamBytes(pasted.stream.Get());
        BITMAPV5HEADER read{};
        if (bmp.size() >= sizeof(BITMAPFILEHEADER) + sizeof(read)) memcpy(&read, bmp.data() + sizeof(BITMAPFILEHEADER), sizeof(read));
        if (read.bV5CSType != LCS_sRGB || read.bV5ProfileData || read.bV5ProfileSize) Fail("linked profile kept", 0);
        Image image;
        if (FAILED(DecodeImage(pasted, image))) Fail("V5 DIB didn't decode", 0);
        if (image.bitmap) DeleteObject(image.bitmap);
    }

    // A pasted PNG is cut at IEND, whatever follows in the clipboard's memory, and named from the HTML.
    std::vector<BYTE> pngBytes = ReadAll(alpha);
    const size_t length = pngBytes.size();
    pngBytes.insert(pngBytes.end(), 100, 0xEE);
    const struct {
        std::string html;
        const wchar_t* name;
    } kHtml[] = {
        {"<html><body><img alt=\"x\" src=\"https://example.com/photos/cat.png?w=200\"></body></html>", L"cat.png"},
        {"<img src='https://example.com/dog.jpg'>", L"dog.jpg"},
        {"<img src=https://example.com/unquoted.png>", L"Pasted image.png"},
        {"<img src=\"https://example.com/evil%2F..%2Fx.png\">", L"Pasted image.png"},
        {"<p>no image here</p>", L"Pasted image.png"},
        {std::string(70000, ' ') + "<img src=\"https://example.com/far.png\">", L"Pasted image.png"},
    };
    for (int i = 0; i < int(std::size(kHtml)); ++i) {
        FakeClipboardClear();
        FakeClipboardPut(PngFormat(), pngBytes);
        FakeClipboardPut(HtmlFormat(), Html(kHtml[i].html));
        pasted = {};
        if (ReadClipboardImage(nullptr, pasted, name, bytes) != S_OK) {
            Fail("PNG with HTML didn't paste", i);
            continue;
        }
        if (name != kHtml[i].name) Fail("pasted image named wrong from its HTML", i);
        if (bytes != length || StreamBytes(pasted.stream.Get()).size() != length) Fail("pasted PNG not cut at IEND", i);
    }

    // What can't be pasted, and why.
    const struct {
        UINT format;
        std::vector<BYTE> data;
        HRESULT hr;
    } kBad[] = {
        {PngFormat(), {'n', 'o', 't', ' ', 'p', 'n', 'g', '!', '!'}, WINCODEC_ERR_UNKNOWNIMAGEFORMAT},
        {PngFormat(), {0x89, 'P', 'N'}, WINCODEC_ERR_UNKNOWNIMAGEFORMAT},
        {CF_DIB, {1, 2, 3}, WINCODEC_ERR_BADIMAGE},
        {CF_TEXT, {'h', 'i', 0}, S_FALSE},
    };
    for (int i = 0; i < int(std::size(kBad)); ++i) {
        FakeClipboardClear();
        FakeClipboardPut(kBad[i].format, kBad[i].data);
        pasted = {};
        if (ReadClipboardImage(nullptr, pasted, name, bytes) != kBad[i].hr) Fail("bad clipboard data gave the wrong result", i);
    }
    FakeClipboardClear();
    if (ReadClipboardImage(nullptr, pasted, name, bytes) != S_FALSE) Fail("empty clipboard pasted something", 0);

    // A file copied in Explorer pastes as its path; PNG data wins over it.
    std::vector<BYTE> drop(sizeof(DROPFILES));
    const DROPFILES header{sizeof(DROPFILES), {}, FALSE, TRUE};
    memcpy(drop.data(), &header, sizeof(header));
    const std::wstring files = alpha + L'\0' + indexed + L'\0' + L'\0';
    drop.insert(drop.end(), reinterpret_cast<const BYTE*>(files.data()),
                reinterpret_cast<const BYTE*>(files.data() + files.size()));
    FakeClipboardPut(CF_HDROP, drop);
    pasted = {};
    if (ReadClipboardImage(nullptr, pasted, name, bytes) != S_OK || pasted.path != alpha || pasted.stream) {
        Fail("copied file didn't paste as its path", 0);
    }
    FakeClipboardPut(PngFormat(), ReadAll(indexed));
    pasted = {};
    if (ReadClipboardImage(nullptr, pasted, name, bytes) != S_OK || !pasted.stream || !pasted.path.empty()) {
        Fail("PNG didn't win over a copied file", 0);
    }
    const HGLOBAL dropped = GlobalAlloc(GMEM_MOVEABLE, drop.size());
    memcpy(GlobalLock(dropped), drop.data(), drop.size());
    GlobalUnlock(dropped);
    if (DroppedFile(HDROP(dropped)) != alpha) Fail("dropped file not read", 0);
    GlobalFree(dropped);

    // Another program holding the clipboard for a moment is waited out; one holding it longer is an error.
    FakeClipboardClear();
    FakeClipboardPut(PngFormat(), ReadAll(indexed));
    FakeClipboardFailOpens(4);
    pasted = {};
    if (ReadClipboardImage(nullptr, pasted, name, bytes) != S_OK) Fail("busy clipboard not retried", 0);
    FakeClipboardFailOpens(5);
    if (ReadClipboardImage(nullptr, pasted, name, bytes) != HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)) {
        Fail("clipboard held by another program gave the wrong error", 0);
    }
    FakeClipboardFailOpens(5);
    png = GlobalAlloc(GMEM_MOVEABLE, 8);
    dib = GlobalAlloc(GMEM_MOVEABLE, 8);
    if (SetClipboardImage(nullptr, png, dib)) Fail("copy to a held clipboard reported success", 0);
    FakeClipboardClear();
}
