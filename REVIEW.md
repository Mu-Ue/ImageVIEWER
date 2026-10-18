# ImageVIEWER review

A first-look review of the whole project (about 4.5k lines). Every finding below has a file:line plus a reproduced failure or a measurement. Anything without that is listed under Hypotheses.

**Hard constraints respected:** displayed pixels stay bit-exact, Windows codecs only, plain GDI.

## Baseline

- Clean Release build in a scratch folder with the repo's own CMake setup: **0 warnings** at `/W4 /permissive- /sdl`. The exe is 365,056 bytes.
- `tests.exe`: **all passed**, in 370 ms. Nothing was failing before this review.
- The working tree has one uncommitted change: `src/main.cpp` drops `ProcessStrictHandleCheckPolicy`. I agree with it (see Leave as is).
- Experiments ran as throwaway harnesses that linked the repo's sources unchanged. Machine: 16 logical cores, local SSD, warm cache.

## Optimization pass (applied, uncommitted)

After the review, every simple, bit-exact optimization was applied, each measured against the old build.

**How it was checked:**
- **Pixels:** a harness ran both builds on the same corpus: JPEG color/gray/rotated, 24 MP and 1080p; PNG RGB/alpha/palette/16-bit; still and animated GIF (offsets, every disposal, local palettes); BMP; TIFF; JXR; ICO. It compared hashes of:
  - the decoded pixels,
  - `DrawScaled` output at 5 zooms,
  - every animation frame over two loops,
  - Copy (PNG and DIB),
  - Save As to PNG/JPG/BMP/GIF/TIFF.
- **Result:** identical everywhere, except one intended fix (below).
- **Not covered:** HEIC/WebP/AVIF, because this machine has no encoders to generate test files. Their code path only gained a container query.

**Kept:**

| Change | Where | Measured (old → new) |
|---|---|---|
| Copy/Save As read the shown bitmap when it is exact, instead of decoding the file again | `viewer.cpp` `ShownSource`, `decoder.cpp` `OpenEncoded` | Copy 24 MP JPEG 574 → 513 ms; Save as BMP 174 → 154 ms; Save as PNG 523 → 504 ms |
| Copy decodes a non-exact source once for both PNG and DIB; GIF Save As reads it once instead of three times | `encoder.cpp` `Buffer` | Copy rgb.png 44 → 36 ms, palette.png 7.7 → 6.6 ms, 1080p JPEG 48 → 46 ms; alpha.png +0.2 ms (too cheap to decode for buffering to pay) |
| Composite over white only when the source can actually be transparent (an opaque palette no longer counts) | `encoder.cpp` `Encode` | skips a full 32bpp copy when saving opaque palette images as JPEG |
| Color JPEGs skip counting colors and decode straight into the 24bpp bitmap | `decoder.cpp` `DecodeImage` | open 24 MP 42.0 → 40.1 ms, 1080p 4.0 → 3.7 ms |
| GIF frames share palettes, and frame objects are released after reading | `animation.h/.cpp` | 20,000-frame GIF: memory kept 22.8 → **3.4 MB**, peak 81 → 61 MB, time unchanged (746 → 740 ms) |
| A GIF whose frames fail is no longer re-read (was C2) | `viewer.cpp` `RunDecoder`, `OnFrames` | 10 steps that find nothing next to such a GIF: **4,172 → 0 ms CPU** |
| File size read on the decoding thread, not the UI thread | `viewer.cpp` `FileSize` | UI thread no longer waits on a slow share |
| Pasted PNG trimmed to `IEND`, and pasted streams sized exactly | `clipboard.cpp` `PngLength`, `CopyImageData` | no slack bytes saved or counted in the status-bar size |

**Fixed along the way:** saving a *gray* image as BMP or GIF changed its pixels. WIC built the palette from the pixels and didn't keep all 256 levels. `ToIndexed` now uses the fixed 256-gray palette, so these saves match the lossless PNG save. There's a test for it.

**Measured and dropped:**

| Idea | Result |
|---|---|
| Other PNG filter for Copy | WIC's default is already the fastest (525 ms, against 581–1056 ms for each explicit filter) |
| Cache the folder listing for stepping | Not safe. The folder's write time stayed unchanged after 471 of 600 back-to-back changes, and change notifications didn't arrive in testing, so the step order would go stale. Sort keys can't replace `CompareStringEx` either: 12 of 4.5M pairs order differently. |
| Faster `IsImageName` | 0.3 ms per 10,000 names. The faster version would stop matching extensions like `.tıf` that the Unicode ignore-case comparison accepts. |
| Reuse `DrawScaled`'s buffers | Allocation is 3–4% of a pan strip's draw time (0.003 of 0.097 ms) |
| Pass the decoder thread's WIC factory into `DecodeImage` | 4 µs per image |
| Delayed clipboard rendering | Only moves the PNG encode to paste time, and freezes the viewer while the other program waits |

**No regressions:**
- Startup: 60–73 ms launch to image shown, both builds.
- Memory: 7.4 MB private, 19 DLLs, no WIC loaded after the startup decode, both builds.
- `tests.exe` passes, with new tests for the IEND trim and the gray save.
- Build: 0 warnings.
- Hidden-desktop smoke test of the real viewer passes: open, status-bar size, stepping and wrap-around, GIF playback, a GIF whose frames fail.

## Startup pass (applied, uncommitted)

**How it was measured:** timestamps from process creation onward, in a scratch copy, on a hidden desktop.
- The ~70 ms I first reported was my launcher's polling. The viewer itself reached its first paint in 12–48 ms.
- Before this pass, about 3 ms of every first decode went into WIC searching its registry: 1.6–1.9 ms to find the decoder, plus 1–1.3 ms for an orientation query that could never succeed.

**Kept:**

| Change | Where | Effect |
|---|---|---|
| Built-in decoders (JPEG, PNG, GIF, BMP, TIFF, ICO, JPEG XR) are created directly from `windowscodecs.dll` by CLSID. HEIF/WebP keep WIC's lookup. | `decoder.cpp` `CreateBuiltInDecoder` | creating the decoder: 1.6–1.9 → 0.05 ms |
| Only the orientation query that can succeed: JPEG `/app1/ifd`, TIFF `/ifd`. None for PNG/GIF/BMP, where WIC reads no orientation; on this Windows build it ignores even a PNG `eXIf` chunk. | `decoder.cpp` `ReadOrientation` | PNG −1.3 ms, JPEG without EXIF −1.1 ms, TIFF −0.2 ms |
| Large color JPEGs (≥1 MP, interleaved, not scaled down) decode in up to 4 bands on separate cores, each with its own decoder; later bands are shorter, since each first reads past the data above it. Progressive JPEGs keep one decoder, since every band would have to decode all of it. | `decoder.cpp` `DecodeBands` | open 2–2.2× faster (table below) |

**Open time** (`DecodeImage`, warm, best of 7), last commit → now. Every image has identical pixels:

| Image | Last commit | Now |
|---|---|---|
| 1080p | 5.1 ms | 3.3 ms |
| 12 MP | 13.9 ms | 6.8 ms |
| 12 MP rotated (EXIF 6) | 32.1 ms | 15.8 ms |
| Windows wallpapers, 3840×2400 | 21–26 ms | 11–13 ms |
| 6400×4000 | 44.1 ms | 21.1 ms |
| Progressive 2736×1539 (no bands) | 27.6 ms | 20.8 ms |

BMP, GIF, ICO, JPEG XR, PNG and TIFF samples, and all 12 played frames of an animated GIF, are also identical. `tests.exe` now includes a 1 MP JPEG in all 8 orientations, which goes through the band path.

**Launch to first paint** (median of 15, last commit → now):

| Image | Last commit | Now |
|---|---|---|
| PNG 886×643 | 13.3 ms | 9.4 ms |
| JPEG 1080p | 18.7 ms | 13.7 ms |
| JPEG 12 MP | 30.9 ms | 20.0 ms |
| JPEG 12 MP rotated | 48.5 ms | 30.9 ms |
| Wallpaper 3840×2400 | 44.1 ms | 26.9 ms |

**Measured and dropped:**

| Idea | Result |
|---|---|
| Delay-loading dwmapi, imm32 and COM, alone or combined with preloading `windowscodecs.dll` on the UI thread | gains under 0.4 ms before `wWinMain`, lost again to loader-lock contention; first paint within noise |
| Warming the drawing thread pool | the second paint costs the same as the first |
| A single `CopyPixels` call instead of strips | 4.11 vs 4.09 ms |
| More than 4 bands | the last band mostly reads past the rows above it; 4 bands get most of the gain, at less extra CPU |

**What's left:**
- About 4 ms of process start and loading system DLLs. A test program with the same imports needs 3.5 ms.
- About 1 ms to start the decode thread and COM, and 0.5 ms to load WIC.
- WIC's own decoding.
- About 1 ms in `ShowWindow`, plus the first paint.

Showing the window before the image is decoded would hide 1–15 ms more, but it would flash an empty window first, so I left it.

## Startup without an image (applied, uncommitted)

**Where the time went** (median of 15 runs, from process creation, launched with no file):

| Step | Time |
|---|---|
| Process start and system DLLs, until `wWinMain` | 4.4 ms |
| First USER call on the UI thread (Windows sets it up for windows) | 0.5–0.9 ms |
| `CreateWindowExW` with both bars | 1.5 ms |
| `PlaceWindow` | 0.3 ms |
| `ShowWindow` | 0.8 ms |
| Paints, and Windows' own gaps between the three windows' paints | 0.6 ms |
| **Measuring the first chevron**: Windows reading the Segoe MDL2 icon font | **1.1 ms** |
| Everything painted | 10.1 ms |

**Kept:**

| Change | Where | Effect |
|---|---|---|
| A short thread, started first thing in `StartViewer`, has Windows read the icon font with GDI calls only, while the UI thread builds the window. The font is cached process-wide, so the status bar's first paint finds it ready. No USER call, so the thread skips the GUI-thread setup that would cost about as much again. | `status_bar.cpp` `PreloadStatusIcons` | everything painted 10.1 → **8.8 ms**; the viewer's own share 5.6 → 4.3 ms. It also helps when an image is opened, since the status bar paints then too. |

**Measured and dropped:**

| Idea | Result |
|---|---|
| Delay-loading COM | `combase`, `RPCRT4` and `msvcrt` load anyway, because system DLLs the viewer needs import them |
| Warming the menu font on the helper thread | the UI thread's menu-font step doesn't get faster; its cost is per size and DPI, not the file read |
| Pre-loading `uxtheme.dll` on the helper thread | within noise |
| Asking about High Contrast earlier or elsewhere | the query itself takes 0.01 ms; the time belongs to the thread's first USER call, which the UI thread has to pay anyway |

**What's left:** process start, Windows setting up the UI thread, creating and showing the windows, and Windows' own work between the three windows' paints. Avoiding that last part would mean drawing the bars inside the main window instead of as their own windows. That would be a large change, and it would break what screen readers read out, so I left it.

## 1. Critical: bugs, undefined behavior, security

No undefined behavior or security hole survived verification. The one security candidate was disproved; it is listed at the end of this section so it isn't chased again.

### C1. GIFs are shown and copied at the frame's size, not the canvas size
`src/imaging/decoder.cpp:335-336` (`GetFrame(0)` → `GetSize`)

- **Why it happens:** WIC reports a GIF frame's own image-descriptor size and ignores its offset. `animation.cpp:93,105-107` already relies on this for animations, but the still-image path doesn't.
- **Reproduced:** I built a GIF with a 200×200 logical screen and one 100×100 red frame at (50,50).
  - Single frame: `DecodeImage` shows **100×100, all red**. Browsers show 200×200 with the red square offset by 50 px.
  - Two frames: the viewer correctly shows the 200×200 canvas, but **Copy produces a 100×100 PNG** (`EncodeClipboardImage` re-reads raw frame 0). Save As to another format does the same, so you don't get what is on screen.
- **Fix:** in `OpenDecoded`, when the container is GIF and frame 0's size or offset differs from `/logscrdesc`, copy frame 0 into a zeroed 32bppBGRA `IWICBitmap` of the logical-screen size at its `/imgdesc` offset, and use that as the source.
  - The display composites the transparent padding over black, the same as the animation canvas (`ShowFrame` fills with `kBackground`).
  - Copy and Save keep the alpha.
  - Normal GIFs, where frame 0 covers the screen, take the existing path and stay bit-identical.
- **Risk:** low. **Effort:** S–M (about 25 lines). The repro above becomes the test.

### C2. A GIF whose frames fail to load stays "awaiting frames" forever, and gets re-read again and again (fixed)
`src/ui/viewer.cpp:456-462`, re-triggered at `:514` and `:731`

- **Why it happens:** when `DecodeAnimation` fails, `RunDecoder` deletes the `Frames` and posts nothing. `g.awaitingFrames` stays `true` until another image replaces this one.
- **Scenario:** open a GIF whose frames exceed `kMaxAnimationBytes` (1 GB of indices) or that has more than 65,536 frames.
  1. The first frame shows.
  2. Reading the frames fails without a word.
  3. After that, every failed open or paste (`:731`), and every step that finds nothing (`CancelDecode`, `:514`), restarts `RequestFrames`. Each restart walks the whole file again on the decoder thread, only to fail the same way.
- **Fix:** on failure, still post a `Frames` with an empty animation. In `OnFrames`, clear `awaitingFrames` when the frames are empty.
- **Risk:** low. **Effort:** S.

### Disproved: BMP linked color profiles
I suspected that viewing a BMP whose V5 header links its profile to a UNC path would make WIC open that path, leaking NTLM credentials. Commit 7fbafa8 only neutralizes this for pasted DIBs.

- **Test:** a BMP linking to `\\.\pipe\<name>`, with a pipe server listening, run through decode, `CopyPixels`, `DecodeImage` and `SaveImage`.
- **Result:** zero connections. WIC's BMP decoder doesn't support color contexts at all: `GetColorContexts` returns `0x88982F81` (`WINCODEC_ERR_UNSUPPORTEDOPERATION`), even for plain BMPs.
- **Consequence:** files on disk are safe. The rewrite at `clipboard.cpp:81-89` is still worth keeping, but for a different reason than its comment gives: a pasted DIB saved as .bmp is copied byte for byte, so the rewrite stops other programs from following the link later. Correct the comment.

## 2. High-impact

### D1. Color management on display (a product decision, not a bug fix)
Profiles are carried through Save As (`encoder.cpp:196-200`) but never applied on screen (`clipboard.cpp:81` comment, `decoder.cpp:625`).

- **Effect:** wide-gamut photos show desaturated. That includes Display P3, the default for iPhone HEIC and many Android JPEGs, and Adobe RGB. CMYK JPEGs get a naive conversion.
- **Option:** run only *tagged, non-sRGB* frames through `IWICColorTransform` to sRGB before the PBGRA conversion.
  - Untagged and sRGB-tagged images stay bit-identical, so the bit-exact rule holds for everything it covers today.
  - On Windows 11 with Auto Color Management, sRGB is exactly what DWM expects from a GDI app.
- **Trade-offs:**
  - Tagged images get different pixels. They are already `exact=false`, so nothing that relies on `exact` breaks.
  - Decoding those images is slower.
  - More distinct colors, so the palette path is used less and those images take more memory.
  - Broken profiles need a fallback to today's behavior.
- **When not to:** if the viewer is meant to show stored values, for example for pixel inspection.
- **Impact:** L for phone photos, none for screenshots and web graphics. **Effort:** M. **Risk:** M.

Nothing else reaches "high impact". The performance hypotheses I measured all came out small (see section 4), which supports the existing design.

## 3. Medium and nice-to-have

- (done) `viewer.cpp:351`: `GetFileAttributesExW` runs on the UI thread for every image shown, and can stall on a slow network share. The decoder thread already has the handle open; return the size in `Decode::bytes`.
- (done) `clipboard.cpp:56,75`, `encoder.cpp:296`: the pasted PNG is sized with `GlobalSize`, which may be larger than the data. A same-format Save As writes those slack bytes from the other process's allocation after `IEND`, and the status bar size includes them (`:210`). Trim to the end of the `IEND` chunk.
- (done) `animation.h:18`: every `Frame` carries its own 1 KB palette, even when all frames share the global color table. **Measured:** a 20,000-frame GIF keeps 23 MB, about 21 MB of it palette copies. Store an index into a list of deduplicated palettes instead.
- (done) `animation.cpp:81`: all `IWICBitmapFrameDecode`s stay alive until the pixel copy. **Measured:** 83 MB peak against 23 MB kept for 20,000 frames, so about 3 KB per frame, or about 200 MB at the 65,536-frame cap. Calling `GetFrame(i)` again in the copy loop drops that peak. Only worth it for very long GIFs.
- `encoder.cpp:304-305`: Save As re-encode opens and parses the source twice (`OpenSource`, then `ReadColorProfiles`). Return the profiles from the frame `OpenSource` already has. This is simplification only: the shared-stream hazard I suspected was **disproved** (pasted alpha PNG → BMP/TIFF/PNG gave identical pixels).
- `viewer.cpp:369-377,616,1022`: `std::thread` throws if the thread can't be created, which ends the process through `terminate`. `Request` (`:482-496`) already handles a `CreateThread` failure. Make them consistent.
- `viewer.cpp:909-913`: swallowing every `SC_KEYMENU` also removes Alt+Space (keyboard Move/Size/Minimize), which hurts accessibility. Letting `lParam == ' '` through fixes it, but the system menu is light-themed, which conflicts with the dark-UI uniformity rule. Your call.
- `status_bar.cpp:60-70`: the ‹ › buttons are painted, not real controls, so screen readers can't find them. The same commands are reachable through the View menu and the arrow keys.
- `clipboard.cpp:229-245`: reports "Can't copy" when PNG was placed but DIB failed.
- `encoder.cpp:14-24`: a crash mid-save leaves `~ImageVIEWER.<pid>.<n>.tmp` next to the target. Set `FileDispositionInfo` to delete right after `CREATE_NEW`, and clear it just before `RenameTo`.
- `encoder.cpp:285-317`: replace-by-rename drops the old file's ACL, hard links, alternate streams and creation time. That is the right price for atomic saves; document it rather than change it.
- `viewer.cpp:621-632` and `file_dialog.cpp:122-135`: the same wait-and-pump loop with `WM_QUIT` re-post, written twice. Share one helper.
- `config.h:9-19` and `encoder.cpp:41-50`: four hand-maintained extension lists (`kExtensions`, `kFileFilter`, `kSaveFilter`, `ContainerFor`). Add a test that checks they agree.
- (done) `encoder.cpp:123-179`: GIF Save As reads the source three times (palette, indexed, alpha). It's a rare path; leave it unless it gets touched.
- **Test gaps**, by value:
  1. The C1 geometry test.
  2. `SniffFormat` table.
  3. `ReadAnimation`/`ReadPlays` on generated GIFs (the hand-written LZW trick from the C1 repro makes this easy).
  4. Save round trips for every container. All five pass today (PNG/JPG/BMP/GIF/TIFF, opaque and alpha), so lock that in.
  5. `Neighbors` ordering and wrap-around (pull it out as a pure function over names).
  6. ICO frame choice.
  7. Gray and indexed direct decode.
  8. The >16384 scaled path.
  9. The decoder worker's latest-wins and skip logic.

  Also register `tests` with `add_test` so `ctest` runs it.

## 4. Hypotheses: measured, and still open

| # | Hypothesis | Result | Decision |
|---|---|---|---|
| H1 | `decoder.cpp:323-326` `FreeLibrary(windowscodecs)` skips `DllCanUnloadNow` | After one factory create/release, WIC reports `S_FALSE` **for good**. Gating on it would mean never unloading, and `CoUninitialize` frees DLLs unconditionally anyway. | Leave as is |
| H3 | `viewer.cpp:552-580` lists the whole folder on the UI thread for every arrow press | 10k files: **4.2 ms**. 50k files: **30.6 ms** (local SSD, warm) | Leave as is. Revisit only for 50k+ folders or network shares (not measured) |
| H4 | Copy/Save from the exact bitmap instead of re-decoding the file | 24 MP JPEG Copy: 846 ms from file vs 791 ms from bitmap, pixels identical. Decoding is only 45 ms; PNG encoding dominates. | Applied in the optimization pass anyway (small but free) |
| H6 | Save As of a pasted stream reads one `IStream` from two decoders | Identical pixels for BMP, TIFF and PNG output | Disproved |
| H8 | The write-only temp handle breaks encoders that read back | All five containers save correctly | Disproved |
| H9 | Full `DrawScaled` on every zoom notch or resize | 6000×4000 → 3240×2160: **10.7 ms**; → 1620×1080: **4.0 ms** | Leave as is (check on a 4-core machine) |

Still open:

- **H2** (`decoder.cpp:17-30,364`): HEIC/AVIF may be rotated twice, if WIC's HEIF decoder already applies `irot` and EXIF tag 274 is then applied on top.
  - Confidence: medium-low.
  - To settle: open an iPhone portrait HEIC and compare against Photos.
- **H7** (`file_dialog.cpp:80`): the anonymous pipe's size is only a hint. If a path is ever larger than the real buffer, the helper's `WriteFile` blocks while the viewer waits for the helper to exit.
  - Confidence: very low.
  - To settle: return a ~32k-character path from the helper.
- **H10** (`encoder.cpp:62-81`): Copy spends most of its 846 ms in the PNG encoder. Setting the WIC PNG `FilterOption` (for example `WICPngFilterNone` or `Sub`) is still lossless and could make it markedly faster, at some cost in clipboard size.
  - To settle: time Copy and measure the PNG size for each filter on a 24 MP photo and a screenshot.

## 5. Leave as is

- **Content sniffing, with one decoder per format** (`decoder.cpp:216-275`). Data never reaches a codec for a different format, and misnamed files still open. This is the right attack-surface trade.
- **`OpenFileStream`** (`decoder.cpp:240-249`). It requires `FILE_TYPE_DISK`, opens with `SECURITY_IDENTIFICATION`, and reads through the checked handle. That blocks devices, pipes and impersonation, and avoids a TOCTOU.
- **Clipboard parsers.** Strict, bounded and tested. The bound in the comment at `clipboard.cpp:45` checks out.
- **Save via a temp file in the target folder, renamed and deleted through one handle.** Atomic, safe against a file or link being swapped in, and it lets you overwrite the source.
- **Exact box-filter scaler** (`scale.cpp`). Integer weights, exact rounding, tested against a slow reference.
  - I checked the bounds: the SSE path only runs when the weight is ≤ 0xFFFF, which always holds when shrinking; when enlarging, the weights are ≤ the source size. Row sums stay below 2^22.
  - Measured above at 4–11 ms per full repaint. No cache needed.
- **Smallest exact DIB** (palette or 24bpp, direct decode into the bitmap when possible). Tested across all 8 orientations and the mid-image switch from palette to 24bpp.
- **Latest-wins decoder worker** (`viewer.cpp:401-497`). I traced replace, cancel, idle exit versus a new request, and thread creation failure. No result is lost, shown twice, or shown stale.
- **File dialogs in a helper process.** Kill-on-close job, inherit-only-the-pipe handle list, suspended start. It keeps about 50 shell DLLs out of the viewer.
- **Process mitigations**, and the uncommitted removal of strict handle checks. With `HandleExceptionsPermanentlyEnabled`, a double close in a Store codec or GPU driver would kill the viewer, and the security gain for a viewer is small.
- **Animation and panning.** Pans paint just before DWM's vblank via `ScrollWindowEx`. GIF frames are timed from when they were due, which removes tick drift. Delay ≤ 1 → 100 ms and the loop count match browsers. Animation pauses while minimized.
- **No logging framework.** Error dialogs give a reason, or else the HRESULT, which is enough for a viewer.
- **Build.** 365 KB static-CRT exe, explicit import list, delay-loaded comdlg32/shell32, CFG/CET/EHCONT, `/DEPENDENTLOADFLAG`.

## 6. Regressions the proposed changes could cause

- **C1:** offset GIFs change shown size, status-bar resolution and Copy/Save output (intended). The padding shows black on screen. Make sure normal GIFs still take the old path; otherwise memory and `exact` change for them.
- **C2:** none expected. Check that a later successful frames request for the *same* image still plays.
- **D1:** different pixels for tagged images. Slower decode and more memory for them. Must fall back cleanly when the transform fails.
- **Palette dedupe and the frame-object change:** `ShowFrame` colors must stay identical; the existing disposal test plus a generated multi-palette GIF covers it.
- **Moving the file-size read** to the decoder thread: the status text must still match the file shown. Read it from the same handle.
- **Alt+Space:** the light system menu breaks dark-UI uniformity.
- **Temp-file delete-on-close:** if clearing the disposition fails, the rename must not go ahead.
- **Every change:** run the old-vs-new pixel diff across JPEG, PNG, GIF, BMP, TIFF, WebP, ICO, HEIC/AVIF and JXR.

## 7. Roadmap (value ÷ cost)

1. ~~C2 stuck animation request~~, ~~GlobalSize trim~~, ~~file size off the UI thread~~: done.
2. **C1** GIF logical screen (S–M), with its repro as the test.
3. **Test gaps** (M): sniffing, `ReadAnimation`, save round trips, `Neighbors`. Cheap insurance for the parts no test covers.
4. **H2** HEIC orientation check (S, needs a sample), then fix if confirmed.
5. **D1** color management: decide first, then M effort.
6. ~~H10 PNG filter for Copy~~: measured, the default is already fastest.
7. The remaining medium items, as the code around them is touched.
