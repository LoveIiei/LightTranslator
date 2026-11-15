#pragma once
#include <windows.h>
#include <shellapi.h>
#include <string>
#include <wil/resource.h>

// Notification-area icon. Identified by (hwnd, uID) rather than a GUID so a portable exe
// keeps working after it is moved to another folder.
class TrayIcon {
public:
    ~TrayIcon();

    // Adds (or re-adds after an Explorer restart) the icon. Callback message: WM_APP_TRAY.
    bool Add(HWND hwnd, HINSTANCE instance, bool lightTaskbar);
    void Remove();
    void SetTheme(bool lightTaskbar);
    void SetTooltip(const std::wstring& tip);

    // Notification toast/balloon anchored to the icon. Used for errors that must not be silent.
    void ShowBalloon(const std::wstring& title, const std::wstring& text, bool warning);

private:
    NOTIFYICONDATAW Base() const;
    void LoadThemedIcon(bool lightTaskbar);

    HWND m_hwnd = nullptr;
    HINSTANCE m_instance = nullptr;
    wil::unique_hicon m_icon;
    std::wstring m_tip;
    bool m_added = false;
};
