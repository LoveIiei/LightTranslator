#include "core/Encoding.h"

#include <windows.h>
#include <climits>

namespace Encoding {

std::string ToUtf8(std::wstring_view utf16)
{
    if (utf16.empty() || utf16.size() > static_cast<size_t>(INT_MAX)) {
        return {};
    }
    const int in = static_cast<int>(utf16.size());
    // No WC_ERR_INVALID_CHARS: lone surrogates are replaced with U+FFFD.
    const int needed = WideCharToMultiByte(CP_UTF8, 0, utf16.data(), in, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(needed), '\0');
    const int written = WideCharToMultiByte(CP_UTF8, 0, utf16.data(), in, out.data(), needed, nullptr, nullptr);
    out.resize(written > 0 ? static_cast<size_t>(written) : 0);
    return out;
}

static std::optional<std::wstring> Decode(std::string_view utf8, DWORD flags)
{
    if (utf8.empty()) {
        return std::wstring{};
    }
    if (utf8.size() > static_cast<size_t>(INT_MAX)) {
        return std::nullopt;
    }
    const int in = static_cast<int>(utf8.size());
    const int needed = MultiByteToWideChar(CP_UTF8, flags, utf8.data(), in, nullptr, 0);
    if (needed <= 0) {
        return std::nullopt;
    }
    std::wstring out(static_cast<size_t>(needed), L'\0');
    const int written = MultiByteToWideChar(CP_UTF8, flags, utf8.data(), in, out.data(), needed);
    if (written <= 0) {
        return std::nullopt;
    }
    out.resize(static_cast<size_t>(written));
    return out;
}

std::optional<std::wstring> FromUtf8Strict(std::string_view utf8)
{
    return Decode(utf8, MB_ERR_INVALID_CHARS);
}

std::wstring FromUtf8(std::string_view utf8)
{
    return Decode(utf8, 0).value_or(std::wstring{});
}

} // namespace Encoding
