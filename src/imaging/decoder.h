#pragma once

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <atomic>
#include <string>
#include <vector>

// Where an image comes from: a file, an in-memory copy of pasted data, or both of those or neither with a decoded
// bitmap that holds the exact upright source pixels (not owned), which is then read instead of decoding them.
struct Source {
    std::wstring path;
    Microsoft::WRL::ComPtr<IStream> stream;
    HBITMAP bitmap = nullptr;
};

struct Image {
    HBITMAP bitmap = nullptr;  // 24bpp or palettized top-down DIB section, composited over kBackground
    UINT width = 0;
    UINT height = 0;
    UINT fullWidth = 0, fullHeight = 0;  // upright size of the source, which is larger if it was scaled down
    bool exact = false;  // the bitmap holds every source pixel and color unchanged, so it can stand in for the source
};

// The WIC factory, made straight from windowscodecs.dll: CoCreateInstance would first look the class up
// in the COM catalog, which loads clbcatq.dll for good. Unloads the DLL again when destroyed, as COM would,
// so every WIC object must be released first.
class WicFactory {
public:
    WicFactory();
    ~WicFactory();
    WicFactory(const WicFactory&) = delete;
    WicFactory& operator=(const WicFactory&) = delete;

    HRESULT hr = S_OK;
    IWICImagingFactory* Get() const { return factory_.Get(); }
    IWICImagingFactory* operator->() const { return factory_.Get(); }

private:
    HMODULE module_ = nullptr;
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory_;
};

// How OpenSource reads colors: as stored, which Save As keeps along with the image's color profiles, or
// converted to sRGB from the image's profile, as they are shown and copied.
enum class Colors { kStored, kSrgb };

// Opens the upright image: the chosen frame, scaled to fit maxSize (0 = no limit), EXIF orientation applied.
// full, if given, is set to the stored size before scaling. If orientation is given, it is set to the
// EXIF orientation (1-8) instead of applying it, and width and height are those of the stored pixels.
// profiles, if given, gets the image's ICC profiles.
HRESULT OpenSource(IWICImagingFactory* wic, const Source& src, UINT maxSize, Colors colors,
                   Microsoft::WRL::ComPtr<IWICBitmapSource>& source, UINT& width, UINT& height,
                   SIZE* full = nullptr, int* orientation = nullptr,
                   std::vector<Microsoft::WRL::ComPtr<IWICColorContext>>* profiles = nullptr);

// Opens the encoded data of a file or pasted source, rewound, and sets format to its container. Fails for a
// source that is only a decoded bitmap, or data in a format this viewer doesn't open.
HRESULT OpenEncoded(const Source& src, Microsoft::WRL::ComPtr<IStream>& stream, GUID& format);

// Whether any pixel of the source can be less than fully opaque: its format has alpha and, if it is
// palettized, so does its palette.
bool CanBeTransparent(IWICImagingFactory* wic, IWICBitmapSource* source);

struct Animation;

// Decodes into the smallest exact DIB section, colors converted to sRGB as OpenSource does for Colors::kSrgb.
// Works without COM for WIC's own codecs, and returns CO_E_NOTINITIALIZED if one that needs it has none. If animated is
// given, it is set to whether the image is an animated GIF, and out is then its canvas showing the first
// frame, for DecodeAnimation to play. Gives up with E_ABORT once stop, if given, is set.
HRESULT DecodeImage(const Source& src, Image& out, bool* animated = nullptr, const std::atomic<bool>* stop = nullptr);

// Reads every frame of an animated GIF, which can take a while for a large one, so it gives up with E_ABORT
// once stop is set. Needs COM initialized on the calling thread.
HRESULT DecodeAnimation(const Source& src, Animation& out, const std::atomic<bool>& stop);

// Whether a file name ends in one of the image extensions this viewer opens.
bool IsImageName(const wchar_t* name);
