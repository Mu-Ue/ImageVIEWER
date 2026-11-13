#include "imaging/decoder.h"

#include <algorithm>
#include <cstring>
#include <span>
#include <type_traits>

#if defined(_M_X64) || defined(_M_IX86)
#include <emmintrin.h>
#endif

#include "config.h"
#include "imaging/animation.h"
#include "platform/handle_stream.h"
#include "platform/pages.h"

using Microsoft::WRL::ComPtr;

namespace {

// Reads the EXIF orientation tag, if any, from where the container keeps it. The first query costs about a
// millisecond, so containers WIC reads no orientation from (PNG, GIF, BMP) skip it, and JPEG and TIFF skip the
// path the other one uses. HEIF's decoder turns the image as its own rotation and mirror boxes say, which the
// format puts above any EXIF tag, so the tag isn't read: applying it as well would turn the image twice.
int ReadOrientation(IWICBitmapFrameDecode* frame, const GUID& container) {
    static constexpr const wchar_t* kQueries[] = {L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}"};
    std::span<const wchar_t* const> queries = kQueries;
    if (container == GUID_ContainerFormatJpeg) {
        queries = queries.first(1);
    } else if (container == GUID_ContainerFormatTiff) {
        queries = queries.last(1);
    } else if (container == GUID_ContainerFormatPng || container == GUID_ContainerFormatGif ||
               container == GUID_ContainerFormatBmp || container == GUID_ContainerFormatHeif) {
        return 1;
    }
    ComPtr<IWICMetadataQueryReader> reader;
    if (FAILED(frame->GetMetadataQueryReader(&reader))) return 1;
    for (const wchar_t* query : queries) {
        PROPVARIANT value;
        PropVariantInit(&value);
        if (SUCCEEDED(reader->GetMetadataByName(query, &value))) {
            const int orientation = value.vt == VT_UI2 ? value.uiVal : 1;
            PropVariantClear(&value);
            if (orientation >= 1 && orientation <= 8) return orientation;
        }
    }
    return 1;
}

// Bits per pixel of a frame's native pixel format, or 0 if unknown.
UINT BitsPerPixel(IWICImagingFactory* wic, IWICBitmapSource* source) {
    WICPixelFormatGUID format;
    ComPtr<IWICComponentInfo> info;
    ComPtr<IWICPixelFormatInfo> formatInfo;
    UINT bpp = 0;
    if (SUCCEEDED(source->GetPixelFormat(&format)) && SUCCEEDED(wic->CreateComponentInfo(format, &info)) &&
        SUCCEEDED(info.As(&formatInfo)) && SUCCEEDED(formatInfo->GetBitsPerPixel(&bpp))) {
        return bpp;
    }
    return 0;
}

}  // namespace

bool CanBeTransparent(IWICImagingFactory* wic, IWICBitmapSource* source) {
    // Indexed formats are transparent only if their palette is, although WIC says they never are.
    ComPtr<IWICPalette> palette;
    BOOL alpha = TRUE;
    if (SUCCEEDED(wic->CreatePalette(&palette)) && SUCCEEDED(source->CopyPalette(palette.Get())) &&
        SUCCEEDED(palette->HasAlpha(&alpha))) {
        return alpha;
    }
    WICPixelFormatGUID format;
    ComPtr<IWICComponentInfo> info;
    ComPtr<IWICPixelFormatInfo2> formatInfo;
    BOOL transparency = TRUE;
    if (FAILED(source->GetPixelFormat(&format)) || FAILED(wic->CreateComponentInfo(format, &info)) ||
        FAILED(info.As(&formatInfo)) || FAILED(formatInfo->SupportsTransparency(&transparency))) {
        return true;
    }
    return transparency;
}

namespace {

// Icons hold several sizes and frame 0 is often the smallest, so pick the largest, deepest one.
HRESULT GetFrame(IWICImagingFactory* wic, IWICBitmapDecoder* decoder, ComPtr<IWICBitmapFrameDecode>& out) {
    HRESULT hr = decoder->GetFrame(0, &out);
    GUID container;
    UINT count = 0;
    if (FAILED(hr) || FAILED(decoder->GetContainerFormat(&container)) || container != GUID_ContainerFormatIco ||
        FAILED(decoder->GetFrameCount(&count))) {
        return hr;
    }
    UINT64 best = 0;
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IWICBitmapFrameDecode> frame;
        UINT w = 0, h = 0;
        if (FAILED(decoder->GetFrame(i, &frame)) || FAILED(frame->GetSize(&w, &h))) continue;
        const UINT64 score = (UINT64(w) * h << 8) | BitsPerPixel(wic, frame.Get());
        if (score > best) {
            best = score;
            out = frame;
        }
    }
    return hr;
}

// Creates a top-down DIB section; colors is the color table for bpp <= 8.
HBITMAP CreateDib(UINT width, UINT height, WORD bpp, const UINT32* colors, UINT count, BYTE*& bits) {
    struct {
        BITMAPINFOHEADER header;
        UINT32 colors[256];  // RGBQUAD layout
    } bmi{};
    bmi.header.biSize = sizeof(bmi.header);
    bmi.header.biWidth = LONG(width);
    bmi.header.biHeight = -LONG(height);  // top-down
    bmi.header.biPlanes = 1;
    bmi.header.biBitCount = bpp;
    bmi.header.biCompression = BI_RGB;
    bmi.header.biClrUsed = count;
    std::copy_n(colors, count, bmi.colors);
    void* p = nullptr;
    HBITMAP bitmap =
        CreateDIBSection(nullptr, reinterpret_cast<BITMAPINFO*>(&bmi), DIB_RGB_COLORS, &p, nullptr, 0);
    bits = static_cast<BYTE*>(p);
    return bitmap;
}

// Maps up to 256 distinct 0x00RRGGBB colors to palette indices.
struct Palette {
    static constexpr UINT32 kUsed = 0x1000000;
    UINT32 colors[256];
    UINT count = 0;
    UINT32 keys[4096] = {};  // color | kUsed, 0 = empty slot
    BYTE indices[4096];

    // Returns the color's index, adding it if new, or -1 once the palette is full.
    int Find(UINT32 color) {
        // Folding the high bits in first keeps palettes built from regular color steps from clustering.
        UINT i = ((color ^ color >> 15) * 0x9E3779B1u) >> 20;
        for (; keys[i] != 0; i = (i + 1) & 4095) {
            if (keys[i] == (color | kUsed)) return indices[i];
        }
        if (count == 256) return -1;
        keys[i] = color | kUsed;
        indices[i] = BYTE(count);
        colors[count] = color;
        return int(count++);
    }

    // Find, through the color asked for last, which most pixels repeat.
    int Index(UINT32 color) {
        if (color != last) {
            const int index = Find(color);
            if (index < 0) return -1;
            last = color;
            lastIndex = BYTE(index);
        }
        return lastIndex;
    }
    UINT32 last = ~0u;
    BYTE lastIndex = 0;
};

// Packs a row of palette indices at Bpp bits each, leftmost pixel in the high bits.
template <UINT Bpp>
void PackRow(const BYTE* in, UINT width, BYTE* d) {
    constexpr UINT perByte = 8 / Bpp;
    UINT x = 0;
    for (; x + perByte <= width; x += perByte) {
        BYTE packed = 0;
        for (UINT k = 0; k < perByte; ++k) packed = BYTE(packed << Bpp | in[x + k]);
        *d++ = packed;
    }
    if (x < width) {
        BYTE packed = 0;
        for (UINT k = 0; k < perByte; ++k) packed = BYTE(packed << Bpp | (x + k < width ? in[x + k] : 0));
        *d = packed;
    }
}

// Where source pixel (x, y) lands in an upright buffer, in bytes: origin + x * stepX + y * stepY.
struct Placement {
    ptrdiff_t origin, stepX, stepY;
};

// For a width x height source with the given EXIF orientation, stored upright with pixel bytes per
// pixel and stride bytes per upright row.
Placement Place(int orientation, UINT width, UINT height, ptrdiff_t pixel, ptrdiff_t stride) {
    const ptrdiff_t right = ptrdiff_t(width) - 1, bottom = ptrdiff_t(height) - 1;
    switch (orientation) {
        case 2: return {right * pixel, -pixel, stride};                    // mirrored
        case 3: return {bottom * stride + right * pixel, -pixel, -stride};  // rotated 180
        case 4: return {bottom * stride, pixel, -stride};                   // flipped
        case 5: return {0, stride, pixel};                                  // transposed
        case 6: return {bottom * pixel, stride, -pixel};                    // rotated 90 clockwise
        case 7: return {right * stride + bottom * pixel, -stride, -pixel};  // transversed
        case 8: return {right * stride, -stride, pixel};                    // rotated 270 clockwise
        default: return {0, pixel, stride};
    }
}

// The upright rectangle that source rows [0, rows) land in.
RECT PlacedRows(int orientation, UINT width, UINT height, UINT rows) {
    const LONG w = LONG(width), h = LONG(height), n = LONG(rows);  // source size
    switch (orientation) {
        case 1: case 2: return {0, 0, w, n};
        case 3: case 4: return {0, h - n, w, h};
        case 5: case 8: return {0, 0, n, w};
        default: return {h - n, 0, h, w};
    }
}

// Calls visit(i, offset) for the source pixels of rows [y0, y1): i counts pixels from (0, y0) in row
// order, offset is where the pixel lands. Stops early if visit returns false. Sideways images are
// walked in column tiles, so their writes, which run down upright columns, stay in cache.
template <ptrdiff_t Pixel, typename Visit>
bool ForEachPixel(UINT width, UINT y0, UINT y1, const Placement& place, bool sideways, Visit&& visit) {
    const auto walk = [&](auto stepX) {
        const UINT tile = sideways ? 64 : width;
        for (UINT x0 = 0; x0 < width; x0 += tile) {
            const UINT x1 = std::min(width, x0 + tile);
            for (UINT y = y0; y < y1; ++y) {
                size_t i = size_t(y - y0) * width + x0;
                ptrdiff_t offset = place.origin + ptrdiff_t(x0) * stepX + ptrdiff_t(y) * place.stepY;
                for (UINT x = x0; x < x1; ++x, ++i, offset += stepX) {
                    if (!visit(i, offset)) return false;
                }
            }
        }
        return true;
    };
    // Upright rows, the common case, get a constant step the compiler can optimize for.
    return place.stepX == Pixel ? walk(std::integral_constant<ptrdiff_t, Pixel>{}) : walk(place.stepX);
}

// Stores the palette index of each of n 32bpp pixels that land in order at d, or returns false once the palette is
// full. Runs of the color last stored, as screenshots and drawings have, are compared four pixels at a time.
bool StoreInOrder(const BYTE* pixels, size_t n, Palette& palette, BYTE* d) {
    const auto store = [&](size_t i) {
        UINT32 color;
        memcpy(&color, pixels + i * 4, 4);
        const int index = palette.Index(color & 0xFFFFFF);
        d[i] = BYTE(index);
        return index >= 0;
    };
    size_t i = 0;
#if defined(_M_X64) || defined(_M_IX86)
    const __m128i rgb = _mm_set1_epi32(0xFFFFFF);
    while (i + 4 <= n) {
        const __m128i v = _mm_and_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(pixels + i * 4)), rgb);
        if (_mm_movemask_epi8(_mm_cmpeq_epi32(v, _mm_set1_epi32(int(palette.last)))) == 0xFFFF) {
            memset(d + i, palette.lastIndex, 4);
            i += 4;
            continue;
        }
        for (const size_t end = i + 4; i < end; ++i) {
            if (!store(i)) return false;
        }
    }
#endif
    for (; i < n; ++i) {
        if (!store(i)) return false;
    }
    return true;
}

// Sets out to what each of the 256 values of an 8bpp gray or palette source converts to as 32bppPBGRA: a row of
// every value, in the source's format and with its palette, is converted as its pixels would be.
HRESULT ConvertValues(IWICImagingFactory* wic, IWICBitmapSource* source, const WICPixelFormatGUID& format,
                      BYTE (&out)[256 * 4]) {
    BYTE all[256];
    for (UINT v = 0; v < 256; ++v) all[v] = BYTE(v);
    ComPtr<IWICBitmap> row;
    ComPtr<IWICPalette> palette;
    ComPtr<IWICFormatConverter> converter;
    HRESULT hr = wic->CreateBitmapFromMemory(256, 1, format, 256, 256, all, &row);
    if (SUCCEEDED(hr) && format == GUID_WICPixelFormat8bppIndexed) {
        hr = wic->CreatePalette(&palette);
        if (SUCCEEDED(hr)) hr = source->CopyPalette(palette.Get());
        if (SUCCEEDED(hr)) hr = row->SetPalette(palette.Get());
    }
    if (SUCCEEDED(hr)) hr = wic->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr)) {
        hr = converter->Initialize(row.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom);
    }
    if (SUCCEEDED(hr)) hr = converter->CopyPixels(nullptr, sizeof(out), sizeof(out), out);
    return hr;
}

// Copies rows [y0, y1) of a strip of In-byte BGR(A) pixels, width per row, to where they land in a 24bpp bitmap.
// A sideways image is copied a source column at a time, which lands along an upright row: the writes then run in
// order, and the strip rows they read stay in cache.
template <size_t In>
void CopyPlaced(const BYTE* strip, UINT width, UINT y0, UINT y1, const Placement& place, bool sideways, BYTE* bits) {
    if (!sideways) {
        ForEachPixel<3>(width, y0, y1, place, false, [&](size_t i, ptrdiff_t at) {
            const BYTE* p = strip + i * In;
            BYTE* d = bits + at;
            d[0] = p[0];
            d[1] = p[1];
            d[2] = p[2];
            return true;
        });
        return;
    }
    const size_t rowBytes = size_t(width) * In;
    for (UINT x = 0; x < width; ++x) {
        const BYTE* p = strip + size_t(x) * In;
        BYTE* d = bits + place.origin + ptrdiff_t(x) * place.stepX + ptrdiff_t(y0) * place.stepY;
        for (UINT y = y0; y < y1; ++y, p += rowBytes, d += place.stepY) {
            d[0] = p[0];
            d[1] = p[1];
            d[2] = p[2];
        }
    }
}

// Whether converting the format to 8-bit BGRA keeps every pixel's value.
bool Is8BitColor(const WICPixelFormatGUID& format) {
    for (const GUID& f : {GUID_WICPixelFormat24bppBGR, GUID_WICPixelFormat32bppBGR, GUID_WICPixelFormat32bppBGRA,
                          GUID_WICPixelFormat8bppIndexed, GUID_WICPixelFormat4bppIndexed,
                          GUID_WICPixelFormat2bppIndexed, GUID_WICPixelFormat1bppIndexed, GUID_WICPixelFormat8bppGray,
                          GUID_WICPixelFormat4bppGray, GUID_WICPixelFormat2bppGray, GUID_WICPixelFormatBlackWhite}) {
        if (format == f) return true;
    }
    return false;
}

// The formats this viewer opens, recognized by their first bytes rather than the file name.
const GUID* SniffFormat(const BYTE* head, ULONG size) {
    const auto at = [&](ULONG offset, const char* magic, ULONG length) {
        return size >= offset + length && memcmp(head + offset, magic, length) == 0;
    };
    if (at(0, "\xFF\xD8\xFF", 3)) return &GUID_ContainerFormatJpeg;
    if (at(0, "\x89PNG\r\n\x1A\n", 8)) return &GUID_ContainerFormatPng;
    if (at(0, "GIF87a", 6) || at(0, "GIF89a", 6)) return &GUID_ContainerFormatGif;
    if (at(0, "BM", 2)) return &GUID_ContainerFormatBmp;
    if (at(0, "II*\0", 4) || at(0, "MM\0*", 4)) return &GUID_ContainerFormatTiff;
    if (at(0, "II\xBC", 3)) return &GUID_ContainerFormatWmp;  // JPEG XR
    if (at(0, "\0\0\1\0", 4)) return &GUID_ContainerFormatIco;
    if (at(0, "RIFF", 4) && at(8, "WEBP", 4)) return &GUID_ContainerFormatWebp;
    if (at(4, "ftyp", 4)) {  // HEIC and AVIF, both read by the HEIF decoder
        for (const char* brand : {"heic", "heix", "hevc", "hevx", "heim", "heis", "mif1", "msf1", "avif", "avis"}) {
            if (at(8, brand, 4)) return &GUID_ContainerFormatHeif;
        }
    }
    return nullptr;
}

// Opens a path only if it names a regular file, not a device or pipe such as \\.\CON, and reads that same
// file through the handle checked. A pipe server can only identify the user, never act as them. Files
// another program has open for writing can still be viewed.
HRESULT OpenFileStream(const std::wstring& path, ComPtr<IStream>& out) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING,
                                    FILE_FLAG_SEQUENTIAL_SCAN | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    if (file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    const HRESULT hr =
        GetFileType(file) == FILE_TYPE_DISK ? CreateHandleStream(file, out) : WINCODEC_ERR_UNKNOWNIMAGEFORMAT;
    if (FAILED(hr)) CloseHandle(file);
    return hr;
}

// Sets format to the container the stream's data starts with and rewinds it.
HRESULT SniffStream(IStream* stream, const GUID*& format) {
    BYTE head[16];
    ULONG read = 0;
    HRESULT hr = stream->Seek({}, STREAM_SEEK_SET, nullptr);
    if (SUCCEEDED(hr)) hr = stream->Read(head, sizeof(head), &read);
    if (SUCCEEDED(hr)) hr = stream->Seek({}, STREAM_SEEK_SET, nullptr);
    if (FAILED(hr)) return hr;
    format = SniffFormat(head, read);
    return format ? S_OK : WINCODEC_ERR_UNKNOWNIMAGEFORMAT;
}

// The decoder Windows has built in for a container, or null for those installed separately (HEIF, WebP).
const CLSID* BuiltInDecoder(const GUID& container) {
    static const struct {
        const GUID& container;
        const CLSID& decoder;
    } kDecoders[] = {
        {GUID_ContainerFormatJpeg, CLSID_WICJpegDecoder}, {GUID_ContainerFormatPng, CLSID_WICPngDecoder},
        {GUID_ContainerFormatGif, CLSID_WICGifDecoder},   {GUID_ContainerFormatBmp, CLSID_WICBmpDecoder},
        {GUID_ContainerFormatTiff, CLSID_WICTiffDecoder}, {GUID_ContainerFormatIco, CLSID_WICIcoDecoder},
        {GUID_ContainerFormatWmp, CLSID_WICWmpDecoder},
    };
    for (const auto& d : kDecoders) {
        if (d.container == container) return &d.decoder;
    }
    return nullptr;
}

// Makes a built-in decoder straight from windowscodecs.dll, which the factory has loaded. Asking the factory
// instead would first look through every installed codec, which takes longer than opening a small image.
HRESULT CreateBuiltInDecoder(const CLSID& clsid, ComPtr<IWICBitmapDecoder>& out) {
    const HMODULE module = GetModuleHandleW(L"windowscodecs.dll");
    const auto getClassObject =
        module ? reinterpret_cast<decltype(&DllGetClassObject)>(GetProcAddress(module, "DllGetClassObject")) : nullptr;
    ComPtr<IClassFactory> factory;
    HRESULT hr = getClassObject ? getClassObject(clsid, IID_PPV_ARGS(&factory)) : E_NOINTERFACE;
    if (SUCCEEDED(hr)) hr = factory->CreateInstance(nullptr, IID_PPV_ARGS(&out));
    return hr;
}

// Creates the decoder for the format the data starts with, and only that one: data is never handed
// to a codec for a different format than it claims to be, and anything else is rejected.
HRESULT CreateDecoder(IWICImagingFactory* wic, IStream* stream, ComPtr<IWICBitmapDecoder>& out) {
    const GUID* format = nullptr;
    HRESULT hr = SniffStream(stream, format);
    if (FAILED(hr)) return hr;

    ComPtr<IWICBitmapDecoder> decoder;
    const CLSID* builtIn = BuiltInDecoder(*format);
    hr = builtIn ? CreateBuiltInDecoder(*builtIn, decoder) : E_NOINTERFACE;
    if (FAILED(hr)) hr = wic->CreateDecoder(*format, &GUID_VendorMicrosoft, &decoder);
    if (SUCCEEDED(hr)) hr = decoder->Initialize(stream, WICDecodeMetadataCacheOnDemand);
    if (SUCCEEDED(hr)) out = decoder;
    return hr;
}

// Opens src's data with the decoder for its format; bytes, if given, is set to the data's size.
HRESULT OpenDecoder(IWICImagingFactory* wic, const Source& src, ComPtr<IWICBitmapDecoder>& decoder,
                    UINT64* bytes = nullptr) {
    ComPtr<IStream> stream = src.stream;
    HRESULT hr = stream ? S_OK : OpenFileStream(src.path, stream);
    STATSTG stat{};
    if (SUCCEEDED(hr) && bytes) hr = stream->Stat(&stat, STATFLAG_NONAME);
    if (SUCCEEDED(hr) && bytes) *bytes = stat.cbSize.QuadPart;
    if (SUCCEEDED(hr)) hr = CreateDecoder(wic, stream.Get(), decoder);
    return hr;
}

HRESULT CheckGif(IWICBitmapDecoder* decoder) {
    GUID container;
    HRESULT hr = decoder->GetContainerFormat(&container);
    return SUCCEEDED(hr) && container != GUID_ContainerFormatGif ? WINCODEC_ERR_UNSUPPORTEDOPERATION : hr;
}

// Shows the first frame of an animated GIF on a new 24bpp canvas. Fails for anything else.
HRESULT DecodeFirstFrame(IWICImagingFactory* wic, IWICBitmapDecoder* decoder, UINT64 bytes, Image& out,
                         const std::atomic<bool>* stop) {
    Animation a;
    HRESULT hr = CheckGif(decoder);
    if (SUCCEEDED(hr)) hr = ReadAnimation(wic, decoder, bytes, a, true, stop);
    if (FAILED(hr)) return hr;
    const HBITMAP bitmap = CreateDib(a.width, a.height, 24, nullptr, 0, a.bits);
    if (!bitmap) return E_OUTOFMEMORY;
    a.stride = (a.width * 3 + 3) & ~3u;
    ShowFrame(a, 0);
    out.bitmap = bitmap;
    out.width = out.fullWidth = a.width;
    out.height = out.fullHeight = a.height;
    out.exact = false;  // the canvas changes as it plays
    return S_OK;
}

bool ToSrgb(IWICImagingFactory* wic, IWICBitmapFrameDecode* frame, ComPtr<IWICBitmapSource>& source);

constexpr UINT64 kMinBandPixels = 1 << 20;  // smaller JPEGs decode sooner than extra threads start
constexpr UINT kMaxBands = 4;  // later bands spend most of their time reading past the rows above
// Converting colors, or reading TIFF strips, costs every band the same for its own rows, so more bands pay:
// measured best on 16 cores.
constexpr UINT kMaxEvenBands = 8;

// Whether a JPEG frame's scans each hold every component, so a decoder can produce a band of rows after reading
// past the data above it. A progressive JPEG has to be decoded whole for any band.
bool IsInterleavedJpeg(IWICBitmapSource* frame) {
    ComPtr<IWICJpegFrameDecode> jpeg;
    WICJpegFrameHeader header{};
    return SUCCEEDED(frame->QueryInterface(IID_PPV_ARGS(&jpeg))) && SUCCEEDED(jpeg->GetFrameHeader(&header)) &&
           header.ScanType == WICJpegScanTypeInterleaved;
}

// Rows [y0, y1) of an opaque 8-bit color JPEG, decoded through a decoder of its own into the 24bpp bitmap, as
// DecodeImage does: straight in when upright, else through a strip placed as place says.
struct Band {
    IWICImagingFactory* wic;
    const Source* src;
    UINT width, y0, y1;
    const Placement* place;
    bool sideways, direct, toSrgb;
    bool strips;  // TIFF: a band reads only its own strips
    BYTE* bits;
    UINT stride24;
    const std::atomic<bool>* stop;
    HRESULT hr = E_FAIL;
};

HRESULT DecodeBand(const Band& b) {
    Source own{b.src->path};  // a stream is cloned, so each band reads at its own position
    HRESULT hr = b.src->stream ? b.src->stream->Clone(&own.stream) : S_OK;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    ComPtr<IWICBitmapSource> source;
    if (SUCCEEDED(hr)) hr = OpenDecoder(b.wic, own, decoder);
    if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(hr)) source = frame;
    if (SUCCEEDED(hr) && b.toSrgb && !ToSrgb(b.wic, frame.Get(), source)) hr = E_FAIL;  // as the first decoder did
    if (SUCCEEDED(hr)) hr = b.wic->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr)) {
        hr = converter->Initialize(source.Get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom);
    }
    const UINT stripStride = b.width * 3;
    const UINT rows = std::clamp((1u << 20) / stripStride, 1u, std::max(1u, b.y1 - b.y0));
    const Pages<BYTE> strip = b.direct ? nullptr : AllocPages<BYTE>(size_t(stripStride) * rows);
    if (SUCCEEDED(hr) && !b.direct && !strip) hr = E_OUTOFMEMORY;
    for (UINT y0 = b.y0; y0 < b.y1 && SUCCEEDED(hr); y0 += rows) {
        if (b.stop && b.stop->load()) return E_ABORT;
        const UINT n = std::min(rows, b.y1 - y0);
        const WICRect rect{0, INT(y0), INT(b.width), INT(n)};
        if (b.direct) {
            hr = converter->CopyPixels(&rect, b.stride24, b.stride24 * n, b.bits + size_t(y0) * b.stride24);
            continue;
        }
        hr = converter->CopyPixels(&rect, stripStride, stripStride * n, strip.get());
        if (SUCCEEDED(hr)) CopyPlaced<3>(strip.get(), b.width, y0, y0 + n, *b.place, b.sideways, b.bits);
    }
    return hr;
}

// Decodes a large JPEG or TIFF in bands on several cores, this thread taking the first. Each band of a JPEG reads
// past the data of the rows above it, at a bit under half the cost of decoding them, so each band is about 0.57
// times as tall as the one above for all to finish together; 0.9 when converting colors, which costs the same in
// every band. A TIFF band reads only its own strips, so those are all as tall.
HRESULT DecodeBands(Band base, UINT height) {
    const UINT count = std::clamp(UINT(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)), 1u,
                                  base.toSrgb || base.strips ? kMaxEvenBands : kMaxBands);
    const double shrink = base.strips ? 1.0 : base.toSrgb ? 0.9 : 0.57;
    double weights[kMaxEvenBands], total = 0;
    for (UINT k = 0; k < count; ++k) total += weights[k] = k ? weights[k - 1] * shrink : 1.0;
    Band bands[kMaxEvenBands];
    HANDLE threads[kMaxEvenBands] = {};
    double above = 0;
    for (UINT k = 0; k < count; ++k) {
        bands[k] = base;
        bands[k].y0 = k ? bands[k - 1].y1 : 0;
        above += weights[k];
        // Edges fall on 16-row boundaries, the tallest a JPEG's blocks of rows can be.
        bands[k].y1 = k + 1 == count ? height : std::max(bands[k].y0, std::min(height, UINT(height * above / total) & ~15u));
        if (k == 0 || bands[k].y0 == bands[k].y1) continue;
        threads[k] = CreateThread(nullptr, 0, [](void* p) -> DWORD {
            Band& b = *static_cast<Band*>(p);
            // Without COM, as WIC's JPEG and TIFF decoders never need it, unless this one does.
            if ((b.hr = DecodeBand(b)) == CO_E_NOTINITIALIZED &&
                SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
                b.hr = DecodeBand(b);
                CoUninitialize();
            }
            return 0;
        }, &bands[k], 0, nullptr);
    }
    HRESULT hr = bands[0].hr = DecodeBand(bands[0]);
    for (UINT k = 1; k < count; ++k) {
        if (bands[k].y0 == bands[k].y1) continue;
        if (threads[k]) {
            WaitForSingleObject(threads[k], INFINITE);
            CloseHandle(threads[k]);
        } else {
            bands[k].hr = DecodeBand(bands[k]);  // no thread: decode it here
        }
        if (SUCCEEDED(hr)) hr = bands[k].hr;
    }
    return hr;
}

}  // namespace

WicFactory::WicFactory() {
    module_ = LoadLibraryExW(L"windowscodecs.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto getClassObject =
        module_ ? reinterpret_cast<decltype(&DllGetClassObject)>(GetProcAddress(module_, "DllGetClassObject")) : nullptr;
    if (!getClassObject) {
        hr = HRESULT_FROM_WIN32(GetLastError());
        return;
    }
    ComPtr<IClassFactory> classFactory;
    hr = getClassObject(CLSID_WICImagingFactory, IID_PPV_ARGS(&classFactory));
    if (SUCCEEDED(hr)) hr = classFactory->CreateInstance(nullptr, IID_PPV_ARGS(&factory_));
}

WicFactory::~WicFactory() {
    factory_.Reset();
    if (module_) FreeLibrary(module_);
}

namespace {

// All of a frame's color contexts: ICC profiles and EXIF color spaces.
std::vector<ComPtr<IWICColorContext>> ColorContexts(IWICImagingFactory* wic, IWICBitmapFrameDecode* frame) {
    UINT count = 0;
    if (FAILED(frame->GetColorContexts(0, nullptr, &count)) || count == 0 || count > 16) return {};
    std::vector<ComPtr<IWICColorContext>> contexts(count);
    std::vector<IWICColorContext*> raw(count);
    for (UINT i = 0; i < count; ++i) {
        if (FAILED(wic->CreateColorContext(&contexts[i]))) return {};
        raw[i] = contexts[i].Get();
    }
    if (FAILED(frame->GetColorContexts(count, raw.data(), &count))) return {};
    contexts.resize(count);
    return contexts;
}

bool IsProfile(IWICColorContext* context) {
    WICColorContextType type;
    return SUCCEEDED(context->GetType(&type)) && type == WICColorContextProfile;
}

// Whether converting from a profile to sRGB leaves 8-bit colors as they are, as it does for sRGB profiles. Tried on
// every level of each channel and of gray, and a lattice of mixed colors; remembered for the next image.
bool KeepsColors(IWICImagingFactory* wic, IWICColorContext* profile) {
    UINT size = 0;
    std::vector<BYTE> bytes;
    if (FAILED(profile->GetProfileBytes(0, nullptr, &size)) || size == 0) return false;
    bytes.resize(size);
    if (FAILED(profile->GetProfileBytes(size, bytes.data(), &size))) return false;
    UINT64 key = 1469598103934665603ull;  // FNV-1a
    for (const BYTE b : bytes) key = (key ^ b) * 1099511628211ull;
    // The IEC 61966-2.1 profile Windows ships, which most cameras and editors embed: known without loading the
    // color engine to try it.
    if (key == 0x5614E5C35A6D4122ull && size == 3144) return true;

    static SRWLOCK lock = SRWLOCK_INIT;
    static struct {
        UINT64 key;
        bool keeps;
    } known[8];
    static UINT knownCount = 0;
    AcquireSRWLockShared(&lock);
    const auto found = std::find_if(known, known + std::min(knownCount, 8u), [&](const auto& k) { return k.key == key; });
    const bool cached = found != known + std::min(knownCount, 8u);
    const bool cachedKeeps = cached && found->keeps;
    ReleaseSRWLockShared(&lock);
    if (cached) return cachedKeeps;

    std::vector<BYTE> probe;
    probe.reserve((256 * 4 + 16 * 16 * 16) * 3);
    for (UINT v = 0; v < 256; ++v) {
        const BYTE b = BYTE(v);
        for (const BYTE p : {b, BYTE(0), BYTE(0), BYTE(0), b, BYTE(0), BYTE(0), BYTE(0), b, b, b, b}) probe.push_back(p);
    }
    for (UINT r = 0; r < 256; r += 17) {
        for (UINT g = 0; g < 256; g += 17) {
            for (UINT b = 0; b < 256; b += 17) {
                for (const UINT p : {b, g, r}) probe.push_back(BYTE(p));
            }
        }
    }
    const UINT n = UINT(probe.size() / 3);
    std::vector<BYTE> out(probe.size());
    ComPtr<IWICBitmap> row;
    ComPtr<IWICColorContext> srgb;
    ComPtr<IWICColorTransform> transform;
    HRESULT hr = wic->CreateBitmapFromMemory(n, 1, GUID_WICPixelFormat24bppBGR, n * 3, n * 3, probe.data(), &row);
    if (SUCCEEDED(hr)) hr = wic->CreateColorContext(&srgb);
    if (SUCCEEDED(hr)) hr = srgb->InitializeFromExifColorSpace(1);
    if (SUCCEEDED(hr)) hr = wic->CreateColorTransformer(&transform);
    if (SUCCEEDED(hr)) hr = transform->Initialize(row.Get(), profile, srgb.Get(), GUID_WICPixelFormat24bppBGR);
    if (SUCCEEDED(hr)) hr = transform->CopyPixels(nullptr, n * 3, n * 3, out.data());
    const bool keeps = SUCCEEDED(hr) && out == probe;  // a CMYK or gray profile can't convert RGB at all

    AcquireSRWLockExclusive(&lock);
    known[knownCount++ % 8] = {key, keeps};
    ReleaseSRWLockExclusive(&lock);
    return keeps;
}

// Converts source, which shows frame, from the color space frame's colors are in to sRGB, the one Windows shows
// a program's pixels in, unless they are in sRGB already. Returns whether it did; a profile Windows can't use
// leaves source as it is.
bool ToSrgb(IWICImagingFactory* wic, IWICBitmapFrameDecode* frame, ComPtr<IWICBitmapSource>& source) {
    ComPtr<IWICColorContext> from;
    for (const auto& context : ColorContexts(wic, frame)) {
        UINT space = 0;
        if (IsProfile(context.Get())) {
            from = context;
            break;
        }
        if (!from && SUCCEEDED(context->GetExifColorSpace(&space)) && space == 2) from = context;  // Adobe RGB
    }
    ComPtr<IWICColorContext> srgb;
    ComPtr<IWICColorTransform> transform;
    if (!from || (IsProfile(from.Get()) && KeepsColors(wic, from.Get())) || FAILED(wic->CreateColorContext(&srgb)) ||
        FAILED(srgb->InitializeFromExifColorSpace(1))) {
        return false;
    }
    // 24bpp unless there is alpha to keep: converting to 32bpp changes colors even from sRGB to sRGB.
    const WICPixelFormatGUID& format =
        CanBeTransparent(wic, source.Get()) ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat24bppBGR;
    if (FAILED(wic->CreateColorTransformer(&transform)) ||
        FAILED(transform->Initialize(source.Get(), from.Get(), srgb.Get(), format))) {
        return false;
    }
    source = transform;
    return true;
}

// Puts a GIF's frame 0, which can be smaller than the image and anywhere on it, where it belongs on a transparent
// canvas, as browsers show it. A canvas too large to make, or larger than a file of bytes could fill, shows the
// frame alone.
HRESULT PlaceGifFrame(IWICImagingFactory* wic, IWICBitmapDecoder* decoder, IWICBitmapFrameDecode* frame, UINT64 bytes,
                      ComPtr<IWICBitmapSource>& source, UINT& width, UINT& height) {
    UINT w = 0, h = 0;
    HRESULT hr = GifCanvas(decoder, frame, w, h);
    if (FAILED(hr) || (w == width && h == height) || w > kMaxBitmapSize || h > kMaxBitmapSize ||
        UINT64(w) * h > GifPixelLimit(bytes)) {
        return hr;
    }
    const POINT at = GifFrameOffset(frame);
    ComPtr<IWICBitmap> canvas;
    ComPtr<IWICFormatConverter> bgra;
    ComPtr<IWICBitmapLock> lock;
    UINT stride = 0, size = 0;
    BYTE* data = nullptr;
    hr = wic->CreateBitmap(w, h, GUID_WICPixelFormat32bppBGRA, WICBitmapCacheOnLoad, &canvas);
    if (SUCCEEDED(hr)) hr = wic->CreateFormatConverter(&bgra);
    if (SUCCEEDED(hr)) {
        hr = bgra->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                              WICBitmapPaletteTypeCustom);
    }
    if (SUCCEEDED(hr)) hr = canvas->Lock(nullptr, WICBitmapLockWrite, &lock);
    if (SUCCEEDED(hr)) hr = lock->GetStride(&stride);
    if (SUCCEEDED(hr)) hr = lock->GetDataPointer(&size, &data);
    if (FAILED(hr)) return hr;
    memset(data, 0, size);
    const size_t offset = size_t(at.y) * stride + size_t(at.x) * 4;  // the canvas fits the frame there
    hr = bgra->CopyPixels(nullptr, stride, UINT(size - offset), data + offset);
    if (FAILED(hr)) return hr;
    source = canvas;
    width = w;
    height = h;
    return S_OK;
}

// The most pixels a file of bytes in container can really hold, as its compression packs no more. WIC fills in what
// is missing, so a small file claiming a huge size would otherwise take seconds and up to a gigabyte to show; files
// cut short, as by a broken download, stay far below. Formats without such a bound get the general limit.
UINT64 MaxPixels(IWICImagingFactory* wic, IWICBitmapFrameDecode* frame, const GUID& container, UINT64 bytes) {
    if (container == GUID_ContainerFormatGif) return GifPixelLimit(bytes);
    // A JPEG spends at least a bit or so on every 8 x 8 block, even progressive: under 512 pixels a byte.
    if (container == GUID_ContainerFormatJpeg) return bytes * 1024;
    // Deflate packs at most 1032 bytes into one; twice that, for the row filter bytes and to spare.
    if (container == GUID_ContainerFormatPng) return bytes * 2064 * 8 / std::max(1u, BitsPerPixel(wic, frame));
    // A run of up to 255 pixels takes 2 bytes; the rest is spare for rows that end early. An RLE BMP's end code
    // would otherwise leave WIC filling in, and holding, the whole bitmap: 1.8 GB for a 1 KB file.
    if (container == GUID_ContainerFormatBmp) return bytes * 1024;
    // LZW, the loosest of TIFF's compressions but fax, emits at most 4096 bytes a code of 9 bits or more. Fax codes
    // pack a blank row in a bit, so those get the general limit.
    if (container == GUID_ContainerFormatTiff) {
        ComPtr<IWICMetadataQueryReader> query;
        PROPVARIANT compression{};
        const bool fax = SUCCEEDED(frame->GetMetadataQueryReader(&query)) &&
                         SUCCEEDED(query->GetMetadataByName(L"/ifd/{ushort=259}", &compression)) &&
                         compression.vt == VT_UI2 && compression.uiVal >= 2 && compression.uiVal <= 4;
        PropVariantClear(&compression);
        if (!fax) return bytes * 4096 * 8 / std::max(1u, BitsPerPixel(wic, frame));
    }
    return kMaxSourcePixels;
}

// What OpenDecoded found out besides the pixels.
struct Opened {
    UINT64 bytes = 0;  // of the encoded data, set by OpenDecoder
    ComPtr<IWICBitmapFrameDecode> frame;
    SIZE full{};  // the stored size, before scaling
    int orientation = 1;  // the EXIF orientation, if not applied
    std::vector<ComPtr<IWICColorContext>> profiles;  // ICC profiles, if asked for
    bool converted = false;  // colors were converted to sRGB
};

// OpenSource for a source that isn't a bitmap, once its decoder is made; the orientation is applied if rotate.
HRESULT OpenDecoded(IWICImagingFactory* wic, IWICBitmapDecoder* decoder, UINT maxSize, Colors colors, bool rotate,
                    bool wantProfiles, ComPtr<IWICBitmapSource>& source, UINT& width, UINT& height, Opened& info) {
    ComPtr<IWICBitmapFrameDecode>& frame = info.frame;
    HRESULT hr = GetFrame(wic, decoder, frame);
    if (SUCCEEDED(hr)) hr = frame->GetSize(&width, &height);
    if (FAILED(hr)) return hr;
    if (width == 0 || height == 0) return WINCODEC_ERR_BADIMAGE;
    if (UINT64(width) * height > kMaxSourcePixels) return WINCODEC_ERR_IMAGESIZEOUTOFRANGE;
    GUID container{};
    decoder->GetContainerFormat(&container);
    source = frame;
    if (UINT64(width) * height > MaxPixels(wic, frame.Get(), container, info.bytes)) {
        return WINCODEC_ERR_IMAGESIZEOUTOFRANGE;
    }
    if (container == GUID_ContainerFormatGif) {
        hr = PlaceGifFrame(wic, decoder, frame.Get(), info.bytes, source, width, height);
        if (FAILED(hr)) return hr;
    }
    info.full = {LONG(width), LONG(height)};
    if (wantProfiles) {
        for (const auto& context : ColorContexts(wic, frame.Get())) {
            if (IsProfile(context.Get())) info.profiles.push_back(context);
        }
    }

    if (maxSize && (width > maxSize || height > maxSize)) {
        // Scaling at the decoder lets codecs like JPEG decode at reduced size directly.
        const double factor = double(maxSize) / std::max(width, height);
        width = std::max(1u, UINT(width * factor));
        height = std::max(1u, UINT(height * factor));
        ComPtr<IWICBitmapScaler> scaler;
        hr = wic->CreateBitmapScaler(&scaler);
        if (SUCCEEDED(hr)) hr = scaler->Initialize(source.Get(), width, height, WICBitmapInterpolationModeFant);
        if (FAILED(hr)) return hr;
        source = scaler;
    }
    if (colors == Colors::kSrgb) info.converted = ToSrgb(wic, frame.Get(), source);

    // EXIF orientation -> WIC transform (WIC flips first, then rotates clockwise).
    static constexpr WICBitmapTransformOptions kTransforms[] = {
        WICBitmapTransformRotate0, WICBitmapTransformRotate0, WICBitmapTransformFlipHorizontal,
        WICBitmapTransformRotate180, WICBitmapTransformFlipVertical,
        WICBitmapTransformOptions(WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal),
        WICBitmapTransformRotate90,
        WICBitmapTransformOptions(WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal),
        WICBitmapTransformRotate270,
    };
    const int exif = ReadOrientation(frame.Get(), container);
    if (!rotate) {
        info.orientation = exif;
    } else if (exif != 1) {
        // The rotator reads its source pixel by pixel, so buffer it to keep strip reads below linear.
        ComPtr<IWICBitmap> buffered;
        ComPtr<IWICBitmapFlipRotator> rotator;
        hr = wic->CreateBitmapFromSource(source.Get(), WICBitmapCacheOnLoad, &buffered);
        if (SUCCEEDED(hr)) hr = wic->CreateBitmapFlipRotator(&rotator);
        if (SUCCEEDED(hr)) hr = rotator->Initialize(buffered.Get(), kTransforms[exif]);
        if (FAILED(hr)) return hr;
        if (exif >= 5) std::swap(width, height);
        source = rotator;
    }
    return S_OK;
}

}  // namespace

HRESULT OpenEncoded(const Source& src, ComPtr<IStream>& stream, GUID& format) {
    if (!src.stream && src.path.empty()) return E_INVALIDARG;
    stream = src.stream;
    HRESULT hr = stream ? S_OK : OpenFileStream(src.path, stream);
    const GUID* sniffed = nullptr;
    if (SUCCEEDED(hr)) hr = SniffStream(stream.Get(), sniffed);
    if (SUCCEEDED(hr)) format = *sniffed;
    return hr;
}

HRESULT OpenSource(IWICImagingFactory* wic, const Source& src, UINT maxSize, Colors colors,
                   ComPtr<IWICBitmapSource>& source, UINT& width, UINT& height, SIZE* full, int* orientation,
                   std::vector<ComPtr<IWICColorContext>>* profiles) {
    if (orientation) *orientation = 1;
    if (src.bitmap) {  // holds the upright pixels as shown, which have no profile
        ComPtr<IWICBitmap> bitmap;
        HRESULT hr = wic->CreateBitmapFromHBITMAP(src.bitmap, nullptr, WICBitmapIgnoreAlpha, &bitmap);
        if (SUCCEEDED(hr)) hr = bitmap->GetSize(&width, &height);
        if (SUCCEEDED(hr)) source = bitmap;
        if (full) *full = {LONG(width), LONG(height)};
        return hr;
    }
    ComPtr<IWICBitmapDecoder> decoder;
    Opened info;
    HRESULT hr = OpenDecoder(wic, src, decoder, &info.bytes);
    if (SUCCEEDED(hr)) {
        hr = OpenDecoded(wic, decoder.Get(), maxSize, colors, !orientation, profiles != nullptr, source, width, height,
                         info);
    }
    if (FAILED(hr)) return hr;
    if (full) *full = info.full;
    if (orientation) *orientation = info.orientation;
    if (profiles) *profiles = std::move(info.profiles);
    return S_OK;
}

HRESULT DecodeAnimation(const Source& src, Animation& out, const std::atomic<bool>& stop) {
    const WicFactory wic;
    HRESULT hr = wic.hr;
    // A pasted image's stream may be read elsewhere at the same time, so read a clone with its own position.
    Source own{src.path};
    if (SUCCEEDED(hr) && src.stream) hr = src.stream->Clone(&own.stream);
    ComPtr<IWICBitmapDecoder> decoder;
    UINT64 bytes = 0;
    if (SUCCEEDED(hr)) hr = OpenDecoder(wic.Get(), own, decoder, &bytes);
    if (SUCCEEDED(hr)) hr = CheckGif(decoder.Get());
    if (SUCCEEDED(hr)) hr = ReadAnimation(wic.Get(), decoder.Get(), bytes, out, false, &stop);
    return hr;
}

HRESULT DecodeImage(const Source& src, Image& out, bool* animated, const std::atomic<bool>* stop) {
    if (animated) *animated = false;
    const WicFactory wic;
    HRESULT hr = wic.hr;
    if (FAILED(hr)) return hr;
    ComPtr<IWICBitmapSource> source;
    UINT width = 0, height = 0;  // of the source, before orientation is applied
    Opened info;  // full is the size before scaling down
    GUID container{};
    if (src.bitmap) {
        hr = OpenSource(wic.Get(), src, kMaxBitmapSize, Colors::kStored, source, width, height, &info.full,
                        &info.orientation);
    } else {
        ComPtr<IWICBitmapDecoder> decoder;
        hr = OpenDecoder(wic.Get(), src, decoder, &info.bytes);
        if (SUCCEEDED(hr) && animated &&
            SUCCEEDED(DecodeFirstFrame(wic.Get(), decoder.Get(), info.bytes, out, stop))) {
            *animated = true;
            return S_OK;
        }
        if (SUCCEEDED(hr)) hr = decoder->GetContainerFormat(&container);
        if (SUCCEEDED(hr)) {
            hr = OpenDecoded(wic.Get(), decoder.Get(), kMaxBitmapSize, Colors::kSrgb, false, true, source, width,
                             height, info);
        }
    }
    if (FAILED(hr)) return hr;
    const SIZE full = info.full;
    const int orientation = info.orientation;
    WICPixelFormatGUID format{};
    source->GetPixelFormat(&format);
    const bool transparent = CanBeTransparent(wic.Get(), source.Get());
    // A color JPEG all but never has 256 colors or fewer, nor does a 24bpp TIFF, which is scanned or photographed
    // (drawings come as palette or 1-bit TIFFs), so they go straight to 24bpp without counting them.
    const bool tiff = container == GUID_ContainerFormatTiff;
    const bool countColors = !((container == GUID_ContainerFormatJpeg || tiff) && format == GUID_WICPixelFormat24bppBGR);
    const UINT bg = kBackground & 0xFF;  // gray: all channels equal
    // Composites transparent 32bpp pixels over the background once, so painting is a plain blit, and ANDs their
    // alpha into alpha.
    const auto composite = [bg](BYTE* p, const BYTE* end, BYTE& alpha) {
        for (; p != end; p += 4) {
            alpha &= p[3];
            const UINT inverse = 255u - p[3];
            if (inverse == 0) continue;
            const BYTE add = BYTE((bg * inverse + 127) / 255);
            p[0] = BYTE(p[0] + add);
            p[1] = BYTE(p[1] + add);
            p[2] = BYTE(p[2] + add);
        }
    };
    // An 8bpp gray or palette image has at most 256 colors, so it is read as stored, a byte per pixel, and each
    // value is converted and looked up in the palette once.
    BYTE values[256 * 4];  // each value as 32bpp, composited if transparent
    const bool byValue = (format == GUID_WICPixelFormat8bppGray || format == GUID_WICPixelFormat8bppIndexed) &&
                         SUCCEEDED(ConvertValues(wic.Get(), source.Get(), format, values));
    if (byValue && transparent) {
        BYTE unused = 255;
        composite(values, values + sizeof(values), unused);
    }

    ComPtr<IWICBitmapSource> reader = source;  // what the strips are read from
    if (!byValue) {
        ComPtr<IWICFormatConverter> converter;
        hr = wic->CreateFormatConverter(&converter);
        if (SUCCEEDED(hr)) {
            hr = converter->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr,
                                       0.0, WICBitmapPaletteTypeCustom);
        }
        if (FAILED(hr)) return hr;
        reader = converter;
    }

    // Decode in strips through a small buffer and store the exact pixels upright, in the smallest
    // form: palette indices while the image has <= 256 colors, otherwise 24bpp (alpha is composited away).
    const bool sideways = orientation >= 5;
    const UINT outW = sideways ? height : width, outH = sideways ? width : height;
    const UINT stripStride = width * (byValue ? 1 : 4);
    // As many rows as 1 MB of 32bpp holds, whatever the strip holds: a sideways image's palette is in the order
    // its colors first appear in each strip.
    const UINT rows = std::clamp((1u << 20) / (width * 4), 1u, height);
    const Pages<BYTE> strip = AllocPages<BYTE>(size_t(stripStride) * rows);
    Pages<BYTE> indices = countColors ? AllocPages<BYTE>(size_t(width) * height) : nullptr;
    if (!strip || (byValue && !indices)) return E_OUTOFMEMORY;  // a strip of values has no 24bpp to fall back on

    const UINT stride24 = (outW * 3 + 3) & ~3u;
    const Placement placeIndex = Place(orientation, width, height, 1, outW);
    const Placement place24 = Place(orientation, width, height, 3, stride24);
    Palette palette;
    int known[256];  // palette index of each value, -1 until seen
    std::fill_n(known, 256, -1);
    BYTE* bits = nullptr;
    HBITMAP bitmap = indices ? nullptr : CreateDib(outW, outH, 24, nullptr, 0, bits);
    if (!indices && !bitmap) return E_OUTOFMEMORY;

    // Once 24bpp, upright opaque 8-bit sources are decoded straight into the bitmap: same bytes, no copy.
    const bool canDecodeDirect = orientation == 1 && !transparent && Is8BitColor(format);
    // A large color JPEG or TIFF shown at full size decodes in bands on several cores instead of the loop below.
    const bool inBands = !countColors && !transparent && UINT(full.cx) == width && UINT(full.cy) == height &&
                         UINT64(width) * height >= kMinBandPixels && (tiff || IsInterleavedJpeg(info.frame.Get()));
    if (inBands) {
        hr = DecodeBands({wic.Get(), &src, width, 0, 0, &place24, sideways, canDecodeDirect, info.converted, tiff, bits,
                          stride24, stop},
                         height);
    }
    ComPtr<IWICFormatConverter> direct;
    const auto startDirect = [&] {
        if (canDecodeDirect && SUCCEEDED(wic->CreateFormatConverter(&direct)) &&
            FAILED(direct->Initialize(source.Get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr,
                                      0.0, WICBitmapPaletteTypeCustom))) {
            direct.Reset();
        }
    };
    if (!indices && !inBands) startDirect();
    BYTE alpha = 255;  // AND of all alpha values: 255 if nothing is transparent
    for (UINT y0 = inBands ? height : 0; y0 < height; y0 += rows) {
        if (stop && stop->load()) {
            hr = E_ABORT;
            break;
        }
        const UINT n = std::min(rows, height - y0);
        const WICRect rect{0, INT(y0), INT(width), INT(n)};
        if (direct) {
            hr = direct->CopyPixels(&rect, stride24, stride24 * n, bits + size_t(y0) * stride24);
            if (FAILED(hr)) break;
            continue;
        }
        hr = reader->CopyPixels(&rect, stripStride, stripStride * n, strip.get());
        if (FAILED(hr)) break;
        BYTE* const begin = strip.get();
        BYTE* const end = begin + size_t(stripStride) * n;

        if (byValue) {
            BYTE* const dst = indices.get();
            ForEachPixel<1>(width, y0, y0 + n, placeIndex, sideways, [&](size_t i, ptrdiff_t at) {
                const BYTE v = begin[i];
                if (known[v] < 0) {
                    UINT32 color;
                    memcpy(&color, values + v * 4, 4);
                    known[v] = palette.Find(color & 0xFFFFFF);  // never full: 256 values at most
                    if (transparent) alpha &= values[v * 4 + 3];
                }
                dst[at] = BYTE(known[v]);
                return true;
            });
            continue;
        }
        if (transparent) composite(begin, end, alpha);  // sources that can't be transparent skip this

        if (indices) {
            BYTE* const dst = indices.get();
            const size_t first = size_t(y0) * width;  // where an upright strip starts
            const auto store = [&](size_t i, ptrdiff_t at) {
                UINT32 color;
                memcpy(&color, begin + i * 4, 4);
                const int index = palette.Index(color & 0xFFFFFF);
                dst[at] = BYTE(index);
                return index >= 0;
            };
            if (orientation == 1 ? StoreInOrder(begin, size_t(width) * n, palette, dst + first)
                                 : ForEachPixel<1>(width, y0, y0 + n, placeIndex, sideways, store)) {
                continue;
            }

            // More than 256 colors: switch to 24bpp, expanding the rows stored so far.
            bitmap = CreateDib(outW, outH, 24, nullptr, 0, bits);
            if (!bitmap) {
                hr = E_OUTOFMEMORY;
                break;
            }
            const RECT done = PlacedRows(orientation, width, height, y0);
            for (LONG y = done.top; y < done.bottom; ++y) {
                const BYTE* in = indices.get() + size_t(y) * outW;
                BYTE* d = bits + size_t(y) * stride24 + size_t(done.left) * 3;
                for (LONG x = done.left; x < done.right; ++x, d += 3) {
                    const UINT32 color = palette.colors[in[x]];
                    d[0] = BYTE(color);
                    d[1] = BYTE(color >> 8);
                    d[2] = BYTE(color >> 16);
                }
            }
            indices.reset();
        }

        CopyPlaced<4>(begin, width, y0, y0 + n, place24, sideways, bits);
        startDirect();
    }
    if (FAILED(hr)) {
        if (bitmap) DeleteObject(bitmap);
        return hr;
    }

    if (indices) {
        const WORD bpp = palette.count <= 2 ? 1 : palette.count <= 16 ? 4 : 8;
        bitmap = CreateDib(outW, outH, bpp, palette.colors, palette.count, bits);
        if (!bitmap) return E_OUTOFMEMORY;
        const UINT stride = (outW * bpp + 31) / 32 * 4;
        for (UINT y = 0; y < outH; ++y) {
            const BYTE* in = indices.get() + size_t(y) * outW;
            BYTE* d = bits + size_t(y) * stride;
            if (bpp == 8) std::copy_n(in, outW, d);
            else if (bpp == 4) PackRow<4>(in, outW, d);
            else PackRow<1>(in, outW, d);
        }
    }

    out.bitmap = bitmap;
    out.width = outW;
    out.height = outH;
    out.fullWidth = UINT(sideways ? full.cy : full.cx);
    out.fullHeight = UINT(sideways ? full.cx : full.cy);
    // A bitmap of the pixels would lose the profile, and converted colors aren't the stored ones.
    out.exact = alpha == 255 && UINT(full.cx) == width && UINT(full.cy) == height && Is8BitColor(format) &&
                info.profiles.empty() && !info.converted;
    return S_OK;
}

bool IsImageName(const wchar_t* name) {
    const wchar_t* dot = wcsrchr(name, L'.');
    if (!dot) return false;
    for (const wchar_t* ext : kExtensions) {
        if (CompareStringOrdinal(dot, -1, ext, -1, TRUE) == CSTR_EQUAL) return true;
    }
    return false;
}
