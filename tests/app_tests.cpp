// The real ImageVIEWER.exe, run on the hidden desktop in a job that ends it with the tests: what it shows, what it
// keeps loaded, and how it exits.
#include "test.h"

#include <psapi.h>

#include <iterator>

namespace {

// Ends every process started here when the tests end, however they end.
HANDLE Job() {
    static const HANDLE job = [] {
        const HANDLE h = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(h, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        return h;
    }();
    return job;
}

std::wstring AppPath() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring path = exe;
    return path.substr(0, path.find_last_of(L'\\') + 1) + L"ImageVIEWER.exe";
}

struct App {
    PROCESS_INFORMATION pi{};

    // Starts the viewer with args after its quoted path, in dir if given.
    explicit App(const std::wstring& args, const std::wstring& dir = L"") {
        std::wstring cmd = L"\"" + AppPath() + L"\" " + args;
        std::wstring desktop = TestDesktop();
        STARTUPINFOW si{sizeof(si)};
        si.lpDesktop = desktop.data();
        if (!CreateProcessW(AppPath().c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr,
                            dir.empty() ? nullptr : dir.c_str(), &si, &pi)) {
            Fail("can't start the viewer", int(GetLastError()));
            return;
        }
        AssignProcessToJobObject(Job(), pi.hProcess);
        ResumeThread(pi.hThread);
    }
    ~App() {
        if (!pi.hProcess) return;
        if (WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }

    // The visible top-level window of the viewer of the given class, waiting for it up to timeoutMs.
    HWND Window(const wchar_t* className, DWORD timeoutMs = 10000) const {
        struct Find {
            DWORD pid;
            const wchar_t* className;
            HWND found;
        } find{pi.dwProcessId, className, nullptr};
        for (const ULONGLONG end = GetTickCount64() + timeoutMs; pi.hProcess && GetTickCount64() < end; Sleep(10)) {
            EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
                auto& f = *reinterpret_cast<Find*>(lp);
                DWORD pid = 0;
                wchar_t name[64];
                GetWindowThreadProcessId(hwnd, &pid);
                if (pid == f.pid && IsWindowVisible(hwnd) && GetClassNameW(hwnd, name, int(std::size(name))) &&
                    wcscmp(name, f.className) == 0) {
                    f.found = hwnd;
                    return FALSE;
                }
                return TRUE;
            }, LPARAM(&find));
            if (find.found) return find.found;
        }
        return nullptr;
    }
    HWND Main() const { return Window(L"ImageVIEWER"); }

    // The exit code once it has exited, or -1 if it is still running after timeoutMs.
    long ExitCode(DWORD timeoutMs = 5000) const {
        DWORD code = 0;
        if (!pi.hProcess || WaitForSingleObject(pi.hProcess, timeoutMs) != WAIT_OBJECT_0) return -1;
        GetExitCodeProcess(pi.hProcess, &code);
        return long(code);
    }

    bool Loaded(const wchar_t* dll) const {
        HMODULE modules[1024];
        DWORD needed = 0;
        if (!EnumProcessModulesEx(pi.hProcess, modules, sizeof(modules), &needed, LIST_MODULES_ALL)) return false;
        for (DWORD i = 0; i < needed / sizeof(HMODULE); ++i) {
            wchar_t name[MAX_PATH];
            if (GetModuleBaseNameW(pi.hProcess, modules[i], name, MAX_PATH) && _wcsicmp(name, dll) == 0) return true;
        }
        return false;
    }
};

std::wstring WindowText(HWND hwnd) {
    wchar_t text[1024] = L"";
    GetWindowTextW(hwnd, text, int(std::size(text)));
    return text;
}

std::wstring StatusOf(HWND main) {
    return WindowText(FindWindowExW(main, nullptr, L"ImageVIEWERStatus", nullptr));
}

template <class Done>
bool WaitFor(Done done, DWORD timeoutMs = 10000) {
    for (const ULONGLONG end = GetTickCount64() + timeoutMs; GetTickCount64() < end; Sleep(10)) {
        if (done()) return true;
    }
    return done();
}

// The text of the message box the viewer shows, which is then closed.
std::wstring TakeBox(const App& app) {
    const HWND box = app.Window(L"#32770");
    if (!box) return L"";
    wchar_t text[1024] = L"";
    GetDlgItemTextW(box, 0xFFFF, text, int(std::size(text)));
    PostMessageW(box, WM_CLOSE, 0, 0);
    return text;
}

}  // namespace

// The app as Explorer starts it: it opens the image named on its command line, shows its info, steps through the
// folder, loads the codecs only while it decodes, explains what it can't open and exits with an error for it,
// and closes on Esc.
void TestApp() {
    if (GetFileAttributesW(AppPath().c_str()) == INVALID_FILE_ATTRIBUTES) {
        Fail("ImageVIEWER.exe isn't next to the tests", 0);
        return;
    }
    std::mt19937 rng(83);
    const std::wstring dir = TempDir(L"app");
    const std::wstring a = dir + L"a.png", b = dir + L"b.jpg", broken = dir + L"broken.png", big = dir + L"big.jpg";
    EncodeFile(a, GUID_ContainerFormatPng, RandomBitmap(rng, GUID_WICPixelFormat24bppBGR, 64, 48, 8).Get());
    EncodeFile(b, GUID_ContainerFormatJpeg, RandomBitmap(rng, GUID_WICPixelFormat24bppBGR, 80, 60).Get());
    WriteAll(broken, {'n', 'o', 'p', 'e'});
    const std::wstring photos = TempDir(L"app photos");
    EncodeFile(photos + L"1.jpg", GUID_ContainerFormatJpeg, RandomBitmap(rng, GUID_WICPixelFormat24bppBGR, 4000, 3000).Get());
    CopyFileW((photos + L"1.jpg").c_str(), (photos + L"2.jpg").c_str(), FALSE);

    // DLLs a shown image must not keep loaded: WIC and the COM catalog stay off the UI thread, and the file
    // dialog's DLLs stay in its helper process.
    const wchar_t* kNeverIdle[] = {L"windowscodecs.dll", L"clbcatq.dll", L"ole32.dll", L"oleaut32.dll",
                                   L"comdlg32.dll", L"shell32.dll"};
    {
        App app(L"\"" + a + L"\"");
        const HWND main = app.Main();
        if (!main) {
            Fail("viewer window didn't appear", 0);
        } else {
            if (WindowText(main) != L"a.png") Fail("viewer not named after its image", 0);
            if (StatusOf(main) != L"64 × 48, " + FormatSizeForTests(ReadAll(a).size()) + L", 100%") {
                Fail("viewer status bar shows the wrong info", 0);
            }
            for (const wchar_t* dll : kNeverIdle) {
                if (app.Loaded(dll)) Fail("DLL loaded with an image shown", int(wcslen(dll)));
            }
            PostMessageW(main, WM_KEYDOWN, VK_RIGHT, 0);
            if (!WaitFor([&] { return WindowText(main) == L"b.jpg"; })) Fail("viewer didn't step to the next image", 0);
            if (!app.Loaded(L"windowscodecs.dll")) Fail("decoder thread doesn't hold WIC while stepping", 0);
            if (app.Loaded(L"clbcatq.dll")) Fail("decoding loaded the COM catalog", 0);
            // Idle for a while, the decoder thread ends and unloads WIC.
            if (!WaitFor([&] { return !app.Loaded(L"windowscodecs.dll"); }, 16000)) Fail("WIC stays loaded when idle", 0);
            PostMessageW(main, WM_KEYDOWN, VK_RIGHT, 0);  // wraps around
            if (!WaitFor([&] { return WindowText(main) == L"a.png"; })) Fail("viewer didn't wrap around", 0);
            PostMessageW(main, WM_KEYDOWN, VK_ESCAPE, 0);
            if (app.ExitCode() != 0) Fail("viewer didn't exit cleanly on Esc", 0);
        }
    }

    // Started without an image, it shows an empty window.
    {
        App app(L"");
        const HWND main = app.Main();
        if (!main || WindowText(main) != L"ImageVIEWER" || StatusOf(main) != L"No file loaded") {
            Fail("empty viewer window wrong", 0);
        }
        if (app.Loaded(L"windowscodecs.dll")) Fail("WIC loaded without an image", 0);
        PostMessageW(main, WM_KEYDOWN, VK_ESCAPE, 0);
        if (app.ExitCode() != 0) Fail("empty viewer didn't exit cleanly", 0);
    }

    // An image it can't open is explained, and it exits with an error.
    const struct {
        std::wstring path;
        const wchar_t* text;
    } kErrors[] = {
        {broken, L"Can't open \"broken.png\".\n\nIt isn't a supported image."},
        {dir + L"missing.png", L"Can't open \"missing.png\".\n\nThe file doesn't exist."},
    };
    for (int i = 0; i < int(std::size(kErrors)); ++i) {
        App app(L"\"" + kErrors[i].path + L"\"");
        if (TakeBox(app) != kErrors[i].text) Fail("viewer explained a bad image wrong", i);
        if (app.ExitCode() != 1) Fail("viewer didn't exit with an error for a bad image", i);
    }

    // A relative or unquoted name, with spaces around it, is found in the current folder, which it steps through.
    for (const std::wstring& args : {std::wstring(L"a.png"), std::wstring(L"  \"a.png\"  ")}) {
        App app(args, dir);
        const HWND main = app.Main();
        if (!main || WindowText(main) != L"a.png") {
            Fail("viewer didn't open a relative path", int(args.size()));
            continue;
        }
        PostMessageW(main, WM_KEYDOWN, VK_LEFT, 0);
        if (!WaitFor([&] { return WindowText(main) == L"b.jpg"; })) Fail("viewer didn't step from a relative path", 0);
        PostMessageW(main, WM_KEYDOWN, VK_ESCAPE, 0);
        if (app.ExitCode() != 0) Fail("viewer didn't exit cleanly", 0);
    }

    // Full screen covers the monitor; Esc first leaves it, then closes.
    {
        App app(L"\"" + a + L"\"");
        const HWND main = app.Main();
        RECT before{}, after{};
        GetWindowRect(main, &before);
        PostMessageW(main, WM_KEYDOWN, VK_F11, 0);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(main, MONITOR_DEFAULTTONEAREST), &mi);
        if (!WaitFor([&] { return GetWindowRect(main, &after) && EqualRect(&after, &mi.rcMonitor); })) {
            Fail("viewer didn't go full screen", 0);
        }
        PostMessageW(main, WM_KEYDOWN, VK_ESCAPE, 0);
        if (!WaitFor([&] { return GetWindowRect(main, &after) && EqualRect(&after, &before); })) {
            Fail("viewer didn't leave full screen", 0);
        }
        if (app.ExitCode(300) != -1) Fail("Esc in full screen closed the viewer", 0);
        PostMessageW(main, WM_KEYDOWN, VK_ESCAPE, 0);
        if (app.ExitCode() != 0) Fail("viewer didn't close after full screen", 0);
    }

    // Closing while a large image decodes exits at once and cleanly.
    {
        App app(L"\"" + photos + L"1.jpg\"");
        const HWND main = app.Main();
        PostMessageW(main, WM_KEYDOWN, VK_RIGHT, 0);
        PostMessageW(main, WM_KEYDOWN, VK_ESCAPE, 0);
        if (!main || app.ExitCode() != 0) Fail("viewer didn't exit cleanly while decoding", 0);
    }
}
