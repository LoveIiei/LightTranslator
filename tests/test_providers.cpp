#include <doctest/doctest.h>

#include "core/Encoding.h"
#include "providers/ITranslationProvider.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string ReadFixture(const char* name)
{
    std::ifstream file(std::string(LT_FIXTURE_DIR) + "/" + name, std::ios::binary);
    REQUIRE_MESSAGE(file.is_open(), "missing fixture: ", name);
    std::ostringstream content;
    content << file.rdbuf();
    return content.str();
}

const ITranslationProvider& Provider(const wchar_t* id)
{
    const ITranslationProvider* provider = Providers::Find(id);
    REQUIRE(provider != nullptr);
    return *provider;
}

bool HasTarget(const ITranslationProvider& provider, std::string_view code)
{
    for (const LanguageCode& lang : provider.SupportedTargets()) {
        if (lang.code == code) {
            return true;
        }
    }
    return false;
}

ProviderError ErrorOf(const ITranslationProvider& provider, int status, std::string_view body)
{
    const auto result = provider.ParseResponse(status, body);
    REQUIRE_FALSE(result.ok());
    return result.error();
}

} // namespace

TEST_CASE("Registry: both providers are registered")
{
    CHECK(Providers::All().size() == 2);
    CHECK(Provider(L"deepl").DisplayName() == L"DeepL");
    CHECK(Provider(L"google").DisplayName() == L"Google Translate");
    CHECK(Providers::Find(L"nope") == nullptr);
    CHECK(Providers::FindOrDefault(L"nope").Id() == L"deepl");
}

TEST_CASE("Registry: every supported target is a canonical language")
{
    for (const auto& provider : Providers::All()) {
        CHECK_FALSE(provider->SupportedTargets().empty());
        for (const LanguageCode& lang : provider->SupportedTargets()) {
            CHECK_MESSAGE(Languages::Find(lang.code) != nullptr, "not canonical: ", std::string(lang.code));
        }
    }
}

TEST_CASE("Registry: ResolveTarget picks the closest supported target")
{
    const auto& deepl = Provider(L"deepl");
    const auto& google = Provider(L"google");
    CHECK(Providers::ResolveTarget(deepl, "de") == "de");
    CHECK(Providers::ResolveTarget(deepl, "en") == "en-US");      // DeepL has no plain "en" target
    CHECK(Providers::ResolveTarget(deepl, "pt") == "pt-BR");
    CHECK(Providers::ResolveTarget(deepl, "zh") == "zh-Hans");
    CHECK(Providers::ResolveTarget(google, "en-US") == "en");     // Google has no regional English
    CHECK(Providers::ResolveTarget(google, "es-419") == "es");
    CHECK(Providers::ResolveTarget(google, "zh-Hant") == "zh-Hant");
    CHECK_FALSE(Providers::ResolveTarget(deepl, "xx").empty());   // unknown -> first target
}

// ---- DeepL -------------------------------------------------------------------------------

TEST_CASE("DeepL: BuildRequest uses header auth and a JSON body")
{
    const auto& deepl = Provider(L"deepl");
    TranslateRequest request;
    request.text = L"Hello \"world\"\n\xD83D\xDE00";
    request.targetLang = "de";

    const HttpRequestSpec spec = deepl.BuildRequest(request, "0123-secret-key");
    CHECK(spec.host == L"api.deepl.com"); // Pro key
    CHECK(spec.path == L"/v2/translate");
    CHECK(spec.path.find(L"secret") == std::wstring::npos); // key never in the URL
    CHECK(spec.headers.find(L"Authorization: DeepL-Auth-Key 0123-secret-key\r\n") != std::wstring::npos);
    CHECK(spec.headers.find(L"Content-Type: application/json") != std::wstring::npos);
    CHECK(spec.body.find("secret") == std::string::npos);

    const auto body = nlohmann::json::parse(spec.body, nullptr, false);
    REQUIRE_FALSE(body.is_discarded());
    REQUIRE(body["text"].is_array());
    REQUIRE(body["text"].size() == 1);
    CHECK(body["text"][0].get<std::string>() == Encoding::ToUtf8(request.text));
    CHECK(body["target_lang"].get<std::string>() == "DE");
    CHECK_FALSE(body.contains("source_lang")); // auto-detect
}

TEST_CASE("DeepL: free keys (':fx') go to api-free, and the plan can be overridden")
{
    const auto& deepl = Provider(L"deepl");
    TranslateRequest request;
    request.text = L"x";
    request.targetLang = "fr";

    CHECK(deepl.BuildRequest(request, "abc:fx").host == L"api-free.deepl.com");
    CHECK(deepl.BuildRequest(request, "  abc:fx \r\n").host == L"api-free.deepl.com"); // pasted with whitespace
    CHECK(deepl.BuildRequest(request, "abc").host == L"api.deepl.com");

    request.plan = "pro";
    CHECK(deepl.BuildRequest(request, "abc:fx").host == L"api.deepl.com");
    request.plan = "free";
    CHECK(deepl.BuildRequest(request, "abc").host == L"api-free.deepl.com");
}

TEST_CASE("DeepL: language mapping and key sanitizing")
{
    const auto& deepl = Provider(L"deepl");
    TranslateRequest request;
    request.text = L"x";

    const auto target = [&](const char* canonical) {
        request.targetLang = canonical;
        request.sourceLang.clear();
        return nlohmann::json::parse(deepl.BuildRequest(request, "k").body)["target_lang"].get<std::string>();
    };
    CHECK(target("en-US") == "EN-US");
    CHECK(target("en-GB") == "EN-GB");
    CHECK(target("pt-BR") == "PT-BR");
    CHECK(target("pt-PT") == "PT-PT");
    CHECK(target("zh-Hans") == "ZH-HANS");
    CHECK(target("zh-Hant") == "ZH-HANT");
    CHECK(target("nb") == "NB");

    request.targetLang = "de";
    request.sourceLang = "en-US"; // sources have no regional variants
    CHECK(nlohmann::json::parse(deepl.BuildRequest(request, "k").body)["source_lang"].get<std::string>() == "EN");
    request.sourceLang = "zh-Hans";
    CHECK(nlohmann::json::parse(deepl.BuildRequest(request, "k").body)["source_lang"].get<std::string>() == "ZH");

    CHECK(HasTarget(deepl, "en-US"));
    CHECK(HasTarget(deepl, "pt-BR"));
    CHECK_FALSE(HasTarget(deepl, "en")); // plain EN is deprecated as a DeepL target

    // No header injection through a malicious/garbled key.
    const HttpRequestSpec spec = deepl.BuildRequest(request, "ab\r\nX-Evil: 1");
    CHECK(spec.headers.find(L"\r\nX-Evil") == std::wstring::npos);
}

TEST_CASE("DeepL: ParseResponse success fixture")
{
    const auto& deepl = Provider(L"deepl");
    const auto result = deepl.ParseResponse(200, ReadFixture("deepl_success.json"));
    REQUIRE(result.ok());
    CHECK(result.value().text == L"Hallo, Welt! Grüß" L"e \xD83D\xDE00");
    CHECK(result.value().detectedSourceLang == "en");
}

TEST_CASE("DeepL: ParseResponse error mapping")
{
    const auto& deepl = Provider(L"deepl");
    CHECK(ErrorOf(deepl, 403, ReadFixture("deepl_error_403.json")) == ProviderError::BadKey);
    CHECK(ErrorOf(deepl, 456, ReadFixture("deepl_quota_456.json")) == ProviderError::QuotaExceeded);
    CHECK(ErrorOf(deepl, 429, "{\"message\":\"Too many requests\"}") == ProviderError::RateLimited);
    CHECK(ErrorOf(deepl, 529, "") == ProviderError::RateLimited);
    CHECK(ErrorOf(deepl, 400, "{\"message\":\"Value for 'target_lang' not supported.\"}") == ProviderError::BadRequest);
    CHECK(ErrorOf(deepl, 413, "") == ProviderError::BadRequest);
    CHECK(ErrorOf(deepl, 500, "") == ProviderError::Server);
    CHECK(ErrorOf(deepl, 503, "<html>Service Unavailable</html>") == ProviderError::Server);
}

TEST_CASE("DeepL: malformed success bodies are Parse errors, never exceptions")
{
    const auto& deepl = Provider(L"deepl");
    CHECK(ErrorOf(deepl, 200, "") == ProviderError::Parse);
    CHECK(ErrorOf(deepl, 200, "not json") == ProviderError::Parse);
    CHECK(ErrorOf(deepl, 200, "{\"translations\":[{\"text\":\"cut") == ProviderError::Parse);
    CHECK(ErrorOf(deepl, 200, "[]") == ProviderError::Parse);
    CHECK(ErrorOf(deepl, 200, "{}") == ProviderError::Parse);
    CHECK(ErrorOf(deepl, 200, "{\"translations\":[]}") == ProviderError::Parse);
    CHECK(ErrorOf(deepl, 200, "{\"translations\":{}}") == ProviderError::Parse);
    CHECK(ErrorOf(deepl, 200, "{\"translations\":[{\"text\":42}]}") == ProviderError::Parse);
    CHECK(ErrorOf(deepl, 200, "{\"translations\":[\"text\"]}") == ProviderError::Parse);
    CHECK(ErrorOf(deepl, 200, "{\"translations\":[{\"text\":\"\xFF\xFE\"}]}") == ProviderError::Parse); // invalid UTF-8

    // A missing detected language is fine.
    const auto result = deepl.ParseResponse(200, "{\"translations\":[{\"text\":\"ok\"}]}");
    REQUIRE(result.ok());
    CHECK(result.value().text == L"ok");
    CHECK(result.value().detectedSourceLang.empty());
}

// ---- Google ------------------------------------------------------------------------------

TEST_CASE("Google: BuildRequest sends the key in a header and format=text")
{
    const auto& google = Provider(L"google");
    TranslateRequest request;
    request.text = L"It's a test";
    request.targetLang = "zh-Hans";

    const HttpRequestSpec spec = google.BuildRequest(request, "AIza-secret");
    CHECK(spec.host == L"translation.googleapis.com");
    CHECK(spec.path == L"/language/translate/v2");
    CHECK(spec.path.find(L"key=") == std::wstring::npos); // not in the URL
    CHECK(spec.headers.find(L"X-Goog-Api-Key: AIza-secret\r\n") != std::wstring::npos);
    CHECK(spec.body.find("AIza") == std::string::npos);

    const auto body = nlohmann::json::parse(spec.body, nullptr, false);
    REQUIRE_FALSE(body.is_discarded());
    REQUIRE(body["q"].is_array());
    CHECK(body["q"][0].get<std::string>() == "It's a test");
    CHECK(body["target"].get<std::string>() == "zh-CN");
    CHECK(body["format"].get<std::string>() == "text");
    CHECK_FALSE(body.contains("source"));

    request.sourceLang = "nb";
    request.targetLang = "zh-Hant";
    const auto body2 = nlohmann::json::parse(google.BuildRequest(request, "k").body);
    CHECK(body2["source"].get<std::string>() == "no");
    CHECK(body2["target"].get<std::string>() == "zh-TW");

    request.sourceLang = "en-US"; // not a Google code: falls back to the primary subtag
    CHECK(nlohmann::json::parse(google.BuildRequest(request, "k").body)["source"].get<std::string>() == "en");

    CHECK(HasTarget(google, "en"));
    CHECK_FALSE(HasTarget(google, "en-US"));
}

TEST_CASE("Google: ParseResponse success fixture")
{
    const auto& google = Provider(L"google");
    const auto result = google.ParseResponse(200, ReadFixture("google_success.json"));
    REQUIRE(result.ok());
    CHECK(result.value().text == L"C'est l'été — 你好");
    CHECK(result.value().detectedSourceLang == "zh-Hans");
}

TEST_CASE("Google: ParseResponse error mapping")
{
    const auto& google = Provider(L"google");
    CHECK(ErrorOf(google, 400, ReadFixture("google_error_badkey.json")) == ProviderError::BadKey);
    CHECK(ErrorOf(google, 403, ReadFixture("google_quota_403.json")) == ProviderError::QuotaExceeded);
    CHECK(ErrorOf(google, 403, ReadFixture("google_ratelimit_403.json")) == ProviderError::RateLimited);
    CHECK(ErrorOf(google, 403, "{\"error\":{\"status\":\"PERMISSION_DENIED\"}}") == ProviderError::BadKey);
    CHECK(ErrorOf(google, 429, "{\"error\":{\"status\":\"RESOURCE_EXHAUSTED\"}}") == ProviderError::RateLimited);
    CHECK(ErrorOf(google, 400, "{\"error\":{\"message\":\"Invalid Value\"}}") == ProviderError::BadRequest);
    CHECK(ErrorOf(google, 500, "") == ProviderError::Server);
}

TEST_CASE("Google: malformed success bodies are Parse errors, never exceptions")
{
    const auto& google = Provider(L"google");
    CHECK(ErrorOf(google, 200, "") == ProviderError::Parse);
    CHECK(ErrorOf(google, 200, "null") == ProviderError::Parse);
    CHECK(ErrorOf(google, 200, "{}") == ProviderError::Parse);
    CHECK(ErrorOf(google, 200, "{\"data\":[]}") == ProviderError::Parse);
    CHECK(ErrorOf(google, 200, "{\"data\":{\"translations\":[]}}") == ProviderError::Parse);
    CHECK(ErrorOf(google, 200, "{\"data\":{\"translations\":[{}]}}") == ProviderError::Parse);
    CHECK(ErrorOf(google, 200, "{\"data\":{\"translations\":[{\"translatedText\":null}]}}") == ProviderError::Parse);

    const auto result = google.ParseResponse(200, "{\"data\":{\"translations\":[{\"translatedText\":\"ok\"}]}}");
    REQUIRE(result.ok());
    CHECK(result.value().detectedSourceLang.empty()); // absent when the source was specified
}
