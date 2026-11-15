#pragma once
#include <windows.h>
#include <winhttp.h>
#include <atomic>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <wil/resource.h>

#include "providers/ITranslationProvider.h"

using unique_winhttp = wil::unique_any<HINTERNET, decltype(&::WinHttpCloseHandle), ::WinHttpCloseHandle>;

enum class TranslateStatus {
    Ok,
    NoApiKey,         // nothing stored in Credential Manager for this provider
    Offline,          // name resolution / connect failed
    Timeout,
    TlsFailure,       // certificate or handshake problem (never ignored)
    NetworkError,     // any other transport error
    ResponseTooLarge, // > 1 MB
    ProviderRejected, // HTTP response received; see providerError / httpStatus
};

// Payload of WM_APP_TRANSLATE_DONE / WM_APP_TEST_DONE (wParam = requestId, lParam = TranslateOutcome*).
struct TranslateOutcome {
    TranslateStatus status = TranslateStatus::NetworkError;
    ProviderError providerError = ProviderError::Server;
    int httpStatus = 0;
    TranslateResult result;
};

struct TranslateJob {
    UINT requestId = 0;
    const ITranslationProvider* provider = nullptr;
    TranslateRequest request;
    HWND replyTo = nullptr;
    UINT replyMessage = 0;
    std::string apiKeyOverride; // Settings "Test" button: a key that is not saved yet; else empty
};

// User-facing text for a failed outcome. Never contains the API key.
struct FailureText {
    std::wstring message;
    std::wstring hint;
};
FailureText DescribeFailure(const TranslateOutcome& outcome, std::wstring_view providerName);

// Network thread: synchronous WinHTTP, one session for the app's lifetime, one connection
// handle per host (TLS connections are reused). Sleeps on an event while idle.
class TranslatorClient {
public:
    TranslatorClient() = default;
    ~TranslatorClient();
    TranslatorClient(const TranslatorClient&) = delete;
    TranslatorClient& operator=(const TranslatorClient&) = delete;

    bool Start();
    void Stop();

    // Replaces any queued job and aborts the request in flight (only the newest matters).
    void Submit(TranslateJob job);

    // Drops the queued job and aborts the request in flight; no result will be posted for them.
    void Cancel();

private:
    static constexpr size_t kMaxResponseBytes = 1024 * 1024;

    void ThreadMain();
    std::optional<TranslateOutcome> Execute(TranslateJob& job, ULONGLONG generation);
    bool EnsureSession();
    HINTERNET ConnectionFor(const std::wstring& host);
    bool IsCancelled(ULONGLONG generation);

    std::thread m_thread;
    wil::unique_event_nothrow m_wake;
    std::atomic<bool> m_stop{false};

    wil::srwlock m_lock;
    std::optional<TranslateJob> m_pending; // guarded by m_lock
    ULONGLONG m_generation = 0;            // guarded by m_lock; bumped by Submit/Cancel
    unique_winhttp m_inFlight;             // guarded by m_lock; closed by exactly one of Cancel / the worker

    // Network-thread state. Declaration order matters: connections are closed before the session.
    unique_winhttp m_session;
    std::map<std::wstring, unique_winhttp> m_connections;
};
