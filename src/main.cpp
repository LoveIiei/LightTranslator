#include <windows.h>
#include <ole2.h>
#include <wil/resource.h>

#include "app/App.h"
#include "app/AppInfo.h"
#include "app/Messages.h"

int APIENTRY wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int)
{
    // Single-instance guard. A second launch asks the first instance to open Settings and exits.
    const HANDLE mutexHandle = CreateMutexW(nullptr, FALSE, AppInfo::kMutexName);
    const DWORD mutexError = GetLastError(); // read before anything else can overwrite it
    wil::unique_mutex_nothrow instanceMutex(mutexHandle);
    if (mutexError == ERROR_ALREADY_EXISTS || !instanceMutex) {
        if (const HWND existing = FindWindowW(AppInfo::kWindowClass, nullptr)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(existing, &pid);
            AllowSetForegroundWindow(pid);
            PostMessageW(existing, WM_APP_OPEN_SETTINGS, 0, 0);
        }
        return 0;
    }

    // STA for the UI thread: the popup's UI Automation provider uses COM threading.
    const HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    int exitCode = 1;
    {
        App app(instance);
        exitCode = app.Run();
    }
    if (SUCCEEDED(coInit)) {
        CoUninitialize();
    }
    return exitCode;
}
