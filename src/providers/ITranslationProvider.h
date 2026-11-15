#pragma once
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "providers/Languages.h"

// Tiny local result type (C++20 has no std::expected). T and E must be distinct types.
template <class T, class E>
class Result {
public:
    Result(T value) : m_data(std::in_place_index<0>, std::move(value)) {}
    Result(E error) : m_data(std::in_place_index<1>, std::move(error)) {}

    bool ok() const { return m_data.index() == 0; }
    explicit operator bool() const { return ok(); }
    T& value() { return std::get<0>(m_data); }
    const T& value() const { return std::get<0>(m_data); }
    const E& error() const { return std::get<1>(m_data); }

private:
    std::variant<T, E> m_data;
};

struct TranslateRequest {
    std::wstring text;
    std::string sourceLang; // canonical code; empty = auto-detect
    std::string targetLang; // canonical code
    std::string plan;       // optional provider-specific tier override (DeepL: "free" / "pro"); empty = auto
};

struct TranslateResult {
    std::wstring text;
    std::string detectedSourceLang; // canonical-ish code, may be empty
};

// Everything the network layer needs for one HTTPS POST (port 443). Providers never do I/O.
struct HttpRequestSpec {
    std::wstring host;
    std::wstring path;
    std::wstring headers; // CRLF-separated; contains the API key, so it is zeroed right after sending
    std::string body;     // UTF-8 JSON
};

enum class ProviderError { BadKey, QuotaExceeded, RateLimited, BadRequest, Server, Parse };

// Pure, stateless and thread-safe: BuildRequest runs on the network thread just before the
// request is sent, ParseResponse right after the response arrived.
class ITranslationProvider {
public:
    virtual ~ITranslationProvider() = default;
    virtual std::wstring_view Id() const = 0; // L"deepl", L"google"
    virtual std::wstring_view DisplayName() const = 0;
    virtual std::span<const LanguageCode> SupportedTargets() const = 0;
    virtual HttpRequestSpec BuildRequest(const TranslateRequest& request, std::string_view apiKey) const = 0;
    virtual Result<TranslateResult, ProviderError> ParseResponse(int httpStatus, std::string_view body) const = 0;
};

// ProviderRegistry.cpp. Adding a provider = one new .cpp file + one entry in the factory array.
namespace Providers {

std::span<const std::unique_ptr<ITranslationProvider>> All();
const ITranslationProvider* Find(std::wstring_view id);
const ITranslationProvider& FindOrDefault(std::wstring_view id);

// The provider's closest supported target for a desired canonical code:
// exact match, then a preferred regional default ("en" -> "en-US"), then any entry with the
// same primary subtag, then the provider's first target.
std::string ResolveTarget(const ITranslationProvider& provider, std::string_view desired);

} // namespace Providers
