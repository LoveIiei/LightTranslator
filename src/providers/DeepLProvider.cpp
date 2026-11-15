// DeepL API: POST /v2/translate, header-based auth, JSON body.
// https://developers.deepl.com/api-reference/translate
#include "core/Encoding.h"
#include "providers/ITranslationProvider.h"
#include "providers/ProviderUtil.h"

namespace {

using ProviderUtil::CodeMapping;

// Canonical -> DeepL target codes. DeepL requires regional variants for English and
// Portuguese targets (plain EN / PT are deprecated as targets).
constexpr CodeMapping kTargets[] = {
    {"ar", "AR"},           {"bg", "BG"},       {"zh-Hans", "ZH-HANS"}, {"zh-Hant", "ZH-HANT"}, {"cs", "CS"},
    {"da", "DA"},           {"nl", "NL"},       {"en-GB", "EN-GB"},     {"en-US", "EN-US"},     {"et", "ET"},
    {"fi", "FI"},           {"fr", "FR"},       {"de", "DE"},           {"el", "EL"},           {"he", "HE"},
    {"hu", "HU"},           {"id", "ID"},       {"it", "IT"},           {"ja", "JA"},           {"ko", "KO"},
    {"lv", "LV"},           {"lt", "LT"},       {"nb", "NB"},           {"pl", "PL"},           {"pt-BR", "PT-BR"},
    {"pt-PT", "PT-PT"},     {"ro", "RO"},       {"ru", "RU"},           {"sk", "SK"},           {"sl", "SL"},
    {"es", "ES"},           {"es-419", "ES-419"}, {"sv", "SV"},         {"th", "TH"},           {"tr", "TR"},
    {"uk", "UK"},           {"vi", "VI"},
};

class DeepLProvider final : public ITranslationProvider {
public:
    DeepLProvider() : m_targets(ProviderUtil::BuildTargets(kTargets)) {}

    std::wstring_view Id() const override { return L"deepl"; }
    std::wstring_view DisplayName() const override { return L"DeepL"; }
    std::span<const LanguageCode> SupportedTargets() const override { return m_targets; }

    HttpRequestSpec BuildRequest(const TranslateRequest& request, std::string_view apiKey) const override
    {
        const std::string key = ProviderUtil::SanitizeKey(apiKey);

        // Free-plan keys end in ":fx" and only work on the api-free host.
        bool freePlan = key.ends_with(":fx");
        if (request.plan == "free") {
            freePlan = true;
        } else if (request.plan == "pro") {
            freePlan = false;
        }

        nlohmann::json body;
        body["text"] = nlohmann::json::array({Encoding::ToUtf8(request.text)});
        body["target_lang"] = TargetCode(request.targetLang);
        if (!request.sourceLang.empty()) {
            body["source_lang"] = SourceCode(request.sourceLang);
        }

        HttpRequestSpec spec;
        spec.host = freePlan ? L"api-free.deepl.com" : L"api.deepl.com";
        spec.path = L"/v2/translate";
        // Header auth only: query-string auth_key and GET are deprecated.
        spec.headers = L"Authorization: DeepL-Auth-Key " + Encoding::FromUtf8(key) + L"\r\nContent-Type: application/json\r\n";
        spec.body = ProviderUtil::Dump(body);
        return spec;
    }

    Result<TranslateResult, ProviderError> ParseResponse(int httpStatus, std::string_view body) const override
    {
        if (httpStatus != 200) {
            switch (httpStatus) {
            case 401:
            case 403:
                return ProviderError::BadKey;
            case 456:
                return ProviderError::QuotaExceeded;
            case 429:
            case 529:
                return ProviderError::RateLimited;
            default:
                return (httpStatus >= 500) ? ProviderError::Server : ProviderError::BadRequest;
            }
        }
        const nlohmann::json root = ProviderUtil::ParseJson(body);
        if (root.is_discarded()) {
            return ProviderError::Parse;
        }
        const nlohmann::json* first = ProviderUtil::FirstOfArray(root, "translations");
        const std::string* text = first ? ProviderUtil::StringField(*first, "text") : nullptr;
        if (!text) {
            return ProviderError::Parse;
        }
        auto wide = Encoding::FromUtf8Strict(*text);
        if (!wide) {
            return ProviderError::Parse;
        }
        TranslateResult result;
        result.text = std::move(*wide);
        if (const std::string* detected = ProviderUtil::StringField(*first, "detected_source_language")) {
            result.detectedSourceLang = ProviderUtil::ToLower(*detected); // "EN" -> "en"
        }
        return result;
    }

private:
    static std::string TargetCode(std::string_view canonical)
    {
        if (const CodeMapping* mapping = ProviderUtil::FindByCanonical(kTargets, canonical)) {
            return std::string(mapping->api);
        }
        return ProviderUtil::ToUpper(canonical);
    }

    // Source languages have no regional variants: "en-US" -> "EN", "zh-Hans" -> "ZH".
    static std::string SourceCode(std::string_view canonical)
    {
        return ProviderUtil::ToUpper(Languages::PrimarySubtag(canonical));
    }

    std::vector<LanguageCode> m_targets;
};

} // namespace

std::unique_ptr<ITranslationProvider> CreateDeepLProvider()
{
    return std::make_unique<DeepLProvider>();
}
