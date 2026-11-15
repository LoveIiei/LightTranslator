#pragma once

// Application-wide identity constants.
namespace AppInfo {

inline constexpr wchar_t kName[] = L"LightTranslate";
inline constexpr wchar_t kWindowClass[] = L"LightTranslate.AppWindow";
inline constexpr wchar_t kMutexName[] = L"Local\\LightTranslate-{7C0E1B52-6D1F-4B0A-9A53-2E8F4C7D91A6}";
inline constexpr wchar_t kRegistryKey[] = L"Software\\LightTranslate";
inline constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

} // namespace AppInfo
