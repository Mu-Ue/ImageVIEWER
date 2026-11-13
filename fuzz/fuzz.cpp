// Fuzzes what untrusted data reaches, with libFuzzer and AddressSanitizer: image files (decoding, playing a GIF,
// scaling) and what other programs put on the clipboard (the paste parsers, then decoding what they accept).
// Build with -DIMAGEVIEWER_FUZZ=ON, then "set FUZZ_TARGET=decode" (or paste) and run "fuzz.exe <corpus folder>".
#include <windows.h>
#include <shlobj_core.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#include "imaging/animation.h"
#include "imaging/decoder.h"
#include "ui/scale.h"

namespace {

// One clipboard format at a time, holding the fuzzer's data; HTML naming the image goes alongside a DIB.
struct {
    UINT format = 0;
    HGLOBAL data = nullptr, html = nullptr;
} fake;

BOOL FakeOpenClipboard(HWND) { return TRUE; }
BOOL FakeCloseClipboard() { return TRUE; }
BOOL FakeEmptyClipboard() { return TRUE; }
HANDLE FakeGetClipboardData(UINT format);
HANDLE FakeSetClipboardData(UINT, HANDLE data) { return data; }
BOOL FakeIsClipboardFormatAvailable(UINT format);

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

namespace {

HANDLE FakeGetClipboardData(UINT format) {
    return format == fake.format ? fake.data : format == HtmlFormat() ? fake.html : nullptr;
}

BOOL FakeIsClipboardFormatAvailable(UINT format) {
    return format == fake.format || (fake.html && format == HtmlFormat());
}

HGLOBAL Copy(const uint8_t* data, size_t size) {
    const HGLOBAL copy = GlobalAlloc(GMEM_MOVEABLE, size ? size : 1);
    if (void* p = copy ? GlobalLock(copy) : nullptr) {
        memcpy(p, data, size);
        GlobalUnlock(copy);
    }
    return copy;
}

// Decodes an image as the viewer shows it: the first frame, then every frame of an animation, then scaled.
void Show(const Source& src) {
    Image image;
    bool animated = false;
    if (FAILED(DecodeImage(src, image, &animated))) return;
    DIBSECTION ds{};
    GetObjectW(image.bitmap, sizeof(ds), &ds);
    if (animated) {
        Animation a;
        const std::atomic<bool> stop{false};
        if (SUCCEEDED(DecodeAnimation(src, a, stop))) {
            a.bits = static_cast<BYTE*>(ds.dsBm.bmBits);
            a.stride = UINT(ds.dsBm.bmWidthBytes);
            for (size_t i = 0; i < a.frames.size(); ++i) ShowFrame(a, i);
        }
    }
    const HDC imageDC = CreateCompatibleDC(nullptr), dc = CreateCompatibleDC(nullptr);
    const HGDIOBJ oldImage = SelectObject(imageDC, image.bitmap);
    BITMAPINFO bi{{sizeof(BITMAPINFOHEADER), 64, -48, 1, 32, BI_RGB}};
    void* bits = nullptr;
    const HBITMAP target = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    const HGDIOBJ oldTarget = SelectObject(dc, target);
    const RECT clip{0, 0, 64, 48};
    DrawScaled(dc, imageDC, -3, 2, 61, 41, clip);  // shrunk
    DrawScaled(dc, imageDC, -int(image.width), -5, int(image.width) * 3, int(image.height) * 3, clip);  // enlarged
    SelectObject(dc, oldTarget);
    SelectObject(imageDC, oldImage);
    DeleteObject(target);
    DeleteDC(dc);
    DeleteDC(imageDC);
    DeleteObject(image.bitmap);
}

bool paste = false;

}  // namespace

extern "C" int LLVMFuzzerInitialize(int*, char***) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    static const WicFactory keep;  // keeps WIC loaded between inputs, instead of loading it for each
    char target[16] = "";
    GetEnvironmentVariableA("FUZZ_TARGET", target, sizeof(target));
    paste = strcmp(target, "paste") == 0;
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (!paste) {
        IStream* stream = nullptr;
        if (const HGLOBAL copy = Copy(data, size); copy && SUCCEEDED(CreateStreamOnHGlobal(copy, TRUE, &stream))) {
            Show(Source{.stream = stream});
            stream->Release();
        }
        return 0;
    }
    // The first byte picks what the data is offered as: PNG, a DIB (named by HTML from the second half), or a
    // file list.
    if (size < 1) return 0;
    const uint8_t kind = data[0] % 4;
    ++data;
    --size;
    const size_t half = kind == 3 ? size / 2 : size;
    fake.format = kind == 0 ? PngFormat() : kind == 2 ? CF_HDROP : CF_DIB;
    fake.data = Copy(data, half);
    fake.html = kind == 3 ? Copy(data + half, size - half) : nullptr;
    Source src;
    std::wstring name;
    UINT64 pngBytes = 0;
    if (ReadClipboardImage(nullptr, src, name, pngBytes) == S_OK && src.stream) Show(src);
    GlobalFree(fake.data);
    if (fake.html) GlobalFree(fake.html);
    return 0;
}
