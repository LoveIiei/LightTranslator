#pragma once
#include <optional>
#include <string>
#include <string_view>

// The ONLY UTF-8 <-> UTF-16 conversion code in the project.
// UTF-16 (std::wstring) is used everywhere in Win32/DirectWrite/UI;
// UTF-8 (std::string) only at the JSON/HTTP boundary.
namespace Encoding {

// Lenient: invalid UTF-16 (e.g. lone surrogates from the clipboard) becomes U+FFFD.
std::string ToUtf8(std::wstring_view utf16);

// Strict: returns nullopt on invalid UTF-8. Use for network responses.
std::optional<std::wstring> FromUtf8Strict(std::string_view utf8);

// Lenient: invalid sequences become U+FFFD. Use for trusted/internal strings only.
std::wstring FromUtf8(std::string_view utf8);

} // namespace Encoding
