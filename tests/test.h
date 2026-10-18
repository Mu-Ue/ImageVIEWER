#pragma once

// Shared by the tests: failure reporting, a scratch folder, and ways to make images and read their pixels back
// through WIC's own decoders, which the viewer's results are checked against.

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <random>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

// Records a failure of the test running, which then fails the run.
void Fail(const char* what, int value);

inline void Check(bool ok, const char* what, int value = 0) {
    if (!ok) Fail(what, value);
}

// The run's WIC factory, made the way the viewer makes its own.
IWICImagingFactory* Wic();

// A path in the run's own scratch folder, which is deleted when the run ends.
std::wstring TempPath(const std::wstring& name);
// A new, empty folder in the scratch folder, ending in a backslash.
std::wstring TempDir(const std::wstring& name);

// The hidden desktop the run's windows are made on, as "WinSta0\name" for STARTUPINFO.
const std::wstring& TestDesktop();
// Whether Windows opens paths longer than MAX_PATH for programs that ask, as the viewer's manifest does.
bool LongPathsEnabled();

std::vector<BYTE> ReadAll(const std::wstring& path);
bool WriteAll(const std::wstring& path, const std::vector<BYTE>& data);
// A stream on a copy of data in memory, as a pasted image is read.
ComPtr<IStream> MemoryStream(const std::vector<BYTE>& data);

// Pixels as 0xAARRGGBB, top-down rows without padding.
struct Pixels {
    UINT width = 0, height = 0;
    std::vector<UINT32> argb;

    UINT32 at(UINT x, UINT y) const { return argb[size_t(y) * width + x]; }
    bool empty() const { return argb.empty(); }
    bool operator==(const Pixels&) const = default;
};

// source converted to a 32bpp format (BGRA or PBGRA), or empty if WIC can't.
Pixels ReadSource(IWICBitmapSource* source, const GUID& format = GUID_WICPixelFormat32bppBGRA);
// source as 16-bit RGBA, to compare more than 8 bits per channel.
std::vector<UINT16> ReadWide(IWICBitmapSource* source);
// source with an EXIF orientation applied by WIC's own flip/rotate.
ComPtr<IWICBitmapSource> Upright(IWICBitmapSource* source, int orientation);
// A frame of a file or of encoded data, read by the decoder WIC itself picks.
ComPtr<IWICBitmapFrameDecode> WicFrame(const std::wstring& path, UINT index = 0);
ComPtr<IWICBitmapFrameDecode> WicFrame(const std::vector<BYTE>& data, UINT index = 0);
WICPixelFormatGUID FormatOf(IWICBitmapSource* source);
UINT BitsOf(const WICPixelFormatGUID& format);
// The pixels the viewer must show for premultiplied ones: composited over kBackground, opaque.
Pixels OverBackground(const Pixels& premultiplied);
// What the viewer shows of frame 0 of a file: upright, composited over the background.
Pixels Shown(const std::wstring& path, int orientation = 1);

// The pixels of a top-down DIB section, opaque, or empty if it isn't a DIB section; bpp is set to its depth. dc,
// if given, is the DC it is selected into, which is needed to read its color table.
Pixels DibPixels(HBITMAP bitmap, int* bpp = nullptr, HDC dc = nullptr);
// The number of distinct colors, ignoring alpha, counting no further than cap.
UINT CountColors(const Pixels& pixels, UINT cap);
// The largest difference of any channel between two images of the same size, or 256 if they differ in size.
int MaxDifference(const Pixels& a, const Pixels& b);

// A bitmap in memory of random pixels in format: random colors (or alpha) for every byte, unless colors limits
// a 24bpp one to that many random colors or opaque sets all alpha to 255. Indexed formats get a palette of up to
// colors entries (default all), with alpha in some entries if alpha is set.
ComPtr<IWICBitmap> RandomBitmap(std::mt19937& rng, const WICPixelFormatGUID& format, UINT w, UINT h, UINT colors = 0,
                                bool opaque = false, bool alpha = false);
// A smooth 32bppBGRA gradient, which JPEG keeps closely, with alpha falling from left to right if alpha is set.
ComPtr<IWICBitmap> GradientBitmap(UINT w, UINT h, bool alpha = false);

// Encodes source as the one frame of container, in format if given (else the source's own), with an EXIF
// orientation if not 1 and an embedded color profile if given.
HRESULT EncodeTo(IStream* stream, const GUID& container, IWICBitmapSource* source,
                 const WICPixelFormatGUID* format = nullptr, int orientation = 1, IWICColorContext* profile = nullptr);
HRESULT EncodeFile(const std::wstring& path, const GUID& container, IWICBitmapSource* source,
                   const WICPixelFormatGUID* format = nullptr, int orientation = 1, IWICColorContext* profile = nullptr);
std::vector<BYTE> EncodeBytes(const GUID& container, IWICBitmapSource* source,
                              const WICPixelFormatGUID* format = nullptr);
// The sRGB profile Windows ships, as a color context.
ComPtr<IWICColorContext> SrgbProfile();

// Writes an image of random pixels in the given container and pixel format, with an EXIF orientation. For 24bpp,
// the first fewColorRows rows use only fewColors colors. profile, if given, is embedded.
HRESULT WriteImage(IWICImagingFactory* wic, const std::wstring& path, const GUID& container, WICPixelFormatGUID format,
                   UINT w, UINT h, int orientation, std::mt19937& rng, UINT fewColorRows = 0, UINT fewColors = 200,
                   IWICColorContext* profile = nullptr);
// The upright 24bpp pixels WIC itself makes of a file, by its own flip/rotate.
std::vector<BYTE> WicUpright(IWICImagingFactory* wic, const std::wstring& path, int orientation, UINT& w, UINT& h);

// One frame of a GIF written by WriteGif: palette indices for rect, which may reach past the canvas.
struct GifFrame {
    RECT rect;
    std::vector<BYTE> indices;  // rect width per row
    std::vector<UINT32> palette;  // its own color table (0x00RRGGBB), or empty for the global one
    UINT delay = 0;  // in 1/100 s
    BYTE disposal = 0;
    int transparent = -1;  // palette index, or -1
};

struct Gif {
    UINT width = 0, height = 0;
    std::vector<UINT32> palette;  // the global color table, or empty for none
    int loops = -1;  // NETSCAPE2.0 repeat count, -1 for none
    std::vector<GifFrame> frames;
};

std::vector<BYTE> WriteGif(const Gif& gif);
// A frame of random indices below colors.
GifFrame RandomGifFrame(std::mt19937& rng, RECT rect, UINT colors);
// The canvas after each frame of one loop, worked out from the GIF spec as the viewer shows it: each loop starts
// on a canvas of kBackground, a frame's transparent index leaves the canvas as it was, disposal 2 then clears the
// frame's area to kBackground and 3 restores what was under it.
std::vector<Pixels> ComposeGif(const Gif& gif);

// Process-wide resources, to catch leaks.
DWORD GdiObjects();
DWORD Handles();

// Formats a byte count exactly as the viewer's status bar does.
std::wstring FormatSizeForTests(UINT64 bytes);

// The fake clipboard the clipboard code is built against in the tests, so the user's own is never touched.
void FakeClipboardClear();
void FakeClipboardPut(UINT format, const std::vector<BYTE>& data);
std::vector<BYTE> FakeClipboardGet(UINT format);
std::vector<UINT> FakeClipboardFormats();  // in the order they were put
void FakeClipboardFailOpens(int count);  // the next count opens fail, as when another program holds it
UINT FakeClipboardPngFormat();
UINT FakeClipboardHtmlFormat();

// The test groups, in the order they run.
void TestScaling();
void TestScalingEdges();
void TestOrientation();
void TestFormats();
void TestDecodeSources();
void TestSniffing();
void TestDecodeLimits();
void TestBands();
void TestIcons();
void TestRobustness();
void TestConcurrentDecodes();
void TestAnimationDisposal();
void TestGifFiles();
void TestSave();
void TestBitmapSource();
void TestSaveMatrix();
void TestSaveSafety();
void TestSaveTypes();
void TestClipboardDib();
void TestClipboardFiles();
void TestClipboardName();
void TestPngLength();
void TestClipboardRoundTrip();
void TestViewerHelpers();
void TestFileDialog();
void TestNeighbors();
void TestViewer();
void TestApp();
void TestLeaks();
