#include <atomic>

#include "imaging/animation.h"
#include "imaging/decoder.h"
#include "imaging/encoder.h"
#include "test.h"
#include "ui/scale.h"

// Opening, playing, saving and copying images, and failing to, again and again, leaves no GDI object or handle
// behind: the viewer does this all day.
void TestLeaks() {
    std::mt19937 rng(89);
    const std::wstring dir = TempDir(L"leaks");
    std::vector<std::wstring> paths;
    const auto add = [&](const wchar_t* name, const GUID& container, IWICBitmapSource* source, int orientation = 1) {
        paths.push_back(dir + name);
        EncodeFile(paths.back(), container, source, nullptr, orientation);
    };
    add(L"rgb.png", GUID_ContainerFormatPng, RandomBitmap(rng, GUID_WICPixelFormat24bppBGR, 120, 80).Get());
    add(L"few.png", GUID_ContainerFormatPng, RandomBitmap(rng, GUID_WICPixelFormat24bppBGR, 120, 80, 12).Get());
    add(L"alpha.png", GUID_ContainerFormatPng, RandomBitmap(rng, GUID_WICPixelFormat32bppBGRA, 120, 80).Get());
    add(L"rotated.jpg", GUID_ContainerFormatJpeg, RandomBitmap(rng, GUID_WICPixelFormat24bppBGR, 120, 80).Get(), 6);
    add(L"bands.jpg", GUID_ContainerFormatJpeg, RandomBitmap(rng, GUID_WICPixelFormat24bppBGR, 1500, 800).Get());
    add(L"gray.tif", GUID_ContainerFormatTiff, RandomBitmap(rng, GUID_WICPixelFormat8bppGray, 120, 80).Get());
    Gif gif{30, 20, std::vector<UINT32>(256, 0x336699), 0};
    gif.frames = {RandomGifFrame(rng, {0, 0, 30, 20}, 256), RandomGifFrame(rng, {5, 5, 25, 15}, 256)};
    gif.frames[0].disposal = 3;
    paths.push_back(dir + L"anim.gif");
    WriteAll(paths.back(), WriteGif(gif));
    paths.push_back(dir + L"broken.png");
    WriteAll(paths.back(), {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n', 1, 2, 3});
    paths.push_back(dir + L"missing.png");

    const auto workload = [&] {
        const HDC target = CreateCompatibleDC(nullptr), imageDC = CreateCompatibleDC(nullptr);
        BITMAPINFO bi{{sizeof(BITMAPINFOHEADER), 200, -150, 1, 32, BI_RGB}};
        void* bits = nullptr;
        const HBITMAP screen = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        const HGDIOBJ oldScreen = SelectObject(target, screen);
        for (const std::wstring& path : paths) {
            for (const Source& src : {Source{path}, Source{.stream = MemoryStream(ReadAll(path))}}) {
                Image image;
                bool animated = false;
                if (SUCCEEDED(DecodeImage(src, image, &animated))) {
                    const HGDIOBJ old = SelectObject(imageDC, image.bitmap);
                    DrawScaled(target, imageDC, 0, 0, 77, 55, {0, 0, 200, 150});
                    SelectObject(imageDC, old);
                    const Source shown{path, src.stream, image.exact ? image.bitmap : nullptr};
                    for (const wchar_t* ext : {L".png", L".jpg", L".gif"}) SaveImage(shown, dir + L"out" + ext);
                    HGLOBAL png = nullptr, dib = nullptr;
                    if (SUCCEEDED(EncodeClipboardImage(shown, png, dib))) {
                        GlobalFree(png);
                        GlobalFree(dib);
                    }
                    DeleteObject(image.bitmap);
                }
                Animation a;
                const std::atomic<bool> stop{false};
                DecodeAnimation(src, a, stop);
                SaveImage(src, dir + L"no such folder\\x.png");
            }
        }
        SelectObject(target, oldScreen);
        DeleteObject(screen);
        DeleteDC(target);
        DeleteDC(imageDC);
    };
    workload();  // anything made once and kept, such as the drawing threads, is made now
    const DWORD gdi = GdiObjects(), handles = Handles();
    for (int i = 0; i < 3; ++i) workload();
    if (GdiObjects() != gdi) Fail("GDI objects leaked", int(GdiObjects() - gdi));
    if (Handles() > handles) Fail("handles leaked", int(Handles() - handles));
}
