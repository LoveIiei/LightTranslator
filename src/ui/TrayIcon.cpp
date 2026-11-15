#include "ui/TrayIcon.h"

#include "app/AppInfo.h"
#include "app/Messages.h"
#include "resource.h"

#include <commctrl.h>

namespace {
constexpr UINT kTrayIconId = 1;
}

TrayIcon::~TrayIcon()
{
    Remove();
}

NOTIFYICONDATAW TrayIcon::Base() const
{
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = m_hwnd;
    nid.uID = kTrayIconId;
    return nid;
}

void TrayIcon::LoadThemedIcon(bool lightTaskbar)
{
    // LoadIconMetric picks/scales the best image for the current DPI (needs Common Controls v6).
    wil::unique_hicon icon;
    if (SUCCEEDED(LoadIconMetric(m_instance, MAKEINTRESOURCEW(lightTaskbar ? IDI_TRAY_LIGHT : IDI_TRAY_DARK), LIM_SMALL,
                                 icon.put()))) {
        m_icon = std::move(icon);
    }
}

bool TrayIcon::Add(HWND hwnd, HINSTANCE instance, bool lightTaskbar)
{
    m_hwnd = hwnd;
    m_instance = instance;
    LoadThemedIcon(lightTaskbar);

    NOTIFYICONDATAW nid = Base();
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = m_icon.get();
    wcsncpy_s(nid.szTip, m_tip.empty() ? AppInfo::kName : m_tip.c_str(), _TRUNCATE);

    if (!Shell_NotifyIconW(NIM_ADD, &nid) && !Shell_NotifyIconW(NIM_MODIFY, &nid)) {
        m_added = false;
        return false; // Explorer not ready yet; "TaskbarCreated" will bring us back here.
    }
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
    m_added = true;
    return true;
}

void TrayIcon::Remove()
{
    if (m_added) {
        NOTIFYICONDATAW nid = Base();
        Shell_NotifyIconW(NIM_DELETE, &nid);
        m_added = false;
    }
}

void TrayIcon::SetTooltip(const std::wstring& tip)
{
    m_tip = tip;
    if (!m_added) {
        return;
    }
    NOTIFYICONDATAW nid = Base();
    nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    wcsncpy_s(nid.szTip, m_tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayIcon::ShowBalloon(const std::wstring& title, const std::wstring& text, bool warning)
{
    if (!m_added) {
        return;
    }
    NOTIFYICONDATAW nid = Base();
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = warning ? NIIF_WARNING : NIIF_INFO;
    wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(nid.szInfo, text.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayIcon::SetTheme(bool lightTaskbar)
{
    LoadThemedIcon(lightTaskbar);
    if (!m_added) {
        return;
    }
    NOTIFYICONDATAW nid = Base();
    nid.uFlags = NIF_ICON;
    nid.hIcon = m_icon.get();
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}
