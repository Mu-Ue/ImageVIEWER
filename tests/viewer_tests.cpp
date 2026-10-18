// The viewer itself, built into the tests so they can drive its window and check its state. Its window lives on
// the run's hidden desktop, its file dialogs are answered by the tests' stand-in for the helper process, and its
// clipboard is the fake one.
#include "test.h"

#include <shlobj_core.h>

#include "platform/file_dialog.cpp"
#include "ui/viewer.cpp"

#include <functional>
#include <iterator>

namespace {

std::vector<std::wstring> boxes;  // the text of every message box shown, which is then closed

// Reads and closes the message boxes the viewer shows, which run their own loop that still dispatches this timer.
void CALLBACK CloseBoxes(HWND, UINT, UINT_PTR, DWORD) {
    EnumThreadWindows(GetCurrentThreadId(), [](HWND hwnd, LPARAM) -> BOOL {
        wchar_t name[16];
        if (GetClassNameW(hwnd, name, int(std::size(name))) && wcscmp(name, L"#32770") == 0 && IsWindowVisible(hwnd) &&
            !GetPropW(hwnd, L"closing")) {
            wchar_t text[1024] = L"";
            GetDlgItemTextW(hwnd, 0xFFFF, text, int(std::size(text)));
            boxes.push_back(text);
            SetPropW(hwnd, L"closing", HANDLE(1));
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        return TRUE;
    }, 0);
}

bool quitPosted = false;

// Runs the viewer's message loop, as main does, until done() or the timeout.
template <class Done>
bool PumpUntil(Done done, DWORD timeoutMs = 15000) {
    const ULONGLONG end = GetTickCount64() + timeoutMs;
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                quitPosted = true;
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (done()) return true;
        const ULONGLONG now = GetTickCount64();
        if (now >= end) return false;
        const HANDLE frame = FrameTimer();
        if (MsgWaitForMultipleObjectsEx(frame ? 1 : 0, &frame, DWORD(std::min<ULONGLONG>(end - now, 20)), QS_ALLINPUT,
                                        MWMO_INPUTAVAILABLE) == WAIT_OBJECT_0 && frame) {
            PresentFrame();
        }
    }
}

void Settle(DWORD ms = 300) {
    PumpUntil([] { return false; }, ms);
}

void Key(WPARAM vk) {
    SendMessageW(g.hwnd, WM_KEYDOWN, vk, 0);
}

std::wstring WindowText(HWND hwnd) {
    wchar_t text[1024] = L"";
    GetWindowTextW(hwnd, text, int(std::size(text)));
    return text;
}

std::wstring Status() {
    return WindowText(g.status);
}

// Waits for the image named name to be shown, with no step still being decoded.
bool WaitShown(const std::wstring& name) {
    return PumpUntil([&] { return g.name == name && g.stepTarget.empty() && worker.next == nullptr; });
}

UINT32 BoxAverage(const Pixels& src, UINT ow, UINT oh, UINT i, UINT j) {
    const UINT64 w = src.width, h = src.height;
    UINT64 sum[3] = {};
    for (UINT64 y = j * h / oh; y < h && y * oh < (j + 1) * h; ++y) {
        const UINT64 wy = std::min((y + 1) * oh, (j + 1) * h) - std::max(y * oh, UINT64(j) * h);
        for (UINT64 x = i * w / ow; x < w && x * ow < (i + 1) * w; ++x) {
            const UINT64 wx = std::min((x + 1) * ow, (i + 1) * w) - std::max(x * ow, UINT64(i) * w);
            const UINT32 c = src.at(UINT(x), UINT(y));
            for (int k = 0; k < 3; ++k) sum[k] += wx * wy * (c >> (8 * k) & 0xFF);
        }
    }
    const UINT64 area = w * h;
    return UINT32((sum[0] + area / 2) / area | (sum[1] + area / 2) / area << 8 | (sum[2] + area / 2) / area << 16);
}

// What the image area must show: each pixel the exact average of the image area it covers, at the zoom and
// offset shown, and the background around the image.
Pixels WantView() {
    const Pixels src = DibPixels(g.image.bitmap, nullptr, g.imageDC);
    const UINT cw = UINT(g.clientW), ch = UINT(g.clientH);
    const int w = int(std::lround(double(g.image.width) * g.zoom)), h = int(std::lround(double(g.image.height) * g.zoom));
    const UINT32 background = GetRValue(kBackground) << 16 | GetGValue(kBackground) << 8 | GetBValue(kBackground);
    Pixels want{cw, ch, std::vector<UINT32>(size_t(cw) * ch)};
    for (UINT j = 0; j < ch; ++j) {
        for (UINT i = 0; i < cw; ++i) {
            const int x = int(i) - g.shownX, y = int(j) - g.shownY;
            want.argb[size_t(j) * cw + i] =
                x >= 0 && y >= 0 && x < w && y < h ? BoxAverage(src, UINT(w), UINT(h), UINT(x), UINT(y)) : background;
        }
    }
    return want;
}

Pixels Capture(int width, int height, const std::function<void(HDC)>& draw) {
    BITMAPINFO bi{{sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB}};
    UINT32* bits = nullptr;
    const HBITMAP bitmap = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, reinterpret_cast<void**>(&bits), nullptr, 0);
    const HDC dc = CreateCompatibleDC(nullptr);
    const HGDIOBJ old = SelectObject(dc, bitmap);
    draw(dc);
    GdiFlush();
    Pixels p{UINT(width), UINT(height), std::vector<UINT32>(bits, bits + size_t(width) * height)};
    for (UINT32& c : p.argb) c &= 0xFFFFFF;
    SelectObject(dc, old);
    DeleteDC(dc);
    DeleteObject(bitmap);
    return p;
}

// Render's drawing of the whole image area matches the exact view.
void CheckView(int id) {
    const int w = int(g.clientW), h = int(g.clientH);
    const Pixels drawn = Capture(w, h, [&](HDC dc) {
        const HRGN all = CreateRectRgn(0, 0, w, h);
        Render(dc, all);
        DeleteObject(all);
    });
    const Pixels want = WantView();
    if (drawn != want) {
        size_t wrong = 0;
        for (size_t i = 0; i < want.argb.size(); ++i) wrong += drawn.argb[i] != want.argb[i];
        Fail("view drawn differs from the exact average", id * 1000000 + int(wrong));
    }
}

// What the window itself holds in its image area after the viewer's own painting, scrolled pixels included,
// matches the exact view. Read straight from the window, since PrintWindow would paint it all afresh.
void CheckWindow(int id) {
    UpdateWindow(g.hwnd);
    RECT client;
    GetClientRect(g.hwnd, &client);
    const Pixels window = Capture(client.right, client.bottom, [&](HDC dc) {
        const HDC source = GetDC(g.hwnd);
        if (!BitBlt(dc, 0, 0, client.right, client.bottom, source, 0, 0, SRCCOPY)) Fail("can't capture the window", id);
        ReleaseDC(g.hwnd, source);
    });
    const Pixels want = WantView();
    int wrong = 0;
    for (UINT y = 0; y < want.height && y + g.barH < window.height; ++y) {
        for (UINT x = 0; x < want.width && x < window.width; ++x) wrong += window.at(x, y + g.barH) != want.at(x, y);
    }
    if (wrong) Fail("window shows pixels other than the exact view", id * 1000000 + wrong);
}

std::wstring Percent(double zoom) {
    return std::to_wstring(std::lround(zoom * 100 * g.image.width / g.image.fullWidth)) + L"%";
}

void WantStatus(const std::wstring& resolution, UINT64 bytes, int id) {
    const std::wstring want = resolution + L", " + FormatSize(bytes) + L", " + Percent(g.zoom);
    if (Status() != want) Fail("status bar shows the wrong image info", id);
}

UINT64 SizeOf(const std::wstring& path) {
    return ReadAll(path).size();
}

void SetAnswer(const std::wstring& answer) {
    SetEnvironmentVariableW(L"IMAGEVIEWER_TEST_DIALOG", answer.empty() ? nullptr : answer.c_str());
}

bool WantBox(const std::wstring& text, int id) {
    if (!PumpUntil([] { return !boxes.empty(); }, 5000)) {
        Fail("expected message box not shown", id);
        return false;
    }
    const bool ok = boxes.front() == text;
    if (!ok) Fail("message box says the wrong thing", id);
    boxes.erase(boxes.begin());
    return ok;
}

// An HDROP of one file, as Explorer drops it.
HGLOBAL DropOf(const std::wstring& path) {
    const std::wstring files = path + L'\0' + L'\0';
    const HGLOBAL drop = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + files.size() * sizeof(wchar_t));
    BYTE* p = static_cast<BYTE*>(GlobalLock(drop));
    const DROPFILES header{sizeof(DROPFILES), {}, FALSE, TRUE};
    memcpy(p, &header, sizeof(header));
    memcpy(p + sizeof(header), files.data(), files.size() * sizeof(wchar_t));
    GlobalUnlock(drop);
    return drop;
}

}  // namespace

std::wstring FormatSizeForTests(UINT64 bytes) {
    return FormatSize(bytes);
}

// The viewer's small pure parts: sizes as Explorer shows them, error messages, names and paths, the order files
// are stepped through, and the Save As type a name starts on.
void TestViewerHelpers() {
    const struct {
        UINT64 bytes;
        const wchar_t* text;
    } kSizes[] = {
        {0, L"0 B"}, {1023, L"1023 B"}, {1024, L"1 KB"}, {1535, L"1 KB"}, {1536, L"2 KB"}, {1048575, L"1024 KB"},
        {1048576, L"1.0 MB"}, {1572864, L"1.5 MB"}, {10485759, L"10.0 MB"}, {1073217535, L"1023.5 MB"},
        {1073741824, L"1.0 GB"}, {5368709120, L"5.0 GB"}, {1099511627776, L"1024.0 GB"},
    };
    for (int i = 0; i < int(std::size(kSizes)); ++i) {
        if (FormatSize(kSizes[i].bytes) != kSizes[i].text) Fail("file size formatted wrong", i);
    }

    const struct {
        HRESULT hr;
        const wchar_t* open;
        const wchar_t* file;
    } kErrors[] = {
        {WINCODEC_ERR_UNKNOWNIMAGEFORMAT, L"It isn't a supported image.", L""},
        {WINCODEC_ERR_BADIMAGE, L"It isn't a supported image.", L""},
        {WINCODEC_ERR_IMAGESIZEOUTOFRANGE, L"It's too large to open.", L""},
        {WINCODEC_ERR_COMPONENTNOTFOUND,
         L"This format isn't supported. Codecs for HEIC/AVIF are available from the Microsoft Store.", L""},
        {HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), L"The file doesn't exist.", L""},
        {HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND), L"The file doesn't exist.", L""},
        {HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED), L"Access is denied.", L"Access is denied."},
        {HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION), L"Another program is using it.", L"Another program is using it."},
        {HRESULT_FROM_WIN32(ERROR_DISK_FULL), L"The disk is full.", L"The disk is full."},
        {STG_E_MEDIUMFULL, L"The disk is full.", L"The disk is full."},
        {E_OUTOFMEMORY, L"", L""},
    };
    for (int i = 0; i < int(std::size(kErrors)); ++i) {
        if (ErrorReason(kErrors[i].hr) != kErrors[i].open) Fail("open error explained wrong", i);
        if (FileErrorReason(kErrors[i].hr) != kErrors[i].file) Fail("file error explained wrong", i);
    }
    if (OrCode(L"", E_OUTOFMEMORY) != L"Error 0x8007000E." || OrCode(L"Why.", E_FAIL) != L"Why.") {
        Fail("error code shown wrong", 0);
    }

    if (FileName(L"C:\\a\\b.png") != L"b.png" || FileName(L"C:/a/b.png") != L"b.png" || FileName(L"b.png") != L"b.png") {
        Fail("file name taken from a path wrong", 0);
    }
    wchar_t cwd[MAX_PATH];
    GetCurrentDirectoryW(MAX_PATH, cwd);
    if (FullPath(L"rel.png") != std::wstring(cwd) + (cwd[wcslen(cwd) - 1] == L'\\' ? L"" : L"\\") + L"rel.png" ||
        FullPath(L"") != L"" || FullPath(L"C:\\a\\..\\b.png") != L"C:\\b.png") {
        Fail("full path made wrong", 0);
    }
    if (!SamePath(L"C:\\A.PNG", L"c:\\a.png") || SamePath(L"C:\\a.png", L"C:\\a.png ")) Fail("paths compared wrong", 0);
    // Like Explorer: numbers by value, case ignored, letters with accents beside their base letter.
    const wchar_t* kOrder[] = {L"1.png", L"2.png", L"10.png", L"a.png", L"B.png", L"e.png", L"\u00E9.png", L"f.png",
                               L"x9a.png", L"x10.png", L"z.png"};
    for (size_t i = 0; i + 1 < std::size(kOrder); ++i) {
        if (CompareNames(kOrder[i], kOrder[i + 1]) >= 0 || CompareNames(kOrder[i + 1], kOrder[i]) <= 0) {
            Fail("names ordered wrong", int(i));
        }
    }
    // Names that sort alike are still put in a fixed order, so stepping never treats two files as one.
    const wchar_t* composed = L"\u00E9.png";
    const wchar_t* decomposed = L"e\u0301.png";
    if (CompareNames(composed, decomposed) == 0 || CompareNames(composed, decomposed) != -CompareNames(decomposed, composed)) {
        Fail("names that sort alike aren't told apart", 0);
    }

    const struct {
        const wchar_t* name;
        DWORD index;
    } kFilters[] = {{L"a.png", 1}, {L"a.PNG", 1}, {L"a.jpg", 2}, {L"a.JPEG", 2}, {L"a.jpe", 2}, {L"a.jfif", 2},
                    {L"a.bmp", 3}, {L"a.dib", 3}, {L"a.gif", 4}, {L"a.tif", 5}, {L"a.tiff", 5}, {L"a.webp", 0},
                    {L"a.heic", 0}, {L"a", 0}, {L"a.tar.gif", 4}, {L"a.pngx", 0}, {L"a.", 0}};
    for (int i = 0; i < int(std::size(kFilters)); ++i) {
        if (SaveFilterIndex(kFilters[i].name) != kFilters[i].index) Fail("Save As starts on the wrong type", i);
    }
}

// The file dialogs run in a helper process that writes the chosen path back through a pipe. The tests' stand-in
// for the helper checks what it is passed and answers as a real one could.
void TestFileDialog() {
    const HWND owner = CreateWindowExW(0, L"STATIC", L"owner", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr,
                                       nullptr, nullptr);
    const struct {
        const wchar_t* answer;
        const wchar_t* kind;
        std::wstring name;
        std::wstring want;
    } kCases[] = {
        {L"", L"open", L"", L""},  // cancelled
        {L"C:\\photos\\cat.png", L"open", L"", L"C:\\photos\\cat.png"},
        {L"?echo", L"save", L"cat.png", L"save|#|cat.png"},
        {L"?echo", L"open", L"", L"open|#|"},
        // Quotes and backslashes can't be in a file name, and would split or escape the argument.
        {L"?echo", L"save", L"my \"odd\" na\\me.png", L"save|#|my odd name.png"},
        {L"?echo", L"save", L"  spaced  name .jpg", L"save|#|  spaced  name .jpg"},
        {L"?echo", L"save", L"caf\u00E9 \u5199\u771F \U0001F600.png", L"save|#|caf\u00E9 \u5199\u771F \U0001F600.png"},
        {L"?long", L"open", L"", L"C:\\" + std::wstring(32764, L'x')},  // the longest path Windows allows
        {L"?odd", L"open", L"", L""},   // not whole characters
        {L"?nul", L"open", L"", L""},   // a null inside
    };
    for (int i = 0; i < int(std::size(kCases)); ++i) {
        SetAnswer(kCases[i].answer);
        std::wstring want = kCases[i].want;  // # stands for the owner window, as the helper is passed it
        if (const size_t at = want.find(L'#'); at != want.npos) want.replace(at, 1, std::to_wstring(UINT_PTR(owner)));
        if (RunFileDialog(owner, kCases[i].kind, kCases[i].name) != want) Fail("file dialog answer read wrong", i);
        if (!IsWindowEnabled(owner)) Fail("owner left disabled after the file dialog", i);
    }
    SetAnswer(L"");
    DestroyWindow(owner);
}

// Stepping goes through a folder's images in Explorer's order, wrapping around at either end, past other files
// and folders, from the image shown whatever the case of its name.
void TestNeighbors() {
    const std::wstring dir = TempDir(L"neighbors");
    const wchar_t* kImages[] = {L"1.png", L"2.JPG", L"10.gif", L"a.bmp", L"B.tif", L"e.png", L"\u00E9.png",
                                L"f.webp", L"x9a.png", L"x10.png", L"z.ico"};
    for (const wchar_t* name : kImages) WriteAll(dir + name, {0});
    for (const wchar_t* other : {L"notes.txt", L"z.png.txt", L"noext"}) WriteAll(dir + other, {0});
    CreateDirectoryW((dir + L"folder.png").c_str(), nullptr);
    const int n = int(std::size(kImages));
    for (int i = 0; i < n; ++i) {
        std::wstring around[2];
        Neighbors(dir + kImages[i], around);
        if (around[0] != dir + kImages[(i + n - 1) % n] || around[1] != dir + kImages[(i + 1) % n]) {
            Fail("wrong neighbors in the folder", i);
        }
    }
    std::wstring around[2];
    Neighbors(dir + L"A.BMP", around);
    if (around[0] != dir + L"10.gif" || around[1] != dir + L"B.tif") Fail("image shown not recognized by its name", 0);
    // An image no longer there still has its place.
    Neighbors(dir + L"c.png", around);
    if (around[0] != dir + L"B.tif" || around[1] != dir + L"e.png") Fail("deleted image lost its place", 0);

    // Alone in its folder, an image has no neighbors; with one other, that one is both.
    const std::wstring alone = TempDir(L"alone");
    WriteAll(alone + L"only.png", {0});
    Neighbors(alone + L"only.png", around);
    if (!around[0].empty() || !around[1].empty()) Fail("lone image has neighbors", 0);
    WriteAll(alone + L"other.jpg", {0});
    Neighbors(alone + L"only.png", around);
    if (around[0] != alone + L"other.jpg" || around[1] != alone + L"other.jpg") Fail("pair of images not neighbors", 0);

    // Names that sort alike are both visited, in a fixed order.
    const std::wstring alike = TempDir(L"alike");
    for (const wchar_t* name : {L"\u00E9.png", L"e\u0301.png", L"z.png"}) WriteAll(alike + name, {0});
    std::wstring at = alike + L"z.png";
    std::vector<std::wstring> visited;
    for (int i = 0; i < 3; ++i) {
        Neighbors(at, around);
        at = around[1];
        visited.push_back(at);
    }
    if (visited.back() != alike + L"z.png" || visited[0] == visited[1]) Fail("names that sort alike not both visited", 0);
}

// The viewer as the user drives it, on the hidden desktop: what it shows at every zoom and offset is exactly the
// average of the image area under each pixel, and opening, stepping, playing, saving, copying and pasting all do
// what they should, with errors explained in message boxes or the status bar.
void TestViewer() {
    std::mt19937 rng(79);
    const std::wstring dir = TempDir(L"viewer");
    const std::wstring one = dir + L"1.png", two = dir + L"2.jpg", gifPath = dir + L"3.gif", big = dir + L"4 big.png",
                       bmp = dir + L"5.bmp", broken = dir + L"6 broken.png", huge = dir + L"7 huge.png";
    EncodeFile(one, GUID_ContainerFormatPng, RandomBitmap(rng, GUID_WICPixelFormat24bppBGR, 300, 200).Get());
    EncodeFile(two, GUID_ContainerFormatJpeg, RandomBitmap(rng, GUID_WICPixelFormat24bppBGR, 400, 300).Get());
    Gif gif{48, 32, std::vector<UINT32>(256), 1};
    for (UINT32& c : gif.palette) c = rng() & 0xFFFFFF;
    gif.frames = {RandomGifFrame(rng, {0, 0, 48, 32}, 256), RandomGifFrame(rng, {5, 5, 30, 20}, 256),
                  RandomGifFrame(rng, {20, 10, 48, 32}, 256)};
    gif.frames[1].disposal = 3;
    gif.frames[2].transparent = 9;
    WriteAll(gifPath, WriteGif(gif));
    EncodeFile(big, GUID_ContainerFormatPng, GradientBitmap(2000, 1500).Get());
    EncodeFile(bmp, GUID_ContainerFormatBmp, RandomBitmap(rng, GUID_WICPixelFormat8bppIndexed, 77, 55, 9).Get());
    WriteAll(broken, {'n', 'o', 'p', 'e'});
    WriteAll(dir + L"notes.txt", {'h', 'i'});
    EncodeFile(huge, GUID_ContainerFormatPng, RandomBitmap(rng, GUID_WICPixelFormat24bppBGR, kMaxBitmapSize + 116, 5).Get());

    const UINT_PTR closer = SetTimer(nullptr, 0, 50, CloseBoxes);
    if (!StartViewer(GetModuleHandleW(nullptr), one, SW_SHOWNORMAL)) {
        Fail("viewer didn't start", 0);
        KillTimer(nullptr, closer);
        return;
    }
    Settle();
    // Opened at full size, centered, with its name and info shown.
    if (g.name != L"1.png" || WindowText(g.hwnd) != L"1.png") Fail("window not named after the image", 0);
    if (g.zoom != 1.0f || !g.fit) Fail("small image not shown at 100%", 0);
    WantStatus(L"300 × 200", SizeOf(one), 1);
    if (DibPixels(g.image.bitmap, nullptr, g.imageDC) != Shown(one)) Fail("image shown differs from the file", 0);
    CheckView(1);
    CheckWindow(1);

    // Zoom keys: in and out by steps, back to fit and 100%, and no further than 64x or below fitting.
    Key(VK_OEM_PLUS);
    if (g.zoom != kZoomStep || g.fit) Fail("zoom in went wrong", 0);
    WantStatus(L"300 × 200", SizeOf(one), 2);
    CheckView(2);
    Key(VK_OEM_MINUS);
    Key(VK_SUBTRACT);
    if (g.zoom != 1.0f || !g.fit) Fail("zoom out past fitting", 0);
    for (int i = 0; i < 30; ++i) Key(VK_ADD);
    if (g.zoom != kMaxZoom) Fail("zoom in passed the largest zoom", int(g.zoom));
    WantStatus(L"300 × 200", SizeOf(one), 3);
    CheckView(3);
    Key('0');
    if (g.zoom != 1.0f || !g.fit) Fail("fit key didn't fit", 0);
    Key(VK_NUMPAD1);
    if (g.zoom != 1.0f) Fail("100% key didn't show 100%", 0);
    // Double-click: a small image goes to 200% and back.
    const LPARAM center = MAKELPARAM(LONG(g.clientW / 2), LONG(g.barH + g.clientH / 2));
    SendMessageW(g.hwnd, WM_LBUTTONDBLCLK, 0, center);
    if (g.zoom != 2.0f) Fail("double-click didn't zoom a small image to 200%", 0);
    CheckView(4);
    SendMessageW(g.hwnd, WM_LBUTTONDBLCLK, 0, center);
    if (g.zoom != 1.0f || !g.fit) Fail("double-click didn't fit again", 0);

    // Steps to the next image, and the animated GIF after it plays.
    Key(VK_RIGHT);
    if (!WaitShown(L"2.jpg")) Fail("next image not shown", 0);
    if (DibPixels(g.image.bitmap, nullptr, g.imageDC) != Shown(two)) Fail("stepped image differs from the file", 0);
    WantStatus(L"400 × 300", SizeOf(two), 5);
    CheckView(5);
    Key(VK_RIGHT);
    if (!WaitShown(L"3.gif") || !PumpUntil([] { return !g.awaitingFrames && !g.animation.frames.empty(); })) {
        Fail("animated GIF didn't start playing", 0);
    } else {
        const std::vector<Pixels> frames = ComposeGif(gif);
        if (DibPixels(g.image.bitmap, nullptr, g.imageDC) != frames[0]) Fail("first GIF frame shown wrong", 0);
        // Minimized, it pauses; restored, it plays on.
        RECT client;
        GetClientRect(g.hwnd, &client);
        SendMessageW(g.hwnd, WM_SIZE, SIZE_MINIMIZED, 0);
        if (!g.paused) Fail("animation didn't pause when minimized", 0);
        SendMessageW(g.hwnd, WM_SIZE, SIZE_RESTORED, MAKELPARAM(client.right, client.bottom));
        if (g.paused) Fail("animation didn't resume when restored", 0);
        // Played by hand from here, two loops as the file asks, then the last frame stays.
        KillTimer(g.hwnd, kAnimationTimer);
        for (int step = 1; step < 6; ++step) {
            NextFrame();
            if (DibPixels(g.image.bitmap, nullptr, g.imageDC) != frames[step % 3]) Fail("GIF frame played wrong", step);
            CheckView(100 + step);
        }
        NextFrame();
        if (!g.animation.frames.empty()) Fail("GIF played more often than it asks", 0);
        if (DibPixels(g.image.bitmap, nullptr, g.imageDC) != frames[2]) Fail("last GIF frame not kept", 0);
    }

    // A large image fits the window, zooms through 100% on its way in, and zooms at the cursor.
    Key(VK_RIGHT);
    if (!WaitShown(L"4 big.png")) Fail("large image not shown", 0);
    if (g.zoom >= 1.0f || !g.fit) Fail("large image doesn't fit the window", int(g.zoom * 100));
    WantStatus(L"2000 × 1500", SizeOf(big), 6);
    CheckView(6);
    for (int i = 0; i < 20 && g.zoom < 1.0f; ++i) Key(VK_OEM_PLUS);
    if (g.zoom != 1.0f) Fail("zooming in skipped 100%", int(g.zoom * 1000));
    Key(VK_OEM_PLUS);
    {
        const float x = g.clientW / 2 + 37, y = g.clientH / 2 - 21;
        const float imageX = (x - g.offsetX) / g.zoom, imageY = (y - g.offsetY) / g.zoom;
        POINT screen{LONG(x), LONG(y + g.barH)};
        ClientToScreen(g.hwnd, &screen);
        SendMessageW(g.hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), MAKELPARAM(screen.x, screen.y));
        if (std::abs(g.zoom - kZoomStep * kZoomStep) > 1e-5f) Fail("wheel didn't zoom a step", int(g.zoom * 1000));
        if (std::abs((x - g.offsetX) / g.zoom - imageX) > 0.01f || std::abs((y - g.offsetY) / g.zoom - imageY) > 0.01f) {
            Fail("wheel zoom moved the point under the cursor", 0);
        }
        CheckView(7);
    }
    // Dragging pans, painting only once per display refresh, and never uncovers the window's edge.
    {
        const float startX = g.offsetX, startY = g.offsetY;
        SendMessageW(g.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(400, 300 + g.barH));
        for (int i = 1; i <= 10; ++i) {
            SendMessageW(g.hwnd, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(400 - 7 * i, 300 + g.barH - 3 * i));
        }
        SendMessageW(g.hwnd, WM_LBUTTONUP, 0, MAKELPARAM(330, 270 + g.barH));
        PumpUntil([] { return !g.frameArmed; });
        if (g.offsetX != startX - 70 || g.offsetY != startY - 30) Fail("drag panned the wrong distance", 0);
        if (g.shownX != int(std::round(g.offsetX)) || g.shownY != int(std::round(g.offsetY))) Fail("pan not shown", 0);
        CheckView(8);
        CheckWindow(8);
        SendMessageW(g.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(10, 10 + g.barH));
        SendMessageW(g.hwnd, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(30000, 30000));
        SendMessageW(g.hwnd, WM_LBUTTONUP, 0, MAKELPARAM(30000, 30000));
        PumpUntil([] { return !g.frameArmed; });
        if (g.offsetX != 0 || g.offsetY != 0) Fail("pan uncovered the window's edge", int(g.offsetX));
        CheckView(9);
        CheckWindow(9);
        // And at the far edges: the image's right and bottom meet the window's.
        SendMessageW(g.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(10, 10 + g.barH));
        SendMessageW(g.hwnd, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(-30000, -30000));
        SendMessageW(g.hwnd, WM_LBUTTONUP, 0, MAKELPARAM(-30000, -30000));
        PumpUntil([] { return !g.frameArmed; });
        if (g.offsetX != g.clientW - g.imageW * g.zoom || g.offsetY != g.clientH - g.imageH * g.zoom) {
            Fail("pan past the far edge uncovered the window's edge", int(g.offsetX));
        }
        CheckView(15);
        CheckWindow(15);
        if (g.dragging) Fail("still dragging after the button was released", 0);
    }
    // Resizing keeps a fitted image fitted, and a zoomed one at its zoom.
    {
        RECT r;
        GetWindowRect(g.hwnd, &r);
        const float zoom = g.zoom;
        SetWindowPos(g.hwnd, nullptr, 0, 0, r.right - r.left - 100, r.bottom - r.top - 80, SWP_NOMOVE | SWP_NOZORDER);
        if (g.zoom != zoom) Fail("resizing changed the zoom", 0);
        CheckView(10);
        // Fit to Window fits, even from above 100%, where zooming in steps stops at 100% on the way.
        Key('0');
        if (!g.fit || g.zoom != FitZoom()) {
            Fail("Fit from above 100% didn't fit", int(g.zoom * 1000));
            Key('0');
        }
        SetWindowPos(g.hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
        if (!g.fit || g.zoom != FitZoom()) Fail("resizing didn't keep the image fitted", 0);
        CheckView(11);
        CheckWindow(11);
        // Double-click toggles between fitting and 100%, and fits from any other zoom.
        SendMessageW(g.hwnd, WM_LBUTTONDBLCLK, 0, MAKELPARAM(100, 100 + g.barH));
        if (g.zoom != 1.0f) Fail("double-click didn't show a large image at 100%", 0);
        SendMessageW(g.hwnd, WM_LBUTTONDBLCLK, 0, MAKELPARAM(100, 100 + g.barH));
        if (!g.fit) Fail("double-click didn't fit a large image again", 0);
        Key(VK_NUMPAD1);
        Key(VK_ADD);
        SendMessageW(g.hwnd, WM_LBUTTONDBLCLK, 0, MAKELPARAM(100, 100 + g.barH));
        if (!g.fit || g.zoom != FitZoom()) {
            Fail("double-click from above 100% didn't fit", int(g.zoom * 1000));
            Key('0');
        }
    }
    // Full screen covers the monitor without the bars; Esc brings the window back as it was.
    {
        RECT before, after, monitor;
        GetWindowRect(g.hwnd, &before);
        Key(VK_F11);
        GetWindowRect(g.hwnd, &after);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(g.hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        monitor = mi.rcMonitor;
        if (!g.fullscreen || g.barH || g.statusH || IsWindowVisible(g.bar) || IsWindowVisible(g.status) ||
            !EqualRect(&after, &monitor) || (GetWindowLongPtrW(g.hwnd, GWL_STYLE) & WS_CAPTION)) {
            Fail("full screen doesn't cover the monitor alone", 0);
        }
        Settle(100);
        if (!g.fit || g.zoom != FitZoom()) Fail("full screen image not fitted", 0);
        CheckView(12);
        Key(VK_ESCAPE);
        GetWindowRect(g.hwnd, &after);
        if (g.fullscreen || g.barH != MenuBarHeight() || !IsWindowVisible(g.bar) || !EqualRect(&after, &before) ||
            !(GetWindowLongPtrW(g.hwnd, GWL_STYLE) & WS_CAPTION)) {
            Fail("leaving full screen didn't restore the window", 0);
        }
        if (quitPosted || !IsWindow(g.hwnd)) Fail("Esc in full screen closed the window", 0);
    }

    // Steps skip files that don't open, and quick presses add up while the decoder catches up.
    Key(VK_RIGHT);
    if (!WaitShown(L"5.bmp")) Fail("BMP not shown", 0);
    Key(VK_RIGHT);
    Key(VK_RIGHT);  // past the broken one
    if (!WaitShown(L"7 huge.png")) Fail("quick steps past a broken file didn't add up", 0);
    WantStatus(std::to_wstring(kMaxBitmapSize + 116) + L" × 5", SizeOf(huge), 13);
    if (g.image.width != kMaxBitmapSize) Fail("huge image not scaled to the largest bitmap", int(g.image.width));
    CheckView(13);
    Key(VK_RIGHT);  // wraps around
    Key(VK_RIGHT);
    Key(VK_RIGHT);
    if (!WaitShown(L"3.gif")) Fail("quick steps around the end didn't add up", 0);
    Key(VK_LEFT);
    Key(VK_LEFT);
    Key(VK_LEFT);
    if (!WaitShown(L"7 huge.png")) Fail("quick steps back around the start didn't add up", 0);
    Key(VK_RIGHT);
    if (!WaitShown(L"1.png")) Fail("step didn't wrap around", 0);
    if (!boxes.empty()) Fail("stepping showed a message box", int(boxes.size()));

    // Opening through the file dialog, and what goes wrong there.
    const std::wstring lonely = TempDir(L"viewer lonely");
    const std::wstring only = lonely + L"only.png";
    EncodeFile(only, GUID_ContainerFormatPng, RandomBitmap(rng, GUID_WICPixelFormat24bppBGR, 50, 40, 100).Get());
    WriteAll(lonely + L"bad 1.png", {1});
    WriteAll(lonely + L"bad 2.jpg", {2});
    SetAnswer(only);
    RunCommand(kCmdOpen);
    if (!WaitShown(L"only.png")) Fail("opened image not shown", 0);
    Key(VK_RIGHT);  // every other image fails: it stays
    Settle(500);
    if (g.name != L"only.png" || !g.stepTarget.empty() || !boxes.empty()) Fail("stepping among broken files moved", 0);
    SetAnswer(broken);
    RunCommand(kCmdOpen);
    WantBox(L"Can't open \"6 broken.png\".\n\nIt isn't a supported image.", 0);
    SetAnswer(dir + L"missing.png");
    RunCommand(kCmdOpen);
    WantBox(L"Can't open \"missing.png\".\n\nThe file doesn't exist.", 1);
    SetAnswer(L"");
    RunCommand(kCmdOpen);  // cancelled
    Settle();
    if (g.name != L"only.png" || !boxes.empty()) Fail("failed or cancelled open changed the image", 0);

    // Saving through the file dialog: it starts on the image's name, and failures are explained.
    const std::wstring saved = TempDir(L"viewer saved") + L"saved.tif";
    SetAnswer(saved);
    RunCommand(kCmdSaveAs);
    if (ReadSource(WicFrame(saved).Get()) != ReadSource(WicFrame(only).Get())) Fail("Save As saved other pixels", 0);
    const std::wstring echoed = TempDir(L"viewer echo");
    SetAnswer(L"?echo:" + echoed);
    RunCommand(kCmdSaveAs);  // the helper's echo isn't a valid file name
    WantBox(L"Can't save \"" + echoed + L"save|" + std::to_wstring(UINT_PTR(g.hwnd)) + L"|only.png\".\n\nError 0x8007007B.", 2);
    const std::wstring nowhere = dir + L"no such folder\\x.png";
    SetAnswer(nowhere);
    RunCommand(kCmdSaveAs);
    WantBox(L"Can't save \"" + nowhere + L"\".\n\nError 0x80070003.", 3);
    SetAnswer(L"");

    // Copy puts the image on the clipboard; Paste shows what is there, or says why not.
    FakeClipboardClear();
    RunCommand(kCmdCopy);
    if (FakeClipboardFormats() != std::vector<UINT>{FakeClipboardPngFormat(), CF_DIB} ||
        ReadSource(WicFrame(FakeClipboardGet(FakeClipboardPngFormat())).Get()) != ReadSource(WicFrame(only).Get())) {
        Fail("Copy didn't put the image on the clipboard", 0);
    }
    FakeClipboardClear();
    RunCommand(kCmdPaste);
    if (Status() != L"Nothing to paste: the clipboard doesn't hold an image.") Fail("empty paste not explained", 0);
    FakeClipboardPut(FakeClipboardPngFormat(), {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n', 0, 0, 0, 0});
    RunCommand(kCmdPaste);
    PumpUntil([] { return Status().starts_with(L"Can't paste."); }, 5000);
    if (Status() != L"Can't paste. It isn't a supported image.") Fail("bad paste not explained", 0);
    if (g.name != L"only.png") Fail("bad paste replaced the image", 0);
    FakeClipboardClear();
    const std::vector<BYTE> pastedPng = ReadAll(one);
    FakeClipboardPut(FakeClipboardPngFormat(), pastedPng);
    const std::string html = "<img src=\"https://example.com/pics/cat.png\">";
    FakeClipboardPut(FakeClipboardHtmlFormat(), std::vector<BYTE>(html.begin(), html.end()));
    RunCommand(kCmdPaste);
    if (!WaitShown(L"cat.png")) {
        Fail("pasted image not shown with its name", 0);
    } else {
        if (DibPixels(g.image.bitmap, nullptr, g.imageDC) != Shown(one)) Fail("pasted image shown wrong", 0);
        if (!g.source.path.empty() || g.source.stream || g.source.bitmap != g.image.bitmap) {
            Fail("exact pasted image still holds its data", 0);
        }
        // The bad paste's message stays its few seconds, then the image info returns.
        if (Status() != L"Can't paste. It isn't a supported image.") Fail("status message didn't stay its time", 0);
        SendMessageW(g.status, WM_TIMER, 1, 0);
        WantStatus(L"300 × 200", pastedPng.size(), 14);
        Key(VK_RIGHT);  // a pasted image has no folder to step through
        Settle();
        if (g.name != L"cat.png") Fail("stepped from a pasted image", 0);
        // Saving and copying a pasted image read its bitmap.
        const std::wstring pastedSave = TempDir(L"viewer pasted") + L"cat.bmp";
        SetAnswer(pastedSave);
        RunCommand(kCmdSaveAs);
        SetAnswer(L"");
        if (ReadSource(WicFrame(pastedSave).Get()) != ReadSource(WicFrame(one).Get())) Fail("pasted image saved wrong", 0);
    }
    // A file copied in Explorer opens as that file, which can be stepped from again.
    FakeClipboardClear();
    std::vector<BYTE> drop(sizeof(DROPFILES));
    const DROPFILES header{sizeof(DROPFILES), {}, FALSE, TRUE};
    memcpy(drop.data(), &header, sizeof(header));
    const std::wstring files = two + L'\0' + L'\0';
    drop.insert(drop.end(), reinterpret_cast<const BYTE*>(files.data()),
                reinterpret_cast<const BYTE*>(files.data() + files.size()));
    FakeClipboardPut(CF_HDROP, drop);
    RunCommand(kCmdPaste);
    if (!WaitShown(L"2.jpg") || g.source.path != two) Fail("pasted file not opened", 0);
    Key(VK_RIGHT);
    if (!WaitShown(L"3.gif")) Fail("couldn't step from a pasted file", 0);
    FakeClipboardClear();

    // Opening ends a step still being decoded, so even when the open fails, the next step starts from the image
    // shown.
    Key(VK_RIGHT);
    SendMessageW(g.hwnd, WM_DROPFILES, WPARAM(DropOf(dir + L"missing.png")), 0);
    if (!g.stepTarget.empty()) Fail("open didn't end the step in progress", 0);
    WantBox(L"Can't open \"missing.png\".\n\nThe file doesn't exist.", 4);
    if (g.name != L"3.gif") Fail("failed open replaced the image", 0);

    // A file dropped from Explorer opens.
    SendMessageW(g.hwnd, WM_DROPFILES, WPARAM(DropOf(bmp)), 0);
    if (!WaitShown(L"5.bmp")) Fail("dropped file not opened", 0);

    // An image decoded while a save reads the one shown appears once the save is done.
    {
        SetAnswer(TempDir(L"viewer held") + L"held.png");
        Key(VK_RIGHT);  // decodes while the save below runs
        RunCommand(kCmdSaveAs);
        SetAnswer(L"");
        if (!WaitShown(L"7 huge.png")) Fail("image decoded during a save not shown after it", 0);
    }

    // Esc closes the window and ends the loop.
    Key(VK_ESCAPE);
    if (!PumpUntil([] { return quitPosted; }, 5000) || IsWindow(g.hwnd)) Fail("Esc didn't close the viewer", 0);
    KillTimer(nullptr, closer);
    if (!boxes.empty()) Fail("unexpected message box", int(boxes.size()));
}
