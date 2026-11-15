#pragma once
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "providers/Languages.h"

// Small helpers shared by provider implementations. Header-only, no I/O.
namespace ProviderUtil {

struct CodeMapping {
    std::string_view canonical; // our code
    std::string_view api;       // the provider's code
};

// Supported targets in canonical-list (display) order.
inline std::vector<LanguageCode> BuildTargets(std::span<const CodeMapping> mappings)
{
    std::vector<LanguageCode> targets;
    for (const LanguageCode& lang : Languages::kAll) {
        for (const CodeMapping& mapping : mappings) {
            if (mapping.canonical == lang.code) {
                targets.push_back(lang);
                break;
            }
        }
    }
    return targets;
}

inline const CodeMapping* FindByCanonical(std::span<const CodeMapping> mappings, std::string_view canonical)
{
    for (const CodeMapping& mapping : mappings) {
        if (Languages::EqualsIgnoreCase(mapping.canonical, canonical)) {
            return &mapping;
        }
    }
    return nullptr;
}

inline std::string ToUpper(std::string_view text)
{
    std::string out(text);
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return out;
}

inline std::string ToLower(std::string_view text)
{
    std::string out(text);
    for (char& c : out) {
        c = Languages::ToLowerAscii(c);
    }
    return out;
}

// API keys go into an HTTP header: keep printable ASCII only (no CR/LF injection, no stray
// whitespace from copy/paste).
inline std::string SanitizeKey(std::string_view key)
{
    std::string out;
    out.reserve(key.size());
    for (const char c : key) {
        if (c > 0x20 && c < 0x7F) {
            out.push_back(c);
        }
    }
    return out;
}

// Non-throwing parse: check is_discarded() on the result.
inline nlohmann::json ParseJson(std::string_view body)
{
    return nlohmann::json::parse(body.begin(), body.end(), nullptr, /*allow_exceptions*/ false);
}

// Serializes without throwing on odd input (invalid UTF-8 would be replaced, never an exception).
inline std::string Dump(const nlohmann::json& value)
{
    return value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

// obj[key] as a string, or nullptr if obj is not an object / key is missing / not a string.
inline const std::string* StringField(const nlohmann::json& obj, const char* key)
{
    if (!obj.is_object()) {
        return nullptr;
    }
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) {
        return nullptr;
    }
    return it->get_ptr<const std::string*>();
}

// obj[key][0] if obj[key] is a non-empty array, else nullptr.
inline const nlohmann::json* FirstOfArray(const nlohmann::json& obj, const char* key)
{
    if (!obj.is_object()) {
        return nullptr;
    }
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_array() || it->empty()) {
        return nullptr;
    }
    return &(*it)[0];
}

} // namespace ProviderUtil
