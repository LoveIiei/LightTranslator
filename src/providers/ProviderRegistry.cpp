#include "providers/ITranslationProvider.h"

#include <vector>

// One declaration + one array entry per provider. Nothing else in the app needs to change.
std::unique_ptr<ITranslationProvider> CreateDeepLProvider();
std::unique_ptr<ITranslationProvider> CreateGoogleProvider();

namespace {

using Factory = std::unique_ptr<ITranslationProvider> (*)();

constexpr Factory kFactories[] = {
    &CreateDeepLProvider,
    &CreateGoogleProvider,
};

const std::vector<std::unique_ptr<ITranslationProvider>>& Instances()
{
    static const std::vector<std::unique_ptr<ITranslationProvider>> instances = [] {
        std::vector<std::unique_ptr<ITranslationProvider>> list;
        for (const Factory factory : kFactories) {
            list.push_back(factory());
        }
        return list;
    }();
    return instances;
}

struct RegionalDefault {
    std::string_view primary;
    std::string_view preferred;
};
constexpr RegionalDefault kRegionalDefaults[] = {
    {"en", "en-US"},
    {"pt", "pt-BR"},
    {"zh", "zh-Hans"},
    {"es", "es"},
};

const LanguageCode* FindTarget(const ITranslationProvider& provider, std::string_view code)
{
    for (const LanguageCode& lang : provider.SupportedTargets()) {
        if (Languages::EqualsIgnoreCase(lang.code, code)) {
            return &lang;
        }
    }
    return nullptr;
}

} // namespace

namespace Providers {

std::span<const std::unique_ptr<ITranslationProvider>> All()
{
    return Instances();
}

const ITranslationProvider* Find(std::wstring_view id)
{
    for (const auto& provider : Instances()) {
        if (provider->Id() == id) {
            return provider.get();
        }
    }
    return nullptr;
}

const ITranslationProvider& FindOrDefault(std::wstring_view id)
{
    const ITranslationProvider* provider = Find(id);
    return provider ? *provider : *Instances().front();
}

std::string ResolveTarget(const ITranslationProvider& provider, std::string_view desired)
{
    if (const LanguageCode* exact = FindTarget(provider, desired)) {
        return std::string(exact->code);
    }
    const std::string_view primary = Languages::PrimarySubtag(desired);
    for (const RegionalDefault& entry : kRegionalDefaults) {
        if (Languages::EqualsIgnoreCase(entry.primary, primary)) {
            if (const LanguageCode* preferred = FindTarget(provider, entry.preferred)) {
                return std::string(preferred->code);
            }
        }
    }
    for (const LanguageCode& lang : provider.SupportedTargets()) {
        if (Languages::EqualsIgnoreCase(Languages::PrimarySubtag(lang.code), primary)) {
            return std::string(lang.code);
        }
    }
    const auto targets = provider.SupportedTargets();
    return targets.empty() ? std::string(desired) : std::string(targets.front().code);
}

} // namespace Providers
