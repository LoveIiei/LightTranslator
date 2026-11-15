#pragma once
#include <windows.h>
#include <string>

// Owns the single global hotkey (RegisterHotKey on the app window, UI thread).
class HotkeyManager {
public:
    static constexpr int kHotkeyId = 1;

    ~HotkeyManager();

    // Replaces any previous registration. MOD_NOREPEAT is always added.
    // On failure the previous registration is gone and LastError() holds the Win32 error
    // (ERROR_HOTKEY_ALREADY_REGISTERED = another app owns the combination).
    bool Register(HWND hwnd, UINT modifiers, UINT vk);
    void Unregister();

    bool IsRegistered() const { return m_registered; }
    DWORD LastError() const { return m_lastError; }

    // Human-readable name, e.g. "Win + Alt + T".
    static std::wstring Describe(UINT modifiers, UINT vk);

private:
    HWND m_hwnd = nullptr;
    bool m_registered = false;
    DWORD m_lastError = ERROR_SUCCESS;
};
