#pragma once
#include <span>
#include <string_view>

// Canonical language list. Internal codes are BCP-47 style; each provider owns its own mapping
// from these codes to whatever its API expects.
struct LanguageCode {
    std::string_view code;  // canonical, e.g. "pt-BR"
    std::wstring_view name; // English display name
    bool rtl = false;       // right-to-left script
};

namespace Languages {

inline constexpr LanguageCode kAll[] = {
    {"ar", L"Arabic", true},
    {"bg", L"Bulgarian"},
    {"bn", L"Bengali"},
    {"ca", L"Catalan"},
    {"zh-Hans", L"Chinese (Simplified)"},
    {"zh-Hant", L"Chinese (Traditional)"},
    {"hr", L"Croatian"},
    {"cs", L"Czech"},
    {"da", L"Danish"},
    {"nl", L"Dutch"},
    {"en", L"English"},
    {"en-GB", L"English (UK)"},
    {"en-US", L"English (US)"},
    {"et", L"Estonian"},
    {"fi", L"Finnish"},
    {"fr", L"French"},
    {"de", L"German"},
    {"el", L"Greek"},
    {"he", L"Hebrew", true},
    {"hi", L"Hindi"},
    {"hu", L"Hungarian"},
    {"id", L"Indonesian"},
    {"it", L"Italian"},
    {"ja", L"Japanese"},
    {"ko", L"Korean"},
    {"lv", L"Latvian"},
    {"lt", L"Lithuanian"},
    {"ms", L"Malay"},
    {"nb", L"Norwegian (Bokmål)"},
    {"fa", L"Persian", true},
    {"pl", L"Polish"},
    {"pt-BR", L"Portuguese (Brazil)"},
    {"pt-PT", L"Portuguese (Portugal)"},
    {"ro", L"Romanian"},
    {"ru", L"Russian"},
    {"sr", L"Serbian"},
    {"sk", L"Slovak"},
    {"sl", L"Slovenian"},
    {"es", L"Spanish"},
    {"es-419", L"Spanish (Latin America)"},
    {"sw", L"Swahili"},
    {"sv", L"Swedish"},
    {"ta", L"Tamil"},
    {"th", L"Thai"},
    {"tr", L"Turkish"},
    {"uk", L"Ukrainian"},
    {"ur", L"Urdu", true},
    {"vi", L"Vietnamese"},
};

constexpr char ToLowerAscii(char c)
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

constexpr bool EqualsIgnoreCase(std::string_view a, std::string_view b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (ToLowerAscii(a[i]) != ToLowerAscii(b[i])) {
            return false;
        }
    }
    return true;
}

// "pt-BR" -> "pt"
constexpr std::string_view PrimarySubtag(std::string_view code)
{
    const size_t dash = code.find_first_of("-_");
    return dash == std::string_view::npos ? code : code.substr(0, dash);
}

// Exact (case-insensitive) match in the canonical list.
constexpr const LanguageCode* Find(std::string_view code)
{
    for (const LanguageCode& lang : kAll) {
        if (EqualsIgnoreCase(lang.code, code)) {
            return &lang;
        }
    }
    return nullptr;
}

// First canonical entry sharing the primary subtag ("pt" -> "pt-BR").
constexpr const LanguageCode* FindByPrimary(std::string_view code)
{
    const std::string_view primary = PrimarySubtag(code);
    for (const LanguageCode& lang : kAll) {
        if (EqualsIgnoreCase(PrimarySubtag(lang.code), primary)) {
            return &lang;
        }
    }
    return nullptr;
}

// Display name for any code a provider may report ("EN" -> "English", "pt" -> "Portuguese").
// Returns an empty view for unknown codes.
constexpr std::wstring_view DisplayName(std::string_view code)
{
    if (const LanguageCode* exact = Find(code)) {
        return exact->name;
    }
    if (const LanguageCode* primary = FindByPrimary(code)) {
        const size_t paren = primary->name.find(L" (");
        return paren == std::wstring_view::npos ? primary->name : primary->name.substr(0, paren);
    }
    return {};
}

constexpr bool IsRtl(std::string_view code)
{
    const LanguageCode* lang = Find(code);
    if (!lang) {
        lang = FindByPrimary(code);
    }
    return lang && lang->rtl;
}

} // namespace Languages
