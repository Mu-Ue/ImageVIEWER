// Color management: images in a color space other than sRGB are shown and copied converted to sRGB, the colors they
// were meant to have, by WIC's own transform. sRGB and untagged images stay exact, and Save As keeps the stored
// colors with their profile.
#include <cstring>

#include "imaging/decoder.h"
#include "imaging/encoder.h"
#include "test.h"

namespace {

Pixels Decoded(const Source& src, Image* keep = nullptr) {
    Image image;
    if (FAILED(DecodeImage(src, image))) return {};
    Pixels p = DibPixels(image.bitmap);
    if (keep) {
        *keep = image;
    } else {
        DeleteObject(image.bitmap);
    }
    return p;
}

// The PNG and DIB the viewer puts on the clipboard, read back as 32bpp: the PNG premultiplied, the DIB opaque.
void Copied(const Source& src, Pixels& png, Pixels& dib, std::vector<BYTE>* dibBytes = nullptr) {
    HGLOBAL pngData = nullptr, dibData = nullptr;
    png = dib = {};
    if (FAILED(EncodeClipboardImage(src, pngData, dibData))) return;
    const auto bytes = [](HGLOBAL h) {
        const BYTE* p = static_cast<const BYTE*>(GlobalLock(h));
        std::vector<BYTE> v(p, p + GlobalSize(h));
        GlobalUnlock(h);
        GlobalFree(h);
        return v;
    };
    const std::vector<BYTE> pngBytes = bytes(pngData);
    std::vector<BYTE> dibFile = bytes(dibData);
    if (dibBytes) *dibBytes = dibFile;
    png = ReadSource(WicFrame(pngBytes).Get(), GUID_WICPixelFormat32bppPBGRA);
    // A DIB is a BMP file without its 14-byte file header.
    BITMAPFILEHEADER header{0x4D42, DWORD(sizeof(BITMAPFILEHEADER) + dibFile.size()), 0, 0,
                            DWORD(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER))};
    dibFile.insert(dibFile.begin(), reinterpret_cast<BYTE*>(&header), reinterpret_cast<BYTE*>(&header + 1));
    dib = ReadSource(WicFrame(dibFile).Get());
}

std::vector<BYTE> ProfileBytes(IWICBitmapFrameDecode* frame) {
    UINT count = 0;
    if (!frame || FAILED(frame->GetColorContexts(0, nullptr, &count)) || count == 0) return {};
    std::vector<ComPtr<IWICColorContext>> contexts(count);
    std::vector<IWICColorContext*> raw;
    for (auto& c : contexts) {
        Wic()->CreateColorContext(&c);
        raw.push_back(c.Get());
    }
    frame->GetColorContexts(count, raw.data(), &count);
    for (UINT i = 0; i < count; ++i) {
        WICColorContextType type;
        UINT size = 0;
        if (SUCCEEDED(contexts[i]->GetType(&type)) && type == WICColorContextProfile &&
            SUCCEEDED(contexts[i]->GetProfileBytes(0, nullptr, &size))) {
            std::vector<BYTE> bytes(size);
            contexts[i]->GetProfileBytes(size, bytes.data(), &size);
            return bytes;
        }
    }
    return {};
}

}  // namespace

void TestColorManagement() {
    std::mt19937 rng(71);
    const std::vector<BYTE> p3Bytes = DisplayP3Profile();
    const ComPtr<IWICColorContext> p3 = ProfileFrom(p3Bytes);
    if (!p3) {
        Fail("can't make a P3 profile", 0);
        return;
    }

    // Wide-gamut images, opaque and with alpha, upright and rotated, small and in JPEG bands, show converted.
    struct Case {
        const wchar_t* name;
        const GUID& container;
        ComPtr<IWICBitmap> pixels;
        int orientation;
        bool alpha;
    };
    const Case cases[] = {
        {L"p3.png", GUID_ContainerFormatPng, GradientBitmap(300, 200), 1, false},
        {L"p3 alpha.png", GUID_ContainerFormatPng, GradientBitmap(300, 200, true), 1, true},
        {L"p3 small.jpg", GUID_ContainerFormatJpeg, GradientBitmap(320, 240), 1, false},
        {L"p3 banded.jpg", GUID_ContainerFormatJpeg, GradientBitmap(1600, 1000), 1, false},
        {L"p3 banded rotated.jpg", GUID_ContainerFormatJpeg, GradientBitmap(1600, 1000), 6, false},
        {L"p3 rotated.png", GUID_ContainerFormatPng, GradientBitmap(90, 60), 3, false},
    };
    int id = 0;
    for (const Case& c : cases) {
        ++id;
        const std::wstring path = TempPath(c.name);
        const WICPixelFormatGUID format = c.alpha ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat24bppBGR;
        HRESULT hr = c.container == GUID_ContainerFormatJpeg || c.orientation == 1
                         ? EncodeFile(path, c.container, c.pixels.Get(), &format, c.orientation, p3.Get())
                         : E_FAIL;
        const int orientation = c.orientation;
        bool tiff = false;
        if (FAILED(hr)) {  // PNG keeps no orientation: rotate it as a TIFF instead
            tiff = true;
            hr = EncodeFile(path, GUID_ContainerFormatTiff, c.pixels.Get(), &format, c.orientation, p3.Get());
        }
        const ComPtr<IWICBitmapFrameDecode> frame = WicFrame(path);
        if (FAILED(hr) || ProfileBytes(frame.Get()) != p3Bytes) {
            Fail("can't write an image with a P3 profile", id);
            continue;
        }
        const Pixels want = ShownInSrgb(frame.Get(), p3.Get(), orientation, c.alpha);
        Image image;
        const Pixels got = Decoded(Source{path}, &image);
        if (want.empty() || got != want) Fail("P3 image not shown converted to sRGB", id * 100 + MaxDifference(got, want));
        if (got == Shown(path, orientation)) Fail("P3 image shown with its stored colors", id);
        if (image.exact) Fail("converted image counts as exact", id);
        if (image.bitmap) DeleteObject(image.bitmap);

        // Copied as shown, so other programs, which read the clipboard as sRGB, see the same colors.
        Pixels png, dib;
        Copied(Source{path}, png, dib);
        if (png.empty() || OverBackground(png) != want) Fail("P3 image not copied converted", id);
        if (!c.alpha && dib != want) Fail("P3 image's DIB not copied converted", id);

        // Saved in another format with its stored colors and its profile, so nothing is lost.
        const std::wstring saved = TempPath(L"saved " + std::to_wstring(id) + (tiff ? L".png" : L".tif"));
        if (FAILED(SaveImage(Source{path}, saved))) {
            Fail("P3 image didn't save", id);
        } else {
            const ComPtr<IWICBitmapFrameDecode> back = WicFrame(saved);
            if (ProfileBytes(back.Get()) != p3Bytes) Fail("saved image lost its P3 profile", id);
            // Upright, as the saved file has no orientation of its own.
            if (ReadSource(back.Get()) != ReadSource(Upright(frame.Get(), orientation).Get())) {
                Fail("saved image's stored colors changed", id);
            }
        }
        DeleteFileW(saved.c_str());
        DeleteFileW(path.c_str());
    }

    // sRGB profiles convert to the same colors, so those images stay exact: also with alpha, which WIC's 32bpp
    // transform would move by up to 3 levels.
    int which = 0;
    for (const WICPixelFormatGUID& format : {GUID_WICPixelFormat24bppBGR, GUID_WICPixelFormat32bppBGRA,
                                             GUID_WICPixelFormat8bppIndexed, GUID_WICPixelFormat64bppRGBA,
                                             GUID_WICPixelFormat8bppGray}) {
        const std::wstring path = TempPath(L"srgb.png");
        EncodeFile(path, GUID_ContainerFormatPng, RandomBitmap(rng, format, 64, 48, 0, false, true).Get(), &format, 1,
                   SrgbProfile().Get());
        if (Decoded(Source{path}) != Shown(path)) Fail("sRGB-tagged image's colors changed", which);
        ++which;
        DeleteFileW(path.c_str());
    }

    // A profile Windows can't use, and one that isn't a color space at all, show the stored colors.
    std::vector<BYTE> broken = p3Bytes;
    broken[131] = 2;  // only two of its tags: no primaries or curves
    std::vector<BYTE> garbage(512, 0x5A);
    memcpy(garbage.data() + 36, "acsp", 4);
    for (const auto& bytes : {broken, garbage}) {
        const ComPtr<IWICColorContext> profile = ProfileFrom(bytes);
        const std::wstring path = TempPath(L"broken profile.png");
        if (!profile || FAILED(EncodeFile(path, GUID_ContainerFormatPng, GradientBitmap(40, 30).Get(), nullptr, 1,
                                          profile.Get()))) {
            continue;  // WIC refused to even store it; nothing to show
        }
        if (Decoded(Source{path}) != Shown(path)) Fail("image with a broken profile not shown as stored", 0);
        DeleteFileW(path.c_str());
    }

    // CMYK with its profile: converted through it, not by WIC's naive formula.
    {
        wchar_t system[MAX_PATH];
        GetSystemDirectoryW(system, MAX_PATH);
        ComPtr<IWICColorContext> swop;
        const std::wstring icc = std::wstring(system) + L"\\spool\\drivers\\color\\RSWOP.icm";
        const std::wstring path = TempPath(L"cmyk.jpg");
        const WICPixelFormatGUID cmyk = GUID_WICPixelFormat32bppCMYK;
        if (SUCCEEDED(Wic()->CreateColorContext(&swop)) && SUCCEEDED(swop->InitializeFromFilename(icc.c_str())) &&
            SUCCEEDED(EncodeFile(path, GUID_ContainerFormatJpeg, RandomBitmap(rng, cmyk, 48, 32).Get(), &cmyk, 1,
                                 swop.Get()))) {
            const ComPtr<IWICBitmapFrameDecode> frame = WicFrame(path);
            const Pixels want = ShownInSrgb(frame.Get(), swop.Get());
            if (want.empty() || Decoded(Source{path}) != want) Fail("CMYK image not shown through its profile", 0);
        }
        DeleteFileW(path.c_str());
    }

    // A rotated image with a profile isn't exact either: a bitmap of it would lose the profile on Save As.
    {
        const std::wstring path = TempPath(L"srgb rotated.jpg");
        EncodeFile(path, GUID_ContainerFormatJpeg, GradientBitmap(64, 48).Get(), nullptr, 6, SrgbProfile().Get());
        Image image;
        if (FAILED(DecodeImage(Source{path}, image)) || image.exact) Fail("rotated image with a profile counts as exact", 0);
        if (image.bitmap) DeleteObject(image.bitmap);
        DeleteFileW(path.c_str());
    }

    // The clipboard DIB's row padding is zeroed, not whatever memory it was given.
    {
        const std::wstring path = TempPath(L"odd width.png");
        EncodeFile(path, GUID_ContainerFormatPng, RandomBitmap(rng, GUID_WICPixelFormat24bppBGR, 5, 7).Get());
        for (int round = 0; round < 4; ++round) {
            // Leaves a freed block of the DIB's size full of nonzero bytes for the heap to hand out again.
            const HGLOBAL poison = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + 16 * 7);
            if (void* p = poison ? GlobalLock(poison) : nullptr) {
                memset(p, 0xCC, sizeof(BITMAPINFOHEADER) + 16 * 7);
                GlobalUnlock(poison);
            }
            if (poison) GlobalFree(poison);
            Pixels png, dib;
            std::vector<BYTE> bytes;
            Copied(Source{path}, png, dib, &bytes);
            const size_t stride = 16;  // 5 pixels of 3 bytes, padded to 4
            for (size_t y = 0; y < 7 && bytes.size() >= sizeof(BITMAPINFOHEADER) + stride * 7; ++y) {
                if (bytes[sizeof(BITMAPINFOHEADER) + y * stride + 15] != 0) Fail("clipboard DIB padding not zeroed", 0);
            }
        }
        DeleteFileW(path.c_str());
    }
}
