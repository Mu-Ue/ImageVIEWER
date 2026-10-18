// Checks that what the viewer shows and saves is exact, and that the app around it behaves. Build with
// "cmake --build build --target tests" and run build\tests.exe; it prints each failure and exits with 1 if there
// was any. Everything runs on a hidden desktop against a fake clipboard, so nothing shows on screen and the
// user's clipboard is never touched.
#include "test.h"

#include <shellapi.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <iterator>

#include "imaging/decoder.h"

namespace {

int failures = 0;
std::atomic<const char*> running{""};  // the test group running, for failures and the watchdog
const WicFactory* factory = nullptr;
std::wstring desktop;

constexpr DWORD kTimeoutMs = 300000;  // a hung test fails the run rather than waiting for ever

// Stands in for the file dialog the viewer runs in a copy of its exe ("--dialog open|save <owner> <name>"),
// answering with IMAGEVIEWER_TEST_DIALOG: a path to write as the chosen one, nothing to cancel, or "?echo" to
// write back what it was asked as "kind|owner|name" ("?echo:<folder>" puts the folder first), "?long" for the
// longest path Windows allows, "?odd" for a partial character and "?nul" for a path with a null in it.
int FakeDialog() {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    wchar_t answer[40000];
    const DWORD length = GetEnvironmentVariableW(L"IMAGEVIEWER_TEST_DIALOG", answer, DWORD(std::size(answer)));
    if (!argv || argc < 5 || length == 0 || length >= std::size(answer)) return 1;
    std::wstring out = answer;
    if (out.starts_with(L"?echo")) {
        out = out.substr(out.size() > 5 ? 6 : 5) + argv[2] + L"|" + argv[3] + L"|" + argv[4];
    } else if (out == L"?long") {
        out = L"C:\\" + std::wstring(32764, L'x');
    } else if (out == L"?nul") {
        out = std::wstring(L"C:\\a\0b.png", 10);
    }
    DWORD written = 0;
    const DWORD bytes = out == L"?odd" ? 3 : DWORD(out.size() * sizeof(wchar_t));
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), out.data(), bytes, &written, nullptr);
    return 0;
}

// Makes the hidden desktop this thread's windows go on, which is never shown and never takes focus.
bool UseHiddenDesktop() {
    const std::wstring name = L"ImageVIEWER-tests-" + std::to_wstring(GetCurrentProcessId());
    const HDESK desk = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!desk || !SetThreadDesktop(desk)) return false;
    desktop = L"WinSta0\\" + name;
    return true;
}

void DeleteTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    const HANDLE find = FindFirstFileW((dir + L"*").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring path = dir + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                DeleteTree(path + L"\\");
            } else {
                SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(path.c_str());
            }
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    RemoveDirectoryW(dir.c_str());
}

}  // namespace

void Fail(const char* what, int value) {
    printf("FAIL [%s] %s (%d)\n", running.load(), what, value);
    ++failures;
}

IWICImagingFactory* Wic() {
    return factory->Get();
}

const std::wstring& TestDesktop() {
    return desktop;
}

int wmain(int argc, wchar_t** argv) {
    if (argc >= 2 && wcscmp(argv[1], L"--dialog") == 0) return FakeDialog();
    setvbuf(stdout, nullptr, _IONBF, 0);
    // WIC's JPEG decoder complains on stderr about the damaged files the tests feed it on purpose, through the
    // system's C runtime rather than this program's.
    if (const HMODULE crt = LoadLibraryExW(L"msvcrt.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        const auto open = reinterpret_cast<int (*)(const char*, int, ...)>(GetProcAddress(crt, "_open"));
        const auto dup2 = reinterpret_cast<int (*)(int, int)>(GetProcAddress(crt, "_dup2"));
        if (open && dup2) dup2(open("NUL", 1 /* _O_WRONLY */), 2);
    }
    if (!UseHiddenDesktop()) {
        printf("FAIL can't make a hidden desktop to test on (%lu)\n", GetLastError());
        return 1;
    }
    CreateThread(nullptr, 0, [](void*) -> DWORD {
        Sleep(kTimeoutMs);
        printf("FAIL [%s] timed out\n", running.load());
        DeleteTree(TempPath(L""));  // as much as the hung test doesn't hold open
        TerminateProcess(GetCurrentProcess(), 1);
        return 0;
    }, nullptr, 0, nullptr);
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;

    static constexpr struct {
        const char* name;
        void (*run)();
    } kTests[] = {
        {"scaling", TestScaling},
        {"scaling edges", TestScalingEdges},
        {"orientation", TestOrientation},
        {"formats", TestFormats},
        {"decode sources", TestDecodeSources},
        {"sniffing", TestSniffing},
        {"decode limits", TestDecodeLimits},
        {"bands", TestBands},
        {"icons", TestIcons},
        {"robustness", TestRobustness},
        {"concurrent decodes", TestConcurrentDecodes},
        {"animation disposal", TestAnimationDisposal},
        {"gif files", TestGifFiles},
        {"save", TestSave},
        {"bitmap source", TestBitmapSource},
        {"save matrix", TestSaveMatrix},
        {"save safety", TestSaveSafety},
        {"save types", TestSaveTypes},
        {"clipboard dib", TestClipboardDib},
        {"clipboard files", TestClipboardFiles},
        {"clipboard name", TestClipboardName},
        {"png length", TestPngLength},
        {"clipboard round trip", TestClipboardRoundTrip},
        {"viewer helpers", TestViewerHelpers},
        {"file dialog", TestFileDialog},
        {"neighbors", TestNeighbors},
        {"viewer", TestViewer},
        {"app", TestApp},
        {"leaks", TestLeaks},
    };
    {
        const WicFactory wic;
        if (FAILED(wic.hr)) return 1;
        factory = &wic;
        LARGE_INTEGER freq, start, end;
        QueryPerformanceFrequency(&freq);
        for (const auto& test : kTests) {
            // "tests.exe save viewer" runs only the groups whose names contain one of the words.
            bool chosen = argc < 2;
            for (int i = 1; i < argc && !chosen; ++i) {
                char word[64];
                WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, word, sizeof(word), nullptr, nullptr);
                chosen = strstr(test.name, word) != nullptr;
            }
            if (!chosen) continue;
            const int before = failures;
            running = test.name;
            QueryPerformanceCounter(&start);
            test.run();
            QueryPerformanceCounter(&end);
            printf("%-22s %s %6lld ms\n", test.name, failures == before ? "ok    " : "FAILED",
                   (end.QuadPart - start.QuadPart) * 1000 / freq.QuadPart);
        }
        factory = nullptr;
    }
    CoUninitialize();
    DeleteTree(TempPath(L""));
    printf(failures ? "%d failed\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
