#pragma once

#include <windows.h>

// Waits for handle while handling this thread's messages, so its windows keep painting meanwhile, as they do
// under a modal dialog. Returns false if the app was asked to quit first, posting that again for the main loop.
inline bool PumpUntil(HANDLE handle) {
    while (MsgWaitForMultipleObjects(1, &handle, FALSE, INFINITE, QS_ALLINPUT) == WAIT_OBJECT_0 + 1) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                PostQuitMessage(int(msg.wParam));
                return false;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return true;
}
