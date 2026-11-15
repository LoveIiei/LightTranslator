#pragma once
#include <string>
#include <string_view>

// API keys in Windows Credential Manager (generic credentials "LightTranslate/<provider id>",
// CRED_PERSIST_LOCAL_MACHINE: no enterprise roaming). The blob is the key as UTF-8 bytes.
//
// Threat model, stated honestly: this keeps keys out of config files, the registry, backups
// and screenshots. It does NOT protect against malware running as the same user.
namespace SecretStore {

bool Write(std::wstring_view providerId, std::string_view keyUtf8);

// Returns the key (empty if none). The caller must SecureClear() its copy after use.
std::string Read(std::wstring_view providerId);

bool Exists(std::wstring_view providerId);
bool Remove(std::wstring_view providerId);

// Zero the buffer (whole capacity), then clear.
void SecureClear(std::string& secret);
void SecureClear(std::wstring& secret);

} // namespace SecretStore
