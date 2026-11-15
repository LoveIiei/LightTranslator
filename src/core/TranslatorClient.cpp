#include "core/TranslatorClient.h"

#include "core/SecretStore.h"

#include <memory>

namespace {

TranslateStatus StatusFromWinHttpError(DWORD error)
{
    switch (error) {
    case ERROR_WINHTTP_TIMEOUT:
        return TranslateStatus::Timeout;
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
    case ERROR_WINHTTP_CANNOT_CONNECT:
    case ERROR_WINHTTP_CONNECTION_ERROR:
        return TranslateStatus::Offline;
    case ERROR_WINHTTP_SECURE_FAILURE:
    case ERROR_WINHTTP_SECURE_CHANNEL_ERROR:
    case ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED:
        return TranslateStatus::TlsFailure;
    default:
        return TranslateStatus::NetworkError;
    }
}

void PostOutcome(const TranslateJob& job, TranslateOutcome outcome)
{
    auto payload = std::make_unique<TranslateOutcome>(std::move(outcome));
    if (PostMessageW(job.replyTo, job.replyMessage, job.requestId, reinterpret_cast<LPARAM>(payload.get()))) {
        payload.release(); // the UI thread owns it now
    }
}

} // namespace

FailureText DescribeFailure(const TranslateOutcome& outcome, std::wstring_view providerName)
{
    const std::wstring provider(providerName);
    const std::wstring http = L" (HTTP " + std::to_wstring(outcome.httpStatus) + L")";
    switch (outcome.status) {
    case TranslateStatus::Ok:
        return {};
    case TranslateStatus::NoApiKey:
        return {L"No API key is set for " + provider + L".", L"Open Settings from the tray icon and add your key."};
    case TranslateStatus::Offline:
        return {L"Can't reach " + provider + L".", L"Check your internet connection."};
    case TranslateStatus::Timeout:
        return {provider + L" did not respond in time.", L"Try again in a moment."};
    case TranslateStatus::TlsFailure:
        return {L"A secure connection to " + provider + L" could not be established.",
                L"Check the system clock and any TLS-inspecting proxy."};
    case TranslateStatus::ResponseTooLarge:
        return {L"The response from " + provider + L" was too large.", L""};
    case TranslateStatus::NetworkError:
        return {L"Network error while contacting " + provider + L".", L"Try again in a moment."};
    case TranslateStatus::ProviderRejected:
        break;
    }
    switch (outcome.providerError) {
    case ProviderError::BadKey:
        return {L"Invalid " + provider + L" API key.", L"Open Settings from the tray icon and check the key."};
    case ProviderError::QuotaExceeded:
        return {provider + L" quota exceeded.", L"The character limit of your plan has been reached."};
    case ProviderError::RateLimited:
        return {L"Too many requests to " + provider + L".", L"Wait a moment and try again."};
    case ProviderError::BadRequest:
        return {provider + L" rejected the request" + http + L".", L"The language pair may not be supported."};
    case ProviderError::Server:
        return {provider + L" is having problems" + http + L".", L"Try again later."};
    case ProviderError::Parse:
        return {provider + L" returned a response that could not be read.", L""};
    }
    return {L"Translation failed.", L""};
}

TranslatorClient::~TranslatorClient()
{
    Stop();
}

bool TranslatorClient::Start()
{
    if (m_thread.joinable()) {
        return true;
    }
    m_stop = false;
    if (!m_wake.try_create(wil::EventOptions::None, nullptr)) {
        return false;
    }
    m_thread = std::thread([this] { ThreadMain(); });
    return true;
}

void TranslatorClient::Stop()
{
    if (!m_thread.joinable()) {
        return;
    }
    m_stop = true;
    Cancel();
    m_wake.SetEvent();
    m_thread.join();
}

void TranslatorClient::Submit(TranslateJob job)
{
    unique_winhttp abort;
    {
        auto guard = m_lock.lock_exclusive();
        ++m_generation;
        if (m_pending) {
            SecretStore::SecureClear(m_pending->apiKeyOverride);
        }
        m_pending = std::move(job);
        abort = std::move(m_inFlight);
    }
    // Closing the handle (outside the lock) makes the blocked WinHTTP call on the network
    // thread fail with ERROR_WINHTTP_OPERATION_CANCELLED.
    abort.reset();
    m_wake.SetEvent();
}

void TranslatorClient::Cancel()
{
    unique_winhttp abort;
    {
        auto guard = m_lock.lock_exclusive();
        ++m_generation;
        if (m_pending) {
            SecretStore::SecureClear(m_pending->apiKeyOverride);
            m_pending.reset();
        }
        abort = std::move(m_inFlight);
    }
    abort.reset();
}

bool TranslatorClient::IsCancelled(ULONGLONG generation)
{
    auto guard = m_lock.lock_shared();
    return generation != m_generation;
}

void TranslatorClient::ThreadMain()
{
    while (!m_stop) {
        WaitForSingleObject(m_wake.get(), INFINITE); // 0% CPU while idle
        if (m_stop) {
            break;
        }
        std::optional<TranslateJob> job;
        ULONGLONG generation = 0;
        {
            auto guard = m_lock.lock_exclusive();
            job.swap(m_pending);
            generation = m_generation;
        }
        if (!job) {
            continue;
        }
        std::optional<TranslateOutcome> outcome = Execute(*job, generation);
        SecretStore::SecureClear(job->apiKeyOverride);
        if (outcome && !IsCancelled(generation)) {
            PostOutcome(*job, std::move(*outcome));
        }
    }
    m_connections.clear();
    m_session.reset();
}

bool TranslatorClient::EnsureSession()
{
    if (m_session) {
        return true;
    }
    // Automatic proxy: respects the system / PAC proxy configuration.
    m_session.reset(WinHttpOpen(L"LightTranslate/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0));
    if (!m_session) {
        return false;
    }
    // TLS 1.3, with 1.2 kept for corporate TLS-inspecting proxies. Certificate checks stay on.
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    if (!WinHttpSetOption(m_session.get(), WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) {
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        WinHttpSetOption(m_session.get(), WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    }
    DWORD http2 = WINHTTP_PROTOCOL_FLAG_HTTP2;
    WinHttpSetOption(m_session.get(), WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL, &http2, sizeof(http2));
    DWORD decompression = WINHTTP_DECOMPRESSION_FLAG_GZIP | WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
    WinHttpSetOption(m_session.get(), WINHTTP_OPTION_DECOMPRESSION, &decompression, sizeof(decompression));
    // resolve 5 s, connect 5 s, send 5 s, receive 10 s
    WinHttpSetTimeouts(m_session.get(), 5000, 5000, 5000, 10000);
    return true;
}

HINTERNET TranslatorClient::ConnectionFor(const std::wstring& host)
{
    if (const auto it = m_connections.find(host); it != m_connections.end()) {
        return it->second.get();
    }
    unique_winhttp connection(WinHttpConnect(m_session.get(), host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connection) {
        return nullptr;
    }
    const HINTERNET raw = connection.get();
    m_connections.emplace(host, std::move(connection));
    return raw;
}

// Returns nullopt when the job was cancelled (nothing must be posted).
std::optional<TranslateOutcome> TranslatorClient::Execute(TranslateJob& job, ULONGLONG generation)
{
    TranslateOutcome outcome;
    if (!job.provider) {
        return outcome;
    }

    // The key lives in our memory only between here and WinHttpSendRequest.
    std::string key = !job.apiKeyOverride.empty() ? std::move(job.apiKeyOverride) : SecretStore::Read(job.provider->Id());
    job.apiKeyOverride.clear();
    if (key.empty()) {
        outcome.status = TranslateStatus::NoApiKey;
        return outcome;
    }
    HttpRequestSpec spec = job.provider->BuildRequest(job.request, key);
    SecretStore::SecureClear(key);
    auto wipeHeaders = wil::scope_exit([&spec] { SecretStore::SecureClear(spec.headers); });

    if (!EnsureSession()) {
        outcome.status = TranslateStatus::NetworkError;
        return outcome;
    }
    const HINTERNET connection = ConnectionFor(spec.host);
    if (!connection) {
        outcome.status = TranslateStatus::NetworkError;
        return outcome;
    }
    // WINHTTP_FLAG_SECURE on every request; the key travels in a header, never in the URL.
    unique_winhttp request(WinHttpOpenRequest(connection, L"POST", spec.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                              WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!request) {
        outcome.status = TranslateStatus::NetworkError;
        return outcome;
    }

    // Publish the handle so Cancel() can abort us. From here on, `raw` may be closed by the
    // UI thread at any moment; every call below then fails and we report "cancelled".
    const HINTERNET raw = request.get();
    {
        auto guard = m_lock.lock_exclusive();
        if (generation != m_generation) {
            return std::nullopt; // cancelled before we started; `request` closes itself
        }
        m_inFlight = std::move(request);
    }

    DWORD error = ERROR_SUCCESS;
    bool tooLarge = false;
    DWORD httpStatus = 0;
    std::string body;

    const DWORD bodySize = static_cast<DWORD>(spec.body.size());
    BOOL ok = WinHttpSendRequest(raw, spec.headers.c_str(), static_cast<DWORD>(spec.headers.size()),
                                 spec.body.empty() ? WINHTTP_NO_REQUEST_DATA : spec.body.data(), bodySize, bodySize, 0);
    if (!ok) {
        error = GetLastError();
    }
    wipeHeaders.reset(); // zero our copy of the auth header right after sending

    if (ok) {
        ok = WinHttpReceiveResponse(raw, nullptr);
        if (!ok) {
            error = GetLastError();
        }
    }
    if (ok) {
        DWORD size = sizeof(httpStatus);
        ok = WinHttpQueryHeaders(raw, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                 &httpStatus, &size, WINHTTP_NO_HEADER_INDEX);
        if (!ok) {
            error = GetLastError();
        }
    }
    while (ok) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(raw, &available)) {
            error = GetLastError();
            ok = FALSE;
            break;
        }
        if (available == 0) {
            break;
        }
        if (body.size() + available > kMaxResponseBytes) {
            tooLarge = true;
            break;
        }
        const size_t offset = body.size();
        body.resize(offset + available);
        DWORD read = 0;
        if (!WinHttpReadData(raw, body.data() + offset, available, &read)) {
            error = GetLastError();
            ok = FALSE;
            break;
        }
        body.resize(offset + read);
        if (read == 0) {
            break;
        }
    }

    // Retire the handle: whoever exchanges it out of the slot closes it, so exactly one close.
    unique_winhttp finished;
    bool cancelled = false;
    {
        auto guard = m_lock.lock_exclusive();
        finished = std::move(m_inFlight);
        cancelled = generation != m_generation;
    }
    finished.reset();
    if (cancelled) {
        return std::nullopt;
    }

    if (tooLarge) {
        outcome.status = TranslateStatus::ResponseTooLarge;
        return outcome;
    }
    if (!ok) {
        outcome.status = StatusFromWinHttpError(error);
        return outcome;
    }

    outcome.httpStatus = static_cast<int>(httpStatus);
    auto parsed = job.provider->ParseResponse(outcome.httpStatus, body);
    if (parsed.ok()) {
        outcome.status = TranslateStatus::Ok;
        outcome.result = std::move(parsed.value());
    } else {
        outcome.status = TranslateStatus::ProviderRejected;
        outcome.providerError = parsed.error();
    }
    return outcome;
}
