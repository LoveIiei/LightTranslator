#include "core/SecretStore.h"

#include "app/AppInfo.h"

#include <windows.h>
#include <wincred.h>
#include <wil/resource.h>

namespace {

using unique_credential = wil::unique_any<PCREDENTIALW, decltype(&::CredFree), ::CredFree>;

std::wstring TargetName(std::wstring_view providerId)
{
    std::wstring target = AppInfo::kName;
    target += L'/';
    target += providerId;
    return target;
}

} // namespace

namespace SecretStore {

bool Write(std::wstring_view providerId, std::string_view keyUtf8)
{
    if (keyUtf8.empty() || keyUtf8.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE) {
        return false;
    }
    std::wstring target = TargetName(providerId);
    wchar_t userName[] = L"api-key";

    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = target.data();
    credential.UserName = userName;
    credential.CredentialBlobSize = static_cast<DWORD>(keyUtf8.size());
    credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(keyUtf8.data()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    return CredWriteW(&credential, 0) != FALSE;
}

std::string Read(std::wstring_view providerId)
{
    unique_credential credential;
    if (!CredReadW(TargetName(providerId).c_str(), CRED_TYPE_GENERIC, 0, credential.put()) || !credential) {
        return {};
    }
    std::string key;
    if (credential.get()->CredentialBlob && credential.get()->CredentialBlobSize > 0) {
        key.assign(reinterpret_cast<const char*>(credential.get()->CredentialBlob), credential.get()->CredentialBlobSize);
        // Copy out, then wipe the system-allocated buffer before it is freed.
        SecureZeroMemory(credential.get()->CredentialBlob, credential.get()->CredentialBlobSize);
    }
    return key;
}

bool Exists(std::wstring_view providerId)
{
    unique_credential credential;
    if (!CredReadW(TargetName(providerId).c_str(), CRED_TYPE_GENERIC, 0, credential.put()) || !credential) {
        return false;
    }
    const bool hasKey = credential.get()->CredentialBlob && credential.get()->CredentialBlobSize > 0;
    if (hasKey) {
        SecureZeroMemory(credential.get()->CredentialBlob, credential.get()->CredentialBlobSize);
    }
    return hasKey;
}

bool Remove(std::wstring_view providerId)
{
    return CredDeleteW(TargetName(providerId).c_str(), CRED_TYPE_GENERIC, 0) != FALSE;
}

void SecureClear(std::string& secret)
{
    if (secret.capacity() > 0) {
        SecureZeroMemory(secret.data(), secret.capacity());
    }
    secret.clear();
}

void SecureClear(std::wstring& secret)
{
    if (secret.capacity() > 0) {
        SecureZeroMemory(secret.data(), secret.capacity() * sizeof(wchar_t));
    }
    secret.clear();
}

} // namespace SecretStore
