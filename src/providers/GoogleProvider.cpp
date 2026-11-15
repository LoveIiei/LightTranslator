// Google Cloud Translation - Basic (v2). Only v2 supports plain API keys; Advanced (v3)
// needs OAuth / service accounts.
// https://cloud.google.com/translate/docs/reference/rest/v2/translate
#include "core/Encoding.h"
#include "providers/ITranslationProvider.h"
#include "providers/ProviderUtil.h"

namespace {

using ProviderUtil::CodeMapping;

// Canonical -> Google codes (https://cloud.google.com/translate/docs/languages).
// Google has no en-US / en-GB / es-419 targets.
constexpr CodeMapping kTargets[] = {
    {"ar", "ar"},       {"bg", "bg"},       {"bn", "bn"},       {"ca", "ca"}, {"zh-Hans", "zh-CN"}, {"zh-Hant", "zh-TW"},
    {"hr", "hr"},       {"cs", "cs"},       {"da", "da"},       {"nl", "nl"}, {"en", "en"},         {"et", "et"},
    {"fi", "fi"},       {"fr", "fr"},       {"de", "de"},       {"el", "el"}, {"he", "he"},         {"hi", "hi"},
    {"hu", "hu"},       {"id", "id"},       {"it", "it"},       {"ja", "ja"}, {"ko", "ko"},         {"lv", "lv"},
    {"lt", "lt"},       {"ms", "ms"},       {"nb", "no"},       {"fa", "fa"}, {"pl", "pl"},         {"pt-BR", "pt-BR"},
    {"pt-PT", "pt-PT"}, {"ro", "ro"},       {"ru", "ru"},       {"sr", "sr"}, {"sk", "sk"},         {"sl", "sl"},
    {"es", "es"},       {"sw", "sw"},       {"sv", "sv"},       {"ta", "ta"}, {"th", "th"},         {"tr", "tr"},
    {"uk", "uk"},       {"ur", "ur"},       {"vi", "vi"},
};

bool ContainsAny(std::string_view haystack, std::initializer_list<std::string_view> needles)
{
    for (const std::string_view needle : needles) {
        if (haystack.find(needle) != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

class GoogleProvider final : public ITranslationProvider {
public:
    GoogleProvider() : m_targets(ProviderUtil::BuildTargets(kTargets)) {}

    std::wstring_view Id() const override { return L"google"; }
    std::wstring_view DisplayName() const override { return L"Google Translate"; }
    std::span<const LanguageCode> SupportedTargets() const override { return m_targets; }

    HttpRequestSpec BuildRequest(const TranslateRequest& request, std::string_view apiKey) const override
    {
        nlohmann::json body;
        body["q"] = nlohmann::json::array({Encoding::ToUtf8(request.text)});
        body["target"] = ApiCode(request.targetLang);
        body["format"] = "text"; // otherwise the output comes back HTML-escaped (&#39;)
        if (!request.sourceLang.empty()) {
            body["source"] = ApiCode(request.sourceLang);
        }

        HttpRequestSpec spec;
        spec.host = L"translation.googleapis.com";
        spec.path = L"/language/translate/v2";
        // Key in a header, not ?key=, so it stays out of URLs and proxy logs.
        spec.headers = L"X-Goog-Api-Key: " + Encoding::FromUtf8(ProviderUtil::SanitizeKey(apiKey)) +
                       L"\r\nContent-Type: application/json; charset=utf-8\r\n";
        spec.body = ProviderUtil::Dump(body);
        return spec;
    }

    Result<TranslateResult, ProviderError> ParseResponse(int httpStatus, std::string_view body) const override
    {
        if (httpStatus != 200) {
            return ClassifyError(httpStatus, body);
        }
        const nlohmann::json root = ProviderUtil::ParseJson(body);
        if (root.is_discarded() || !root.is_object()) {
            return ProviderError::Parse;
        }
        const auto data = root.find("data");
        if (data == root.end()) {
            return ProviderError::Parse;
        }
        const nlohmann::json* first = ProviderUtil::FirstOfArray(*data, "translations");
        const std::string* text = first ? ProviderUtil::StringField(*first, "translatedText") : nullptr;
        if (!text) {
            return ProviderError::Parse;
        }
        auto wide = Encoding::FromUtf8Strict(*text);
        if (!wide) {
            return ProviderError::Parse;
        }
        TranslateResult result;
        result.text = std::move(*wide);
        if (const std::string* detected = ProviderUtil::StringField(*first, "detectedSourceLanguage")) {
            result.detectedSourceLang = CanonicalCode(*detected);
        }
        return result;
    }

private:
    // Google reports the reason inside the JSON error body; the HTTP status alone is ambiguous
    // (403 is used for bad/restricted keys AND for exhausted quotas).
    static ProviderError ClassifyError(int httpStatus, std::string_view body)
    {
        const bool quota = ContainsAny(body, {"dailyLimitExceeded", "quotaExceeded", "billingNotEnabled", "BILLING_DISABLED"});
        const bool rate = ContainsAny(body, {"rateLimitExceeded", "userRateLimitExceeded", "RATE_LIMIT_EXCEEDED"});
        const bool badKey = ContainsAny(body, {"API_KEY_INVALID", "API key not valid", "keyInvalid", "API_KEY_SERVICE_BLOCKED",
                                               "SERVICE_DISABLED", "accessNotConfigured"});
        switch (httpStatus) {
        case 400:
            return badKey ? ProviderError::BadKey : ProviderError::BadRequest;
        case 401:
            return ProviderError::BadKey;
        case 403:
            if (quota) return ProviderError::QuotaExceeded;
            if (rate) return ProviderError::RateLimited;
            return ProviderError::BadKey;
        case 429:
            return quota ? ProviderError::QuotaExceeded : ProviderError::RateLimited;
        default:
            return (httpStatus >= 500) ? ProviderError::Server : ProviderError::BadRequest;
        }
    }

    static std::string ApiCode(std::string_view canonical)
    {
        if (const CodeMapping* mapping = ProviderUtil::FindByCanonical(kTargets, canonical)) {
            return std::string(mapping->api);
        }
        // e.g. "en-US" as a configured source: fall back to the primary subtag.
        return ProviderUtil::ToLower(Languages::PrimarySubtag(canonical));
    }

    static std::string CanonicalCode(std::string_view api)
    {
        for (const CodeMapping& mapping : kTargets) {
            if (Languages::EqualsIgnoreCase(mapping.api, api)) {
                return std::string(mapping.canonical);
            }
        }
        if (Languages::EqualsIgnoreCase(api, "iw")) {
            return "he"; // legacy Hebrew code
        }
        if (Languages::EqualsIgnoreCase(api, "zh")) {
            return "zh-Hans";
        }
        return std::string(api);
    }

    std::vector<LanguageCode> m_targets;
};

} // namespace

std::unique_ptr<ITranslationProvider> CreateGoogleProvider()
{
    return std::make_unique<GoogleProvider>();
}
