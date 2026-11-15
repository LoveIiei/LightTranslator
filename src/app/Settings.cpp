#include "app/Settings.h"

#include "app/AppInfo.h"
#include "core/Encoding.h"

#include <wil/resource.h>
#include <wil/stl.h>
#include <wil/win32_helpers.h>
#include <algorithm>

namespace {

DWORD ReadDword(PCWSTR name, DWORD fallback)
{
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, AppInfo::kRegistryKey, name, RRF_RT_REG_DWORD, nullptr, &value, &size) ==
        ERROR_SUCCESS) {
        return value;
    }
    return fallback;
}

std::wstring ReadString(HKEY root, PCWSTR key, PCWSTR name, std::wstring fallback)
{
    DWORD bytes = 0;
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS || bytes < sizeof(wchar_t)) {
        return fallback;
    }
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ, nullptr, value.data(), &bytes) != ERROR_SUCCESS) {
        return fallback;
    }
    value.resize(bytes / sizeof(wchar_t));
    while (!value.empty() && value.back() == L'\0') {
        value.pop_back();
    }
    return value;
}

void WriteDword(HKEY key, PCWSTR name, DWORD value)
{
    RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
}

void WriteString(HKEY key, PCWSTR name, const std::wstring& value)
{
    RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                   static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

std::wstring QuotedExePath()
{
    std::wstring path;
    if (FAILED(wil::GetModuleFileNameW(nullptr, path)) || path.empty()) {
        return {};
    }
    return L"\"" + path + L"\"";
}

} // namespace

void Settings::Load()
{
    const Settings defaults;
    hotkeyModifiers = ReadDword(L"HotkeyModifiers", defaults.hotkeyModifiers) & (MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN);
    hotkeyVk = ReadDword(L"HotkeyVk", defaults.hotkeyVk) & 0xFF;
    providerId = ReadString(HKEY_CURRENT_USER, AppInfo::kRegistryKey, L"Provider", defaults.providerId);

    const std::wstring source = ReadString(HKEY_CURRENT_USER, AppInfo::kRegistryKey, L"SourceLang", L"auto");
    sourceLang = (source == L"auto") ? std::string{} : Encoding::ToUtf8(source);
    targetLang = Encoding::ToUtf8(
        ReadString(HKEY_CURRENT_USER, AppInfo::kRegistryKey, L"TargetLang", Encoding::FromUtf8(defaults.targetLang)));
    if (targetLang.empty()) {
        targetLang = defaults.targetLang;
    }

    allowClipboardFallback = ReadDword(L"AllowClipboardFallback", defaults.allowClipboardFallback ? 1 : 0) != 0;
    deeplPlan = ReadString(HKEY_CURRENT_USER, AppInfo::kRegistryKey, L"DeepLPlan", defaults.deeplPlan);
    maxChars = std::clamp<UINT>(ReadDword(L"MaxChars", defaults.maxChars), 100, 50000);
    useBackdrop = ReadDword(L"UseBackdrop", 1) != 0;
    popupPinned = ReadDword(L"PopupPinned", 0) != 0;
    hasPinPosition = ReadDword(L"HasPinPosition", 0) != 0;
    pinX = static_cast<int>(ReadDword(L"PinX", 0));
    pinY = static_cast<int>(ReadDword(L"PinY", 0));

    // The Run key is the source of truth: the user may have removed the entry via Task Manager / Settings.
    startWithWindows = IsAutostartEnabled();
}

void Settings::Save() const
{
    wil::unique_hkey key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, AppInfo::kRegistryKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, key.put(),
                        nullptr) != ERROR_SUCCESS) {
        return;
    }
    WriteDword(key.get(), L"HotkeyModifiers", hotkeyModifiers);
    WriteDword(key.get(), L"HotkeyVk", hotkeyVk);
    WriteString(key.get(), L"Provider", providerId);
    WriteString(key.get(), L"SourceLang", sourceLang.empty() ? std::wstring(L"auto") : Encoding::FromUtf8(sourceLang));
    WriteString(key.get(), L"TargetLang", Encoding::FromUtf8(targetLang));
    WriteDword(key.get(), L"AllowClipboardFallback", allowClipboardFallback ? 1 : 0);
    WriteString(key.get(), L"DeepLPlan", deeplPlan);
    WriteDword(key.get(), L"StartWithWindows", startWithWindows ? 1 : 0);
    WriteDword(key.get(), L"MaxChars", maxChars);
    WriteDword(key.get(), L"UseBackdrop", useBackdrop ? 1 : 0);
    WriteDword(key.get(), L"PopupPinned", popupPinned ? 1 : 0);
    WriteDword(key.get(), L"HasPinPosition", hasPinPosition ? 1 : 0);
    WriteDword(key.get(), L"PinX", static_cast<DWORD>(pinX));
    WriteDword(key.get(), L"PinY", static_cast<DWORD>(pinY));
}

bool Settings::IsAutostartEnabled()
{
    return !ReadString(HKEY_CURRENT_USER, AppInfo::kRunKey, AppInfo::kName, {}).empty();
}

bool Settings::SetAutostart(bool enable)
{
    wil::unique_hkey key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, AppInfo::kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, key.put(), nullptr) !=
        ERROR_SUCCESS) {
        return false;
    }
    if (!enable) {
        const LSTATUS status = RegDeleteValueW(key.get(), AppInfo::kName);
        return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
    }
    const std::wstring command = QuotedExePath();
    if (command.empty()) {
        return false;
    }
    return RegSetValueExW(key.get(), AppInfo::kName, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                          static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}
