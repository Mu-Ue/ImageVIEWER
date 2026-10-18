#include "test.h"

#include <algorithm>
#include <cstring>
#include <unordered_set>

#include "config.h"

namespace {

std::wstring ScratchDir() {
    static const std::wstring dir = [] {
        wchar_t temp[MAX_PATH];
        GetTempPathW(MAX_PATH, temp);
        std::wstring d = std::wstring(temp) + L"ImageVIEWER-tests-" + std::to_wstring(GetCurrentProcessId()) + L"\\";
        CreateDirectoryW(d.c_str(), nullptr);
        return d;
    }();
    return dir;
}

constexpr WICBitmapTransformOptions kTransforms[] = {
    WICBitmapTransformRotate0, WICBitmapTransformRotate0, WICBitmapTransformFlipHorizontal,
    WICBitmapTransformRotate180, WICBitmapTransformFlipVertical,
    WICBitmapTransformOptions(WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal),
    WICBitmapTransformRotate90,
    WICBitmapTransformOptions(WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal),
    WICBitmapTransformRotate270,
};

}  // namespace

std::wstring TempPath(const std::wstring& name) {
    return ScratchDir() + name;
}

std::wstring TempDir(const std::wstring& name) {
    const std::wstring dir = ScratchDir() + name + L"\\";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

bool LongPathsEnabled() {
    const auto enabled = reinterpret_cast<BOOLEAN(NTAPI*)()>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlAreLongPathsEnabled"));
    return enabled && enabled();
}

std::vector<BYTE> ReadAll(const std::wstring& path) {
    std::vector<BYTE> data;
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return data;
    LARGE_INTEGER size;
    DWORD n = 0;
    if (GetFileSizeEx(file, &size)) {
        data.resize(size_t(size.QuadPart));
        ReadFile(file, data.data(), DWORD(data.size()), &n, nullptr);
    }
    CloseHandle(file);
    return data;
}

bool WriteAll(const std::wstring& path, const std::vector<BYTE>& data) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD n = 0;
    const bool ok = WriteFile(file, data.data(), DWORD(data.size()), &n, nullptr) && n == data.size();
    CloseHandle(file);
    return ok;
}

ComPtr<IStream> MemoryStream(const std::vector<BYTE>& data) {
    const HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, std::max<size_t>(1, data.size()));
    if (!memory) return nullptr;
    memcpy(GlobalLock(memory), data.data(), data.size());
    GlobalUnlock(memory);
    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(memory, TRUE, &stream))) {
        GlobalFree(memory);
        return nullptr;
    }
    stream->SetSize({.QuadPart = data.size()});
    return stream;
}

Pixels ReadSource(IWICBitmapSource* source, const GUID& format) {
    Pixels p;
    ComPtr<IWICFormatConverter> converter;
    if (!source || FAILED(source->GetSize(&p.width, &p.height)) || FAILED(Wic()->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(source, format, WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) {
        return {};
    }
    p.argb.resize(size_t(p.width) * p.height);
    if (FAILED(converter->CopyPixels(nullptr, p.width * 4, UINT(p.argb.size() * 4),
                                     reinterpret_cast<BYTE*>(p.argb.data())))) {
        return {};
    }
    return p;
}

std::vector<UINT16> ReadWide(IWICBitmapSource* source) {
    UINT w = 0, h = 0;
    ComPtr<IWICFormatConverter> converter;
    std::vector<UINT16> values;
    if (!source || FAILED(source->GetSize(&w, &h)) || FAILED(Wic()->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(source, GUID_WICPixelFormat64bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) {
        return values;
    }
    values.resize(size_t(w) * h * 4);
    if (FAILED(converter->CopyPixels(nullptr, w * 8, UINT(values.size() * 2), reinterpret_cast<BYTE*>(values.data())))) {
        values.clear();
    }
    return values;
}

ComPtr<IWICBitmapSource> Upright(IWICBitmapSource* source, int orientation) {
    if (orientation == 1) return source;
    ComPtr<IWICBitmap> buffered;
    ComPtr<IWICBitmapFlipRotator> rotator;
    if (FAILED(Wic()->CreateBitmapFromSource(source, WICBitmapCacheOnLoad, &buffered)) ||
        FAILED(Wic()->CreateBitmapFlipRotator(&rotator)) ||
        FAILED(rotator->Initialize(buffered.Get(), kTransforms[orientation]))) {
        return nullptr;
    }
    return rotator;
}

ComPtr<IWICBitmapFrameDecode> WicFrame(const std::wstring& path, UINT index) {
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (SUCCEEDED(Wic()->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                   &decoder))) {
        decoder->GetFrame(index, &frame);
    }
    return frame;
}

ComPtr<IWICBitmapFrameDecode> WicFrame(const std::vector<BYTE>& data, UINT index) {
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    const ComPtr<IStream> stream = MemoryStream(data);
    if (stream && SUCCEEDED(Wic()->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand,
                                                           &decoder))) {
        decoder->GetFrame(index, &frame);
    }
    return frame;
}

WICPixelFormatGUID FormatOf(IWICBitmapSource* source) {
    WICPixelFormatGUID format = GUID_WICPixelFormatDontCare;
    if (source) source->GetPixelFormat(&format);
    return format;
}

UINT BitsOf(const WICPixelFormatGUID& format) {
    ComPtr<IWICComponentInfo> info;
    ComPtr<IWICPixelFormatInfo> pixel;
    UINT bits = 0;
    if (SUCCEEDED(Wic()->CreateComponentInfo(format, &info)) && SUCCEEDED(info.As(&pixel))) pixel->GetBitsPerPixel(&bits);
    return bits;
}

Pixels OverBackground(const Pixels& premultiplied) {
    Pixels out = premultiplied;
    for (UINT32& c : out.argb) {
        const UINT inverse = 255 - (c >> 24);
        UINT32 shown = 0xFF000000;
        for (int k = 0; k < 3; ++k) {
            const UINT bg = kBackground >> (8 * (2 - k)) & 0xFF;  // COLORREF is 0x00BBGGRR
            shown |= ((c >> (8 * k) & 0xFF) + (bg * inverse * 2 + 255) / 510) << (8 * k);
        }
        c = shown;
    }
    return out;
}

Pixels Shown(const std::wstring& path, int orientation) {
    const ComPtr<IWICBitmapFrameDecode> frame = WicFrame(path);
    return frame ? OverBackground(ReadSource(Upright(frame.Get(), orientation).Get(), GUID_WICPixelFormat32bppPBGRA))
                 : Pixels{};
}

Pixels DibPixels(HBITMAP bitmap, int* bpp, HDC dc) {
    DIBSECTION ds{};
    if (bpp) *bpp = 0;
    // GetObject reports a top-down DIB's height as positive, so rows read the wrong way up show as wrong pixels.
    if (!bitmap || GetObjectW(bitmap, sizeof(ds), &ds) != sizeof(ds) || !ds.dsBm.bmBits) return {};
    GdiFlush();
    const int depth = ds.dsBm.bmBitsPixel;
    if (bpp) *bpp = depth;
    RGBQUAD colors[256] = {};
    if (depth <= 8) {
        const HDC own = dc ? nullptr : CreateCompatibleDC(nullptr);
        const HGDIOBJ old = own ? SelectObject(own, bitmap) : nullptr;
        GetDIBColorTable(dc ? dc : own, 0, 256, colors);
        if (own) {
            SelectObject(own, old);
            DeleteDC(own);
        }
    }
    Pixels p{UINT(ds.dsBm.bmWidth), UINT(ds.dsBm.bmHeight)};
    p.argb.resize(size_t(p.width) * p.height);
    for (UINT y = 0; y < p.height; ++y) {
        const BYTE* row = static_cast<const BYTE*>(ds.dsBm.bmBits) + size_t(y) * ds.dsBm.bmWidthBytes;
        for (UINT x = 0; x < p.width; ++x) {
            UINT32 c;
            if (depth == 24) {
                c = row[x * 3] | row[x * 3 + 1] << 8 | row[x * 3 + 2] << 16;
            } else if (depth == 32) {
                c = row[x * 4] | row[x * 4 + 1] << 8 | row[x * 4 + 2] << 16;
            } else {
                const UINT bit = x * depth;
                const RGBQUAD& q = colors[(row[bit / 8] >> (8 - depth - bit % 8)) & ((1 << depth) - 1)];
                c = q.rgbBlue | q.rgbGreen << 8 | q.rgbRed << 16;
            }
            p.argb[size_t(y) * p.width + x] = c | 0xFF000000;
        }
    }
    return p;
}

UINT CountColors(const Pixels& pixels, UINT cap) {
    std::unordered_set<UINT32> seen;
    for (const UINT32 c : pixels.argb) {
        seen.insert(c & 0xFFFFFF);
        if (seen.size() >= cap) break;
    }
    return UINT(seen.size());
}

int MaxDifference(const Pixels& a, const Pixels& b) {
    if (a.width != b.width || a.height != b.height || a.argb.size() != b.argb.size()) return 256;
    int most = 0;
    for (size_t i = 0; i < a.argb.size(); ++i) {
        for (int k = 0; k < 32; k += 8) most = std::max(most, std::abs(int(a.argb[i] >> k & 0xFF) - int(b.argb[i] >> k & 0xFF)));
    }
    return most;
}

ComPtr<IWICBitmap> RandomBitmap(std::mt19937& rng, const WICPixelFormatGUID& format, UINT w, UINT h, UINT colors,
                                bool opaque, bool alpha) {
    ComPtr<IWICBitmap> bitmap;
    ComPtr<IWICBitmapLock> lock;
    UINT stride = 0, size = 0;
    BYTE* data = nullptr;
    const bool indexed = format == GUID_WICPixelFormat1bppIndexed || format == GUID_WICPixelFormat2bppIndexed ||
                         format == GUID_WICPixelFormat4bppIndexed || format == GUID_WICPixelFormat8bppIndexed;
    const UINT bits = BitsOf(format);
    if (FAILED(Wic()->CreateBitmap(w, h, format, WICBitmapCacheOnLoad, &bitmap))) return nullptr;
    if (indexed) {
        const UINT entries = colors ? colors : 1u << bits;
        std::vector<WICColor> table(entries);
        for (UINT i = 0; i < entries; ++i) {
            const UINT32 a = alpha && i % 3 == 0 ? (i % 2 ? 0 : 0x80) : 0xFF;
            table[i] = a << 24 | (rng() & 0xFFFFFF);
        }
        ComPtr<IWICPalette> palette;
        if (FAILED(Wic()->CreatePalette(&palette)) || FAILED(palette->InitializeCustom(table.data(), entries)) ||
            FAILED(bitmap->SetPalette(palette.Get()))) {
            return nullptr;
        }
    }
    if (FAILED(bitmap->Lock(nullptr, WICBitmapLockWrite, &lock)) || FAILED(lock->GetStride(&stride)) ||
        FAILED(lock->GetDataPointer(&size, &data))) {
        return nullptr;
    }
    std::vector<UINT32> few(colors && !indexed ? colors : 0);
    for (UINT32& c : few) c = rng() & 0xFFFFFF;
    for (UINT y = 0; y < h; ++y) {
        BYTE* const row = data + size_t(y) * stride;
        if (!few.empty()) {  // 24bpp
            for (UINT x = 0; x < w; ++x) memcpy(row + x * 3, &few[rng() % few.size()], 3);
        } else if (indexed) {
            const UINT entries = colors ? colors : 1u << bits;
            memset(row, 0, (size_t(w) * bits + 7) / 8);
            for (UINT x = 0; x < w; ++x) {
                const UINT bit = x * bits;
                row[bit / 8] |= BYTE((rng() % entries) << (8 - bits - bit % 8));
            }
        } else {
            for (UINT x = 0; x < (w * bits + 7) / 8; ++x) row[x] = BYTE(rng());
            if (opaque && bits == 32) {
                for (UINT x = 0; x < w; ++x) row[x * 4 + 3] = 255;
            }
        }
    }
    return bitmap;
}

ComPtr<IWICBitmap> GradientBitmap(UINT w, UINT h, bool alpha) {
    std::vector<UINT32> pixels(size_t(w) * h);
    for (UINT y = 0; y < h; ++y) {
        for (UINT x = 0; x < w; ++x) {
            const UINT32 a = alpha ? 255 - x * 255 / std::max(1u, w - 1) : 255;
            pixels[size_t(y) * w + x] = a << 24 | (x * 255 / w) << 16 | (y * 255 / h) << 8 | ((x + y) * 127 / (w + h));
        }
    }
    ComPtr<IWICBitmap> bitmap;
    Wic()->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppBGRA, w * 4, UINT(pixels.size() * 4),
                                  reinterpret_cast<BYTE*>(pixels.data()), &bitmap);
    return bitmap;
}

HRESULT EncodeTo(IStream* stream, const GUID& container, IWICBitmapSource* source, const WICPixelFormatGUID* format,
                 int orientation, IWICColorContext* profile) {
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> options;
    UINT w = 0, h = 0;
    WICPixelFormatGUID pixelFormat = format ? *format : FormatOf(source);
    HRESULT hr = source ? source->GetSize(&w, &h) : E_POINTER;
    if (SUCCEEDED(hr)) hr = Wic()->CreateEncoder(container, nullptr, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, &options);
    if (SUCCEEDED(hr) && container == GUID_ContainerFormatBmp) {  // keeps alpha, as the viewer saves BMP
        PROPBAG2 name{};
        name.pstrName = const_cast<LPOLESTR>(L"EnableV5Header32bppBGRA");
        VARIANT value{};
        value.vt = VT_BOOL;
        value.boolVal = VARIANT_TRUE;
        options->Write(1, &name, &value);
    }
    if (SUCCEEDED(hr)) hr = frame->Initialize(options.Get());
    if (SUCCEEDED(hr)) hr = frame->SetSize(w, h);
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&pixelFormat);
    if (SUCCEEDED(hr) && profile) hr = frame->SetColorContexts(1, &profile);
    ComPtr<IWICPalette> palette;
    if (SUCCEEDED(hr) && SUCCEEDED(Wic()->CreatePalette(&palette)) && SUCCEEDED(source->CopyPalette(palette.Get()))) {
        frame->SetPalette(palette.Get());
    }
    if (SUCCEEDED(hr) && orientation != 1) {
        ComPtr<IWICMetadataQueryWriter> meta;
        PROPVARIANT value;
        PropVariantInit(&value);
        value.vt = VT_UI2;
        value.uiVal = USHORT(orientation);
        hr = frame->GetMetadataQueryWriter(&meta);
        const wchar_t* query = container == GUID_ContainerFormatJpeg ? L"/app1/ifd/{ushort=274}" : L"/ifd/{ushort=274}";
        if (SUCCEEDED(hr)) hr = meta->SetMetadataByName(query, &value);
    }
    if (SUCCEEDED(hr)) hr = frame->WriteSource(source, nullptr);
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();
    return hr;
}

HRESULT EncodeFile(const std::wstring& path, const GUID& container, IWICBitmapSource* source,
                   const WICPixelFormatGUID* format, int orientation, IWICColorContext* profile) {
    ComPtr<IWICStream> stream;
    HRESULT hr = Wic()->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = EncodeTo(stream.Get(), container, source, format, orientation, profile);
    return hr;
}

std::vector<BYTE> EncodeBytes(const GUID& container, IWICBitmapSource* source, const WICPixelFormatGUID* format) {
    ComPtr<IStream> stream;
    std::vector<BYTE> data;
    STATSTG stat;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) || FAILED(EncodeTo(stream.Get(), container, source, format)) ||
        FAILED(stream->Stat(&stat, STATFLAG_NONAME)) || FAILED(stream->Seek({}, STREAM_SEEK_SET, nullptr))) {
        return data;
    }
    data.resize(size_t(stat.cbSize.QuadPart));
    ULONG n = 0;
    stream->Read(data.data(), ULONG(data.size()), &n);
    return data;
}

ComPtr<IWICColorContext> SrgbProfile() {
    wchar_t system[MAX_PATH];
    GetSystemDirectoryW(system, MAX_PATH);
    const std::wstring icc = std::wstring(system) + L"\\spool\\drivers\\color\\sRGB Color Space Profile.icm";
    ComPtr<IWICColorContext> profile;
    if (FAILED(Wic()->CreateColorContext(&profile)) || FAILED(profile->InitializeFromFilename(icc.c_str()))) return nullptr;
    return profile;
}

HRESULT WriteImage(IWICImagingFactory* wic, const std::wstring& path, const GUID& container, WICPixelFormatGUID format,
                   UINT w, UINT h, int orientation, std::mt19937& rng, UINT fewColorRows, UINT fewColors,
                   IWICColorContext* profile) {
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IWICMetadataQueryWriter> meta;
    HRESULT hr = wic->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = wic->CreateEncoder(container, nullptr, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, nullptr);
    if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
    if (SUCCEEDED(hr)) hr = frame->SetSize(w, h);
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
    if (SUCCEEDED(hr) && profile) hr = frame->SetColorContexts(1, &profile);
    const bool indexed = format == GUID_WICPixelFormat8bppIndexed;
    if (SUCCEEDED(hr) && indexed) {
        ComPtr<IWICPalette> palette;
        WICColor colors[200];
        for (WICColor& c : colors) c = 0xFF000000 | (rng() & 0xFFFFFF);
        hr = wic->CreatePalette(&palette);
        if (SUCCEEDED(hr)) hr = palette->InitializeCustom(colors, 200);
        if (SUCCEEDED(hr)) hr = frame->SetPalette(palette.Get());
    }
    if (SUCCEEDED(hr) && orientation) {
        PROPVARIANT value;
        PropVariantInit(&value);
        value.vt = VT_UI2;
        value.uiVal = USHORT(orientation);
        hr = frame->GetMetadataQueryWriter(&meta);
        const wchar_t* query = container == GUID_ContainerFormatJpeg ? L"/app1/ifd/{ushort=274}" : L"/ifd/{ushort=274}";
        if (SUCCEEDED(hr)) hr = meta->SetMetadataByName(query, &value);
    }
    const UINT bpp = indexed || format == GUID_WICPixelFormat8bppGray ? 8 : format == GUID_WICPixelFormat32bppBGRA ? 32 : 24;
    const UINT stride = (w * bpp / 8 + 3) & ~3u;
    std::vector<BYTE> pixels(size_t(stride) * h);
    std::vector<UINT32> few(fewColorRows ? fewColors : 0);
    for (UINT32& c : few) c = rng() & 0xFFFFFF;
    for (UINT y = 0; y < h; ++y) {
        BYTE* const row = pixels.data() + size_t(y) * stride;
        if (y < fewColorRows) {
            for (UINT x = 0; x < w; ++x) memcpy(row + x * 3, &few[rng() % fewColors], 3);
        } else {
            for (UINT x = 0; x < w * bpp / 8; ++x) row[x] = BYTE(indexed ? rng() % 200 : rng());
        }
    }
    if (SUCCEEDED(hr)) hr = frame->WritePixels(h, stride, UINT(pixels.size()), pixels.data());
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();
    return hr;
}

std::vector<BYTE> WicUpright(IWICImagingFactory* wic, const std::wstring& path, int orientation, UINT& w, UINT& h) {
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    ComPtr<IWICBitmap> buffered;
    ComPtr<IWICBitmapFlipRotator> rotator;
    std::vector<BYTE> pixels;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                              &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(wic->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeCustom)) ||
        FAILED(wic->CreateBitmapFromSource(converter.Get(), WICBitmapCacheOnLoad, &buffered)) ||
        FAILED(wic->CreateBitmapFlipRotator(&rotator)) ||
        FAILED(rotator->Initialize(buffered.Get(), kTransforms[orientation])) || FAILED(rotator->GetSize(&w, &h))) {
        return pixels;
    }
    const UINT stride = (w * 3 + 3) & ~3u;
    pixels.resize(size_t(stride) * h);
    rotator->CopyPixels(nullptr, stride, UINT(pixels.size()), pixels.data());
    return pixels;
}

namespace {

// Packs 9-bit LZW codes, least significant bit first, into GIF data sub-blocks.
struct CodeWriter {
    std::vector<BYTE>& out;
    std::vector<BYTE> bytes;
    UINT bits = 0, count = 0;

    void Put(UINT code) {
        bits |= code << count;
        for (count += 9; count >= 8; count -= 8, bits >>= 8) bytes.push_back(BYTE(bits));
    }
    void Finish() {
        if (count) bytes.push_back(BYTE(bits));
        for (size_t at = 0; at < bytes.size(); at += 255) {
            const size_t n = std::min<size_t>(255, bytes.size() - at);
            out.push_back(BYTE(n));
            out.insert(out.end(), bytes.begin() + ptrdiff_t(at), bytes.begin() + ptrdiff_t(at + n));
        }
        out.push_back(0);
    }
};

void PutWord(std::vector<BYTE>& out, UINT value) {
    out.push_back(BYTE(value));
    out.push_back(BYTE(value >> 8));
}

// A color table: its size field, and the colors padded to that power of two.
BYTE PutColors(std::vector<BYTE>& out, const std::vector<UINT32>& colors) {
    BYTE field = 0;
    while ((2u << field) < colors.size()) ++field;
    for (UINT i = 0; i < (2u << field); ++i) {
        const UINT32 c = i < colors.size() ? colors[i] : 0;
        out.push_back(BYTE(c >> 16));
        out.push_back(BYTE(c >> 8));
        out.push_back(BYTE(c));
    }
    return field;
}

}  // namespace

std::vector<BYTE> WriteGif(const Gif& gif) {
    std::vector<BYTE> out = {'G', 'I', 'F', '8', '9', 'a'};
    PutWord(out, gif.width);
    PutWord(out, gif.height);
    const size_t flags = out.size();
    out.insert(out.end(), {0, 0, 0});  // flags, background index, aspect
    if (!gif.palette.empty()) out[flags] = BYTE(0x80 | PutColors(out, gif.palette));
    if (gif.loops >= 0) {
        out.insert(out.end(), {0x21, 0xFF, 11, 'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0', 3, 1});
        PutWord(out, UINT(gif.loops));
        out.push_back(0);
    }
    for (const GifFrame& f : gif.frames) {
        out.insert(out.end(), {0x21, 0xF9, 4, BYTE(f.disposal << 2 | (f.transparent >= 0 ? 1 : 0))});
        PutWord(out, f.delay);
        out.insert(out.end(), {BYTE(f.transparent >= 0 ? f.transparent : 0), 0});
        out.push_back(0x2C);
        PutWord(out, UINT(f.rect.left));
        PutWord(out, UINT(f.rect.top));
        PutWord(out, UINT(f.rect.right - f.rect.left));
        PutWord(out, UINT(f.rect.bottom - f.rect.top));
        const size_t frameFlags = out.size();
        out.push_back(0);
        if (!f.palette.empty()) out[frameFlags] = BYTE(0x80 | PutColors(out, f.palette));
        // Every index as a literal code, clearing the table before it would grow past 9-bit codes.
        out.push_back(8);
        CodeWriter codes{out};
        for (size_t i = 0; i < f.indices.size(); ++i) {
            if (i % 250 == 0) codes.Put(256);
            codes.Put(f.indices[i]);
        }
        codes.Put(257);
        codes.Finish();
    }
    out.push_back(0x3B);
    return out;
}

GifFrame RandomGifFrame(std::mt19937& rng, RECT rect, UINT colors) {
    GifFrame f{rect};
    f.indices.resize(size_t(rect.right - rect.left) * (rect.bottom - rect.top));
    for (BYTE& i : f.indices) i = BYTE(rng() % colors);
    return f;
}

std::vector<Pixels> ComposeGif(const Gif& gif) {
    const UINT32 background = 0xFF000000 | GetRValue(kBackground) << 16 | GetGValue(kBackground) << 8 |
                              GetBValue(kBackground);
    std::vector<Pixels> shown;
    Pixels canvas{gif.width, gif.height, std::vector<UINT32>(size_t(gif.width) * gif.height, background)};
    for (const GifFrame& f : gif.frames) {
        const Pixels before = canvas;
        const std::vector<UINT32>& colors = f.palette.empty() ? gif.palette : f.palette;
        const LONG w = f.rect.right - f.rect.left;
        for (LONG y = f.rect.top; y < f.rect.bottom; ++y) {
            for (LONG x = f.rect.left; x < f.rect.right; ++x) {
                const BYTE index = f.indices[size_t(y - f.rect.top) * w + (x - f.rect.left)];
                if (x >= LONG(gif.width) || y >= LONG(gif.height) || index == f.transparent) continue;
                canvas.argb[size_t(y) * gif.width + x] = 0xFF000000 | (index < colors.size() ? colors[index] : 0);
            }
        }
        shown.push_back(canvas);
        for (LONG y = f.rect.top; y < std::min(f.rect.bottom, LONG(gif.height)); ++y) {
            for (LONG x = f.rect.left; x < std::min(f.rect.right, LONG(gif.width)); ++x) {
                const size_t i = size_t(y) * gif.width + x;
                if (f.disposal == 2) canvas.argb[i] = background;
                if (f.disposal == 3) canvas.argb[i] = before.argb[i];
            }
        }
    }
    return shown;
}

DWORD GdiObjects() {
    return GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
}

DWORD Handles() {
    DWORD count = 0;
    GetProcessHandleCount(GetCurrentProcess(), &count);
    return count;
}
