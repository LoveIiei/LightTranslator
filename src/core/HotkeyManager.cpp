#include "core/HotkeyManager.h"

HotkeyManager::~HotkeyManager()
{
    Unregister();
}

bool HotkeyManager::Register(HWND hwnd, UINT modifiers, UINT vk)
{
    Unregister();
    m_hwnd = hwnd;
    if (vk == 0) {
        m_lastError = ERROR_INVALID_PARAMETER;
        return false;
    }
    const UINT mods = (modifiers & (MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN)) | MOD_NOREPEAT;
    if (RegisterHotKey(hwnd, kHotkeyId, mods, vk)) {
        m_registered = true;
        m_lastError = ERROR_SUCCESS;
        return true;
    }
    m_lastError = GetLastError();
    return false;
}

void HotkeyManager::Unregister()
{
    if (m_registered) {
        UnregisterHotKey(m_hwnd, kHotkeyId);
        m_registered = false;
    }
}

std::wstring HotkeyManager::Describe(UINT modifiers, UINT vk)
{
    std::wstring text;
    const auto add = [&text](PCWSTR part) {
        if (!text.empty()) {
            text += L" + ";
        }
        text += part;
    };
    if (modifiers & MOD_WIN) add(L"Win");
    if (modifiers & MOD_CONTROL) add(L"Ctrl");
    if (modifiers & MOD_ALT) add(L"Alt");
    if (modifiers & MOD_SHIFT) add(L"Shift");

    wchar_t name[64] = {};
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        name[0] = static_cast<wchar_t>(vk);
    } else if (vk >= VK_F1 && vk <= VK_F24) {
        swprintf_s(name, L"F%u", vk - VK_F1 + 1);
    } else {
        LONG lParam = static_cast<LONG>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC)) << 16;
        switch (vk) { // keys that live on the extended part of the keyboard
        case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_DIVIDE: case VK_NUMLOCK:
            lParam |= 1L << 24;
            break;
        }
        if (GetKeyNameTextW(lParam, name, ARRAYSIZE(name)) == 0) {
            swprintf_s(name, L"0x%02X", vk);
        }
    }
    add(name);
    return text;
}
