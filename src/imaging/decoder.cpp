#include "imaging/decoder.h"

#include <algorithm>
#include <cstring>
#include <span>
#include <type_traits>

#include "config.h"
#include "imaging/animation.h"
#include "platform/handle_stream.h"
#include "platform/pages.h"

using Microsoft::WRL::ComPtr;

namespace {

// Reads the EXIF orientation tag, if any, from where the container keeps it. The first query costs about a
// millisecond, so containers WIC reads no orientation from (PNG, GIF, BMP) skip it, and JPEG and TIFF skip the
// path the other one uses.
int ReadOrientation(IWICBitmapFrameDecode* frame, const GUID& container) {
    static constexpr const wchar_t* kQueries[] = {L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}"};
    std::span<const wchar_t* const> queries = kQueries;
    if (container == GUID_ContainerFormatJpeg) {
        queries = queries.first(1);
    } else if (container == GUID_ContainerFormatTiff) {
        queries = queries.last(1);
    } else if (container == GUID_ContainerFormatPng || container == GUID_ContainerFormatGif ||
               container == GUID_ContainerFormatBmp) {
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
    WICPixelFormatGUID format;
    ComPtr<IWICComponentInfo> info;
    ComPtr<IWICPixelFormatInfo2> formatInfo;
    BOOL transparency = TRUE;
    if (FAILED(source->GetPixelFormat(&format)) || FAILED(wic->CreateComponentInfo(format, &info)) ||
        FAILED(info.As(&formatInfo)) || FAILED(formatInfo->SupportsTransparency(&transparency))) {
        return true;
    }
    if (!transparency) return false;
    // Indexed formats are transparent only if their palette is.
    ComPtr<IWICPalette> palette;
    BOOL alpha = TRUE;
    if (SUCCEEDED(wic->CreatePalette(&palette)) && SUCCEEDED(source->CopyPalette(palette.Get())) &&
        SUCCEEDED(palette->HasAlpha(&alpha))) {
        return alpha;
    }
    return true;
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

HRESULT OpenDecoder(IWICImagingFactory* wic, const Source& src, ComPtr<IWICBitmapDecoder>& decoder) {
    ComPtr<IStream> stream = src.stream;
    HRESULT hr = stream ? S_OK : OpenFileStream(src.path, stream);
    if (SUCCEEDED(hr)) hr = CreateDecoder(wic, stream.Get(), decoder);
    return hr;
}

HRESULT CheckGif(IWICBitmapDecoder* decoder) {
    GUID container;
    HRESULT hr = decoder->GetContainerFormat(&container);
    return SUCCEEDED(hr) && container != GUID_ContainerFormatGif ? WINCODEC_ERR_UNSUPPORTEDOPERATION : hr;
}

// Shows the first frame of an animated GIF on a new 24bpp canvas. Fails for anything else.
HRESULT DecodeFirstFrame(IWICImagingFactory* wic, IWICBitmapDecoder* decoder, Image& out,
                         const std::atomic<bool>* stop) {
    Animation a;
    HRESULT hr = CheckGif(decoder);
    if (SUCCEEDED(hr)) hr = ReadAnimation(wic, decoder, a, true, stop);
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

constexpr UINT64 kMinBandPixels = 1 << 20;  // smaller JPEGs decode sooner than extra threads start
constexpr UINT kMaxBands = 4;  // later bands spend most of their time reading past the rows above

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
    bool sideways, direct;
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
    if (SUCCEEDED(hr)) hr = OpenDecoder(b.wic, own, decoder);
    if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(hr)) hr = b.wic->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr)) {
        hr = converter->Initialize(frame.Get(), b.direct ? GUID_WICPixelFormat24bppBGR : GUID_WICPixelFormat32bppPBGRA,
                                   WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    }
    const UINT stripStride = b.width * 4;
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
        if (FAILED(hr)) break;
        ForEachPixel<3>(b.width, y0, y0 + n, *b.place, b.sideways, [&](size_t i, ptrdiff_t at) {
            const BYTE* p = strip.get() + i * 4;
            BYTE* d = b.bits + at;
            d[0] = p[0];
            d[1] = p[1];
            d[2] = p[2];
            return true;
        });
    }
    return hr;
}

// Decodes a large JPEG in bands on several cores, this thread taking the first. Each band reads past the data of
// the rows above it, at a bit under half the cost of decoding them, so each band is about 0.57 times as tall as
// the one above for all to finish together.
HRESULT DecodeBands(Band base, UINT height) {
    const UINT count = std::clamp(UINT(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)), 1u, kMaxBands);
    double weights[kMaxBands], total = 0;
    for (UINT k = 0; k < count; ++k) total += weights[k] = k ? weights[k - 1] * 0.57 : 1.0;
    Band bands[kMaxBands];
    HANDLE threads[kMaxBands] = {};
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
            const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            b.hr = DecodeBand(b);
            if (SUCCEEDED(com)) CoUninitialize();
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

// OpenSource for a source that isn't a bitmap, once its decoder is made.
HRESULT OpenDecoded(IWICImagingFactory* wic, IWICBitmapDecoder* decoder, UINT maxSize,
                    ComPtr<IWICBitmapSource>& source, UINT& width, UINT& height, SIZE* full, int* orientation) {
    if (orientation) *orientation = 1;
    ComPtr<IWICBitmapFrameDecode> frame;
    HRESULT hr = GetFrame(wic, decoder, frame);
    if (SUCCEEDED(hr)) hr = frame->GetSize(&width, &height);
    if (FAILED(hr)) return hr;
    if (width == 0 || height == 0) return WINCODEC_ERR_BADIMAGE;
    if (UINT64(width) * height > kMaxSourcePixels) return WINCODEC_ERR_IMAGESIZEOUTOFRANGE;
    if (full) *full = {LONG(width), LONG(height)};

    source = frame;
    if (maxSize && (width > maxSize || height > maxSize)) {
        // Scaling at the decoder lets codecs like JPEG decode at reduced size directly.
        const double factor = double(maxSize) / std::max(width, height);
        width = std::max(1u, UINT(width * factor));
        height = std::max(1u, UINT(height * factor));
        ComPtr<IWICBitmapScaler> scaler;
        hr = wic->CreateBitmapScaler(&scaler);
        if (SUCCEEDED(hr)) hr = scaler->Initialize(frame.Get(), width, height, WICBitmapInterpolationModeFant);
        if (FAILED(hr)) return hr;
        source = scaler;
    }

    // EXIF orientation -> WIC transform (WIC flips first, then rotates clockwise).
    static constexpr WICBitmapTransformOptions kTransforms[] = {
        WICBitmapTransformRotate0, WICBitmapTransformRotate0, WICBitmapTransformFlipHorizontal,
        WICBitmapTransformRotate180, WICBitmapTransformFlipVertical,
        WICBitmapTransformOptions(WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal),
        WICBitmapTransformRotate90,
        WICBitmapTransformOptions(WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal),
        WICBitmapTransformRotate270,
    };
    GUID container{};
    decoder->GetContainerFormat(&container);
    const int exif = ReadOrientation(frame.Get(), container);
    if (orientation) {
        *orientation = exif;
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

HRESULT OpenSource(IWICImagingFactory* wic, const Source& src, UINT maxSize, ComPtr<IWICBitmapSource>& source,
                   UINT& width, UINT& height, SIZE* full, int* orientation) {
    if (src.bitmap) {
        if (orientation) *orientation = 1;
        ComPtr<IWICBitmap> bitmap;
        HRESULT hr = wic->CreateBitmapFromHBITMAP(src.bitmap, nullptr, WICBitmapIgnoreAlpha, &bitmap);
        if (SUCCEEDED(hr)) hr = bitmap->GetSize(&width, &height);
        if (SUCCEEDED(hr)) source = bitmap;
        if (full) *full = {LONG(width), LONG(height)};
        return hr;
    }
    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = OpenDecoder(wic, src, decoder);
    if (SUCCEEDED(hr)) hr = OpenDecoded(wic, decoder.Get(), maxSize, source, width, height, full, orientation);
    return hr;
}

namespace {

// Adds the ICC profiles of a decoded frame to out.
void FrameProfiles(IWICImagingFactory* wic, IWICBitmapFrameDecode* frame, std::vector<ComPtr<IWICColorContext>>& out) {
    UINT count = 0;
    if (FAILED(frame->GetColorContexts(0, nullptr, &count)) || count == 0 || count > 16) return;
    std::vector<ComPtr<IWICColorContext>> contexts(count);
    std::vector<IWICColorContext*> raw(count);
    for (UINT i = 0; i < count; ++i) {
        if (FAILED(wic->CreateColorContext(&contexts[i]))) return;
        raw[i] = contexts[i].Get();
    }
    if (FAILED(frame->GetColorContexts(count, raw.data(), &count))) return;
    for (UINT i = 0; i < count; ++i) {
        WICColorContextType type;
        if (SUCCEEDED(contexts[i]->GetType(&type)) && type == WICColorContextProfile) out.push_back(contexts[i]);
    }
}

// Whether source is a decoded frame with an ICC profile, which a bitmap of its pixels would lose.
bool HasColorProfile(IWICImagingFactory* wic, IWICBitmapSource* source) {
    ComPtr<IWICBitmapFrameDecode> frame;
    std::vector<ComPtr<IWICColorContext>> profiles;
    if (SUCCEEDED(source->QueryInterface(IID_PPV_ARGS(&frame)))) FrameProfiles(wic, frame.Get(), profiles);
    return !profiles.empty();
}

}  // namespace

void ReadColorProfiles(IWICImagingFactory* wic, const Source& src, std::vector<ComPtr<IWICColorContext>>& out) {
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (!src.bitmap && SUCCEEDED(OpenDecoder(wic, src, decoder)) && SUCCEEDED(GetFrame(wic, decoder.Get(), frame))) {
        FrameProfiles(wic, frame.Get(), out);
    }
}

HRESULT DecodeAnimation(const Source& src, Animation& out, const std::atomic<bool>& stop) {
    const WicFactory wic;
    HRESULT hr = wic.hr;
    // A pasted image's stream may be read elsewhere at the same time, so read a clone with its own position.
    Source own{src.path};
    if (SUCCEEDED(hr) && src.stream) hr = src.stream->Clone(&own.stream);
    ComPtr<IWICBitmapDecoder> decoder;
    if (SUCCEEDED(hr)) hr = OpenDecoder(wic.Get(), own, decoder);
    if (SUCCEEDED(hr)) hr = CheckGif(decoder.Get());
    if (SUCCEEDED(hr)) hr = ReadAnimation(wic.Get(), decoder.Get(), out, false, &stop);
    return hr;
}

HRESULT DecodeImage(const Source& src, Image& out, bool* animated, const std::atomic<bool>* stop) {
    if (animated) *animated = false;
    const WicFactory wic;
    HRESULT hr = wic.hr;
    if (FAILED(hr)) return hr;
    ComPtr<IWICBitmapSource> source;
    UINT width = 0, height = 0;  // of the source, before orientation is applied
    SIZE full{};  // the same, before scaling down
    int orientation = 1;
    GUID container{};
    if (src.bitmap) {
        hr = OpenSource(wic.Get(), src, kMaxBitmapSize, source, width, height, &full, &orientation);
    } else {
        ComPtr<IWICBitmapDecoder> decoder;
        hr = OpenDecoder(wic.Get(), src, decoder);
        if (SUCCEEDED(hr) && animated && SUCCEEDED(DecodeFirstFrame(wic.Get(), decoder.Get(), out, stop))) {
            *animated = true;
            return S_OK;
        }
        if (SUCCEEDED(hr)) hr = decoder->GetContainerFormat(&container);
        if (SUCCEEDED(hr)) {
            hr = OpenDecoded(wic.Get(), decoder.Get(), kMaxBitmapSize, source, width, height, &full, &orientation);
        }
    }
    if (FAILED(hr)) return hr;
    WICPixelFormatGUID format{};
    source->GetPixelFormat(&format);
    const bool transparent = CanBeTransparent(wic.Get(), source.Get());
    // A color JPEG all but never has 256 colors or fewer, so it goes straight to 24bpp without counting them.
    const bool countColors = container != GUID_ContainerFormatJpeg || format != GUID_WICPixelFormat24bppBGR;

    ComPtr<IWICFormatConverter> converter;
    hr = wic->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr)) {
        hr = converter->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                   nullptr, 0.0, WICBitmapPaletteTypeCustom);
    }
    if (FAILED(hr)) return hr;

    // Decode in strips through a small 32bpp buffer and store the exact pixels upright, in the smallest
    // form: palette indices while the image has <= 256 colors, otherwise 24bpp (alpha is composited away).
    const bool sideways = orientation >= 5;
    const UINT outW = sideways ? height : width, outH = sideways ? width : height;
    const UINT stripStride = width * 4;
    const UINT rows = std::clamp((1u << 20) / stripStride, 1u, height);
    const Pages<BYTE> strip = AllocPages<BYTE>(size_t(stripStride) * rows);
    Pages<BYTE> indices = countColors ? AllocPages<BYTE>(size_t(width) * height) : nullptr;
    if (!strip) return E_OUTOFMEMORY;

    const UINT stride24 = (outW * 3 + 3) & ~3u;
    const Placement placeIndex = Place(orientation, width, height, 1, outW);
    const Placement place24 = Place(orientation, width, height, 3, stride24);
    Palette palette;
    UINT32 lastColor = ~0u;
    BYTE lastIndex = 0;
    BYTE* bits = nullptr;
    HBITMAP bitmap = indices ? nullptr : CreateDib(outW, outH, 24, nullptr, 0, bits);
    if (!indices && !bitmap) return E_OUTOFMEMORY;

    // Once 24bpp, upright opaque 8-bit sources are decoded straight into the bitmap: same bytes, no copy.
    const bool canDecodeDirect = orientation == 1 && !transparent && Is8BitColor(format);
    // A large color JPEG shown at full size decodes in bands on several cores instead of the loop below.
    const bool inBands = !countColors && !transparent && UINT(full.cx) == width && UINT(full.cy) == height &&
                         UINT64(width) * height >= kMinBandPixels && IsInterleavedJpeg(source.Get());
    if (inBands) {
        hr = DecodeBands({wic.Get(), &src, width, 0, 0, &place24, sideways, canDecodeDirect, bits, stride24, stop},
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
    const UINT bg = kBackground & 0xFF;  // gray: all channels equal
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
        hr = converter->CopyPixels(&rect, stripStride, stripStride * n, strip.get());
        if (FAILED(hr)) break;
        BYTE* const begin = strip.get();
        BYTE* const end = begin + size_t(stripStride) * n;

        // Composite transparent pixels over the background once, so painting is a plain blit; sources
        // that cannot be transparent skip this.
        for (BYTE* p = transparent ? begin : end; p != end; p += 4) {
            alpha &= p[3];
            const UINT inverse = 255u - p[3];
            if (inverse == 0) continue;
            const BYTE add = BYTE((bg * inverse + 127) / 255);
            p[0] = BYTE(p[0] + add);
            p[1] = BYTE(p[1] + add);
            p[2] = BYTE(p[2] + add);
        }

        if (indices) {
            BYTE* const dst = indices.get();
            const auto store = [&](size_t i, ptrdiff_t at) {
                UINT32 color;
                memcpy(&color, begin + i * 4, 4);
                color &= 0xFFFFFF;
                if (color != lastColor) {
                    const int index = palette.Find(color);
                    if (index < 0) return false;
                    lastColor = color;
                    lastIndex = BYTE(index);
                }
                dst[at] = lastIndex;
                return true;
            };
            if (ForEachPixel<1>(width, y0, y0 + n, placeIndex, sideways, store)) continue;

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

        ForEachPixel<3>(width, y0, y0 + n, place24, sideways, [&](size_t i, ptrdiff_t at) {
            const BYTE* p = begin + i * 4;
            BYTE* d = bits + at;
            d[0] = p[0];
            d[1] = p[1];
            d[2] = p[2];
            return true;
        });
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
    out.exact = alpha == 255 && UINT(full.cx) == width && UINT(full.cy) == height && Is8BitColor(format) &&
                !HasColorProfile(wic.Get(), source.Get());
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
