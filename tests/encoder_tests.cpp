#include <cstring>
#include <iterator>

#include "config.h"
#include "imaging/decoder.h"
#include "imaging/encoder.h"
#include "test.h"

namespace {

// The container a file's data is in, by the viewer's own sniffing.
GUID ContainerOf(const std::wstring& path) {
    ComPtr<IStream> stream;
    GUID format = GUID_NULL;
    OpenEncoded(Source{path}, stream, format);
    return format;
}

// The temporary files a save left behind in a folder.
int TempFiles(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    int count = 0;
    const HANDLE find = FindFirstFileW((dir + L"~ImageVIEWER.*.tmp").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return 0;
    do ++count;
    while (FindNextFileW(find, &fd));
    FindClose(find);
    return count;
}

std::vector<BYTE> ProfileBytes(IWICBitmapFrameDecode* frame) {
    UINT count = 0;
    std::vector<BYTE> bytes;
    if (!frame || FAILED(frame->GetColorContexts(0, nullptr, &count)) || count == 0) return bytes;
    std::vector<ComPtr<IWICColorContext>> contexts(count);
    std::vector<IWICColorContext*> raw;
    for (auto& c : contexts) {
        Wic()->CreateColorContext(&c);
        raw.push_back(c.Get());
    }
    frame->GetColorContexts(count, raw.data(), &count);
    for (UINT i = 0; i < count; ++i) {
        UINT size = 0;
        WICColorContextType type;
        if (SUCCEEDED(contexts[i]->GetType(&type)) && type == WICColorContextProfile &&
            SUCCEEDED(contexts[i]->GetProfileBytes(0, nullptr, &size))) {
            bytes.resize(size);
            contexts[i]->GetProfileBytes(size, bytes.data(), &size);
            return bytes;
        }
    }
    return bytes;
}

// Whether a JPEG was written at the best quality: every quantization step 1, and no component subsampled.
bool BestQualityJpeg(IWICBitmapFrameDecode* frame) {
    ComPtr<IWICJpegFrameDecode> jpeg;
    WICJpegFrameHeader header{};
    if (!frame || FAILED(frame->QueryInterface(IID_PPV_ARGS(&jpeg))) || FAILED(jpeg->GetFrameHeader(&header))) {
        return false;
    }
    for (UINT c = 0; c < header.cComponents; ++c) {
        if ((header.SampleFactors >> (8 * c) & 0xFF) != 0x11) return false;
        DXGI_JPEG_QUANTIZATION_TABLE table{};
        if (FAILED(jpeg->GetQuantizationTable(header.ScanType, header.QuantizationTableIndices >> (8 * c) & 0xFF, &table))) {
            return false;
        }
        for (const BYTE step : table.Elements) {
            if (step != 1) return false;
        }
    }
    return true;
}

// The pixels composited over white, as a JPEG must show an image with alpha.
Pixels OverWhite(const Pixels& straight) {
    Pixels out = straight;
    for (UINT32& c : out.argb) {
        const UINT a = c >> 24;
        UINT32 r = 0xFF000000;
        for (int k = 0; k < 24; k += 8) r |= ((c >> k & 0xFF) * a + 255 * (255 - a) + 127) / 255 << k;
        c = r;
    }
    return out;
}

}  // namespace

// Save As copies a file saved in its own format, and composites alpha over white for JPEG.
void TestSave() {
    IWICImagingFactory* const wic = Wic();
    std::mt19937 rng(5);
    const std::wstring jpg = TempPath(L"save.jpg"), copy = TempPath(L"save copy.jpg");
    if (FAILED(WriteImage(wic, jpg, GUID_ContainerFormatJpeg, GUID_WICPixelFormat24bppBGR, 64, 48, 6, rng)) ||
        FAILED(SaveImage(Source{jpg}, copy)) || ReadAll(jpg) != ReadAll(copy)) {
        Fail("saving a JPEG as JPEG didn't copy it", 0);
    }

    // A PNG whose fully transparent pixels hide red.
    const std::wstring png = TempPath(L"alpha.png"), flat = TempPath(L"alpha.jpg");
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    std::vector<UINT32> red(64 * 48, 0x00FF0000);
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    HRESULT hr = wic->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(png.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, nullptr);
    if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
    if (SUCCEEDED(hr)) hr = frame->SetSize(64, 48);
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
    if (SUCCEEDED(hr)) hr = frame->WritePixels(48, 64 * 4, UINT(red.size() * 4), reinterpret_cast<BYTE*>(red.data()));
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();
    frame.Reset();
    encoder.Reset();
    stream.Reset();  // closes the file, so it can be read and deleted
    if (SUCCEEDED(hr)) hr = SaveImage(Source{png}, flat);
    UINT w = 0, h = 0;
    const std::vector<BYTE> pixels = SUCCEEDED(hr) ? WicUpright(wic, flat, 1, w, h) : std::vector<BYTE>();
    int dark = 0;
    for (const BYTE b : pixels) dark += b < 250;
    if (pixels.empty() || dark) Fail("transparent pixels saved as JPEG aren't white", dark);
    for (const std::wstring& path : {jpg, copy, png, flat}) DeleteFileW(path.c_str());

    // Gray pixels saved as palettized BMP and GIF keep every one of their 256 levels.
    const std::wstring gray = TempPath(L"gray.png");
    if (FAILED(WriteImage(wic, gray, GUID_ContainerFormatPng, GUID_WICPixelFormat8bppGray, 300, 200, 0, rng))) {
        Fail("can't write a gray test image", 0);
    } else {
        const std::vector<BYTE> want = WicUpright(wic, gray, 1, w, h);
        for (const wchar_t* ext : {L".bmp", L".gif"}) {
            const std::wstring saved = TempPath((std::wstring(L"gray") + ext).c_str());
            UINT sw = 0, sh = 0;
            if (FAILED(SaveImage(Source{gray}, saved)) || want.empty() || WicUpright(wic, saved, 1, sw, sh) != want) {
                Fail("gray pixels changed when saved palettized", ext[1]);
            }
            DeleteFileW(saved.c_str());
        }
    }
    DeleteFileW(gray.c_str());
}

// An image the decoded bitmap holds exactly, as a pasted one then stands in for its data, saves with every pixel
// from that bitmap on the 24bpp and palette paths, even while it is selected into a DC as the viewer keeps it.
// One with a color profile isn't exact, since the bitmap would lose the profile.
void TestBitmapSource() {
    IWICImagingFactory* const wic = Wic();
    std::mt19937 rng(11);
    const std::wstring png = TempPath(L"bitmap.png"), saved = TempPath(L"bitmap saved.png");
    const struct {
        UINT colors;
        WORD bpp;
    } kinds[] = {{0, 24}, {200, 8}, {16, 4}, {2, 1}};
    const HDC dc = CreateCompatibleDC(nullptr);
    for (const auto& kind : kinds) {
        Image image;
        DIBSECTION ds{};
        if (FAILED(WriteImage(wic, png, GUID_ContainerFormatPng, GUID_WICPixelFormat24bppBGR, 150, 97, 0, rng,
                              kind.colors ? 97 : 0, kind.colors))) {
            Fail("can't write a bitmap test image", kind.bpp);
        } else if (FAILED(DecodeImage(Source{png}, image)) || !image.exact ||
                   GetObjectW(image.bitmap, sizeof(ds), &ds) != sizeof(ds) || ds.dsBm.bmBitsPixel != kind.bpp) {
            Fail("decoded bitmap isn't the smallest exact one", kind.bpp);
        } else {
            const HGDIOBJ old = SelectObject(dc, image.bitmap);
            const HRESULT hr = SaveImage(Source{.bitmap = image.bitmap}, saved);
            SelectObject(dc, old);
            UINT w = 0, h = 0, sw = 0, sh = 0;
            const std::vector<BYTE> want = WicUpright(wic, png, 1, w, h);
            if (FAILED(hr) || want.empty() || want != WicUpright(wic, saved, 1, sw, sh) || sw != w || sh != h) {
                Fail("saving from the decoded bitmap changed pixels", kind.bpp);
            }
        }
        if (image.bitmap) DeleteObject(image.bitmap);
    }
    DeleteDC(dc);

    ComPtr<IWICColorContext> profile = SrgbProfile();
    Image image;
    if (!profile ||
        FAILED(WriteImage(wic, png, GUID_ContainerFormatPng, GUID_WICPixelFormat24bppBGR, 150, 97, 0, rng, 97, 16,
                          profile.Get())) ||
        FAILED(DecodeImage(Source{png}, image))) {
        Fail("can't decode an image with a color profile", 0);
    } else if (image.exact) {
        Fail("an image with a color profile counts as exact", 0);
    }
    if (image.bitmap) DeleteObject(image.bitmap);
    for (const std::wstring& path : {png, saved}) DeleteFileW(path.c_str());
}

// Every kind of source saved as every type: its own type is copied byte for byte, PNG and TIFF keep every pixel
// at full depth with alpha, BMP keeps 8-bit pixels and alpha, GIF keeps palettes and grays and makes alpha
// transparent, JPEG composites alpha over white and stays close, and color profiles go where the type has room.
void TestSaveMatrix() {
    std::mt19937 rng(61);
    const std::wstring dir = TempDir(L"save matrix");
    const ComPtr<IWICColorContext> profile = SrgbProfile();
    Gif anim{24, 16, std::vector<UINT32>(256), 0};
    for (UINT32& c : anim.palette) c = rng() & 0xFFFFFF;
    anim.frames = {RandomGifFrame(rng, {0, 0, 24, 16}, 256), RandomGifFrame(rng, {0, 0, 24, 16}, 256)};
    const struct {
        const wchar_t* name;
        const GUID& container;
        ComPtr<IWICBitmap> pixels;
        int orientation = 1;
        bool smooth = false;  // close to the original once saved as JPEG
        bool profiled = false;
    } kSources[] = {
        {L"rgb.png", GUID_ContainerFormatPng, RandomBitmap(rng, GUID_WICPixelFormat24bppBGR, 61, 37)},
        {L"alpha.png", GUID_ContainerFormatPng, RandomBitmap(rng, GUID_WICPixelFormat32bppBGRA, 61, 37)},
        {L"gray.png", GUID_ContainerFormatPng, RandomBitmap(rng, GUID_WICPixelFormat8bppGray, 61, 37)},
        {L"indexed.png", GUID_ContainerFormatPng, RandomBitmap(rng, GUID_WICPixelFormat8bppIndexed, 61, 37, 200)},
        {L"deep.png", GUID_ContainerFormatPng, RandomBitmap(rng, GUID_WICPixelFormat64bppRGBA, 61, 37)},
        {L"photo.jpg", GUID_ContainerFormatJpeg, GradientBitmap(61, 37), 1, true},
        {L"rotated.jpg", GUID_ContainerFormatJpeg, GradientBitmap(61, 37), 6, true},
        {L"still.gif", GUID_ContainerFormatGif, RandomBitmap(rng, GUID_WICPixelFormat8bppIndexed, 61, 37, 100)},
        {L"alpha.bmp", GUID_ContainerFormatBmp, RandomBitmap(rng, GUID_WICPixelFormat32bppBGRA, 61, 37)},
        {L"cmyk.tif", GUID_ContainerFormatTiff, RandomBitmap(rng, GUID_WICPixelFormat32bppCMYK, 61, 37)},
        {L"profiled.png", GUID_ContainerFormatPng, GradientBitmap(61, 37), 1, true, true},
        {L"soft alpha.png", GUID_ContainerFormatPng, GradientBitmap(61, 37, true), 1, true},
        {L"anim.gif", GUID_ContainerFormatGif, nullptr},
    };
    const struct {
        const wchar_t* ext;
        const GUID& container;
    } kTargets[] = {{L".png", GUID_ContainerFormatPng},
                    {L".jpg", GUID_ContainerFormatJpeg},
                    {L".bmp", GUID_ContainerFormatBmp},
                    {L".gif", GUID_ContainerFormatGif},
                    {L".tif", GUID_ContainerFormatTiff}};
    for (int s = 0; s < int(std::size(kSources)); ++s) {
        const auto& src = kSources[s];
        const std::wstring path = dir + src.name;
        const HRESULT written = src.pixels ? EncodeFile(path, src.container, src.pixels.Get(), nullptr, src.orientation,
                                                        src.profiled ? profile.Get() : nullptr)
                                           : WriteAll(path, WriteGif(anim)) ? S_OK : E_FAIL;
        if (FAILED(written)) {
            Fail("can't write a save test source", s);
            continue;
        }
        const ComPtr<IWICBitmapFrameDecode> srcFrame = WicFrame(path);
        const ComPtr<IWICBitmapSource> upright = Upright(srcFrame.Get(), src.orientation);
        const Pixels want = ReadSource(upright.Get());
        const std::vector<UINT16> wantWide = ReadWide(upright.Get());
        const WICPixelFormatGUID native = FormatOf(srcFrame.Get());
        const bool deep = BitsOf(native) > 32;
        const bool alpha = std::any_of(want.argb.begin(), want.argb.end(), [](UINT32 c) { return c >> 24 != 255; });

        for (int t = 0; t < int(std::size(kTargets)); ++t) {
            const auto& target = kTargets[t];
            const int id = s * 10 + t;
            const std::wstring out = dir + L"out " + std::to_wstring(id) + target.ext;
            if (FAILED(SaveImage(Source{path}, out))) {
                Fail("save failed", id);
                continue;
            }
            if (ContainerOf(out) != target.container) Fail("saved in the wrong type", id);
            if (src.container == target.container) {
                if (ReadAll(out) != ReadAll(path)) Fail("saving in its own type didn't copy the file", id);
                continue;
            }
            const ComPtr<IWICBitmapFrameDecode> outFrame = WicFrame(out);
            const Pixels got = ReadSource(outFrame.Get());
            if (got.width != want.width || got.height != want.height) {
                Fail("saved image has the wrong size", id);
                continue;
            }
            if (target.container == GUID_ContainerFormatPng || target.container == GUID_ContainerFormatTiff) {
                if (got != want) Fail("lossless save changed pixels", id);
                if (deep && ReadWide(outFrame.Get()) != wantWide) Fail("lossless save lost bits per channel", id);
            } else if (target.container == GUID_ContainerFormatBmp) {
                if (got != want) Fail("BMP save changed pixels or alpha", id);
                // Deeper BMPs, which WIC can write in fixed point, are ones other programs can't read.
                if (BitsOf(FormatOf(outFrame.Get())) > 32) Fail("BMP saved deeper than other programs read", id);
            } else if (target.container == GUID_ContainerFormatGif) {
                if ((native == GUID_WICPixelFormat8bppIndexed || native == GUID_WICPixelFormat8bppGray) && got != want) {
                    Fail("GIF save changed palette or gray pixels", id);
                }
                for (size_t i = 0; alpha && i < want.argb.size(); ++i) {
                    if ((want.argb[i] >> 24 < 128) != (got.argb[i] >> 24 == 0)) {
                        Fail("GIF save made the wrong pixels transparent", id);
                        break;
                    }
                }
            } else {  // JPEG
                if (!BestQualityJpeg(outFrame.Get())) Fail("JPEG not saved at the best quality", id);
                const int diff = src.smooth ? MaxDifference(got, OverWhite(want)) : 0;
                if (diff > 12) Fail("JPEG save strayed from the image over white", diff);
            }
            if (src.profiled && target.container != GUID_ContainerFormatBmp &&
                target.container != GUID_ContainerFormatGif) {
                UINT size = 0;
                profile->GetProfileBytes(0, nullptr, &size);
                std::vector<BYTE> bytes(size);
                profile->GetProfileBytes(size, bytes.data(), &size);
                if (ProfileBytes(outFrame.Get()) != bytes) Fail("color profile not kept", id);
            }
        }
    }

    // Pasted data saves the same: its own type copied, others re-encoded with the same pixels.
    const std::vector<BYTE> png = ReadAll(dir + L"alpha.png");
    const std::wstring copied = dir + L"pasted.png", converted = dir + L"pasted.tif";
    if (FAILED(SaveImage(Source{.stream = MemoryStream(png)}, copied)) || ReadAll(copied) != png) {
        Fail("pasted PNG not copied byte for byte", 0);
    }
    if (FAILED(SaveImage(Source{.stream = MemoryStream(png)}, converted)) ||
        ReadSource(WicFrame(converted).Get()) != ReadSource(WicFrame(png).Get())) {
        Fail("pasted PNG saved as TIFF changed pixels", 0);
    }

    // What the viewer passes when its bitmap is exact: the file, plus the bitmap to read instead of decoding it.
    // The file's own type is still copied from the file.
    const std::wstring rgb = dir + L"rgb.png";
    Image image;
    if (FAILED(DecodeImage(Source{rgb}, image)) || !image.exact) {
        Fail("can't decode the exact test image", 0);
    } else {
        const Source shown{rgb, nullptr, image.bitmap};
        const std::wstring same = dir + L"shown.png", other = dir + L"shown.bmp";
        if (FAILED(SaveImage(shown, same)) || ReadAll(same) != ReadAll(rgb)) Fail("shown image not copied", 0);
        if (FAILED(SaveImage(shown, other)) || ReadSource(WicFrame(other).Get()) != ReadSource(WicFrame(rgb).Get())) {
            Fail("shown image saved from its bitmap changed pixels", 0);
        }
    }
    if (image.bitmap) DeleteObject(image.bitmap);
    if (TempFiles(dir)) Fail("saves left temporary files", TempFiles(dir));
}

// Saving never leaves a partial or temporary file, replaces files whole, can replace the file being saved, and
// leaves what was there alone when it fails.
void TestSaveSafety() {
    std::mt19937 rng(67);
    const std::wstring dir = TempDir(L"save safety");
    const std::wstring png = dir + L"a.png";
    EncodeFile(png, GUID_ContainerFormatPng, RandomBitmap(rng, GUID_WICPixelFormat24bppBGR, 31, 17).Get());
    const Pixels pixels = ReadSource(WicFrame(png).Get());
    const std::vector<BYTE> before = {'o', 'l', 'd'};

    // Over an existing file.
    const std::wstring existing = dir + L"existing.bmp";
    WriteAll(existing, before);
    if (FAILED(SaveImage(Source{png}, existing)) || ReadSource(WicFrame(existing).Get()) != pixels) {
        Fail("save over an existing file didn't replace it", 0);
    }
    // Over the file being saved, in its own type and, for a misnamed one, in another.
    const std::vector<BYTE> original = ReadAll(png);
    if (FAILED(SaveImage(Source{png}, png)) || ReadAll(png) != original) Fail("saving a file over itself changed it", 0);
    const std::wstring misnamed = dir + L"misnamed.bmp";
    WriteAll(misnamed, original);  // PNG data
    if (FAILED(SaveImage(Source{misnamed}, misnamed)) || ContainerOf(misnamed) != GUID_ContainerFormatBmp ||
        ReadSource(WicFrame(misnamed).Get()) != pixels) {
        Fail("saving a misnamed file over itself didn't convert it", 0);
    }

    // Failures leave the target as it was, or absent.
    const std::wstring garbage = dir + L"garbage.png";
    WriteAll(garbage, {'n', 'o', 't', ' ', 'a', 'n', ' ', 'i', 'm', 'a', 'g', 'e'});
    const std::wstring kept = dir + L"kept.tif";
    WriteAll(kept, before);
    if (SUCCEEDED(SaveImage(Source{garbage}, kept)) || ReadAll(kept) != before) Fail("failed save changed the target", 0);
    if (SUCCEEDED(SaveImage(Source{garbage}, dir + L"new.tif")) || GetFileAttributesW((dir + L"new.tif").c_str()) !=
                                                                      INVALID_FILE_ATTRIBUTES) {
        Fail("failed save left a file", 0);
    }
    if (SaveImage(Source{png}, dir + L"no such folder\\a.png") != HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)) {
        Fail("save into a missing folder gave the wrong error", 0);
    }
    const std::wstring readOnly = dir + L"read only.png";
    WriteAll(readOnly, before);
    SetFileAttributesW(readOnly.c_str(), FILE_ATTRIBUTE_READONLY);
    if (SaveImage(Source{png}, readOnly) != HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) || ReadAll(readOnly) != before) {
        Fail("save over a read-only file didn't fail cleanly", 0);
    }
    SetFileAttributesW(readOnly.c_str(), FILE_ATTRIBUTE_NORMAL);
    const std::wstring locked = dir + L"locked.bmp";
    WriteAll(locked, before);
    const HANDLE other = CreateFileW(locked.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (SUCCEEDED(SaveImage(Source{png}, locked))) Fail("save over a file open elsewhere succeeded", 0);
    CloseHandle(other);
    if (ReadAll(locked) != before) Fail("failed save over a file open elsewhere changed it", 0);
    if (TempFiles(dir)) Fail("failed saves left temporary files", TempFiles(dir));

    // Unicode names, and names as long as they can be, where long paths are on.
    std::vector<std::wstring> names = {L"写真 \U0001F600.tif"};
    if (LongPathsEnabled()) names.push_back(std::wstring(251, L'x') + L".jpg");
    for (const std::wstring& name : names) {
        if (FAILED(SaveImage(Source{png}, dir + name))) Fail("save with an unusual name failed", int(name.size()));
    }

    // Temporary files another save left (by a crash, say) are skipped and kept; once a hundred are in the way,
    // saving fails rather than overwriting one.
    const std::wstring prefix = dir + L"~ImageVIEWER." + std::to_wstring(GetCurrentProcessId()) + L".";
    WriteAll(prefix + L"0.tmp", before);
    if (FAILED(SaveImage(Source{png}, dir + L"after crash.png")) || ReadAll(prefix + L"0.tmp") != before) {
        Fail("save didn't step past a leftover temporary file", 0);
    }
    for (int i = 1; i < 100; ++i) WriteAll(prefix + std::to_wstring(i) + L".tmp", before);
    if (SUCCEEDED(SaveImage(Source{png}, dir + L"blocked.png"))) Fail("save overwrote a leftover temporary file", 0);
    for (int i = 0; i < 100; ++i) {
        if (ReadAll(prefix + std::to_wstring(i) + L".tmp") != before) Fail("leftover temporary file changed", i);
        DeleteFileW((prefix + std::to_wstring(i) + L".tmp").c_str());
    }
    if (TempFiles(dir)) Fail("saves left temporary files", TempFiles(dir));
}

// The types Save As offers, the extensions each writes, and the images the viewer opens and steps through all
// agree: each offered extension, in any case, writes its type, and anything else writes PNG.
void TestSaveTypes() {
    std::mt19937 rng(71);
    const std::wstring dir = TempDir(L"save types");
    const std::wstring png = dir + L"source.bmp";
    EncodeFile(png, GUID_ContainerFormatBmp, RandomBitmap(rng, GUID_WICPixelFormat24bppBGR, 9, 7).Get());
    const struct {
        const wchar_t* label;
        const GUID& container;
    } kTypes[] = {{L"PNG", GUID_ContainerFormatPng}, {L"JPG", GUID_ContainerFormatJpeg}, {L"BMP", GUID_ContainerFormatBmp},
                  {L"GIF", GUID_ContainerFormatGif}, {L"TIFF", GUID_ContainerFormatTiff}};
    int types = 0;
    for (const wchar_t* p = kSaveFilter; *p; ++types) {
        const wchar_t* patterns = p + wcslen(p) + 1;
        const GUID* container = nullptr;
        for (const auto& type : kTypes) {
            if (wcscmp(p, type.label) == 0) container = &type.container;
        }
        if (!container) Fail("Save As offers a type the tests don't know", types);
        for (const wchar_t* q = patterns; *q;) {
            const size_t len = wcscspn(q, L";");
            const std::wstring ext(q + 1, len - 1);
            std::wstring upper = ext;
            for (wchar_t& c : upper) c = wchar_t(towupper(c));
            for (const std::wstring& e : {ext, upper}) {
                const std::wstring out = dir + L"t" + e;
                if (FAILED(SaveImage(Source{png}, out)) || (container && ContainerOf(out) != *container)) {
                    Fail("offered extension doesn't write its type", types);
                }
                DeleteFileW(out.c_str());
            }
            if (!IsImageName((L"x" + ext).c_str())) Fail("a type Save As writes isn't opened", types);
            q += q[len] ? len + 1 : len;
        }
        p = patterns + wcslen(patterns) + 1;
    }
    if (types != int(std::size(kTypes))) Fail("Save As offers the wrong number of types", types);

    CreateDirectoryW((dir + L"dot.folder").c_str(), nullptr);
    for (const wchar_t* name : {L"x.webp", L"x.heic", L"x.ico", L"x.jxr", L"x.xyz", L"x", L"x.", L"dot.folder\\x"}) {
        if (FAILED(SaveImage(Source{png}, dir + name)) || ContainerOf(dir + name) != GUID_ContainerFormatPng) {
            Fail("a type Windows can't write isn't saved as PNG", int(wcslen(name)));
        }
    }

    // The Open dialog's image filter lists exactly the extensions the viewer opens, in the same order.
    std::wstring want;
    for (const wchar_t* ext : kExtensions) want += (want.empty() ? L"*" : L";*") + std::wstring(ext);
    const wchar_t* filter = kFileFilter + wcslen(kFileFilter) + 1;
    if (want != filter) Fail("Open dialog filter doesn't match the extensions opened", 0);
}
