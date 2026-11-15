#pragma once
#include <windows.h>
#include <ole2.h> // WIN32_LEAN_AND_MEAN drops the COM headers that uiautomation.h needs
#include <uiautomation.h>
#include <atomic>
#include <optional>
#include <string>
#include <thread>
#include <wil/com.h>
#include <wil/resource.h>

#include "app/Messages.h"

struct CaptureJob {
    UINT requestId = 0;
    bool allowClipboardFallback = true;
    size_t maxChars = 5000;
};

// Capture thread: reads the selected text of the foreground application.
//   Tier 1: UI Automation TextPattern (no clipboard, no input injection).
//   Tier 2: clipboard fallback (Ctrl+C with snapshot/restore), only if allowed.
// The thread owns a COM MTA, one IUIAutomation instance and a message-only window used as the
// clipboard owner/listener. It sleeps on an event and uses no CPU while idle.
// Results are posted to the app window as WM_APP_CAPTURE_DONE (wParam = requestId,
// lParam = CaptureResult* owned by the receiver).
class TextCapture {
public:
    TextCapture() = default;
    ~TextCapture();
    TextCapture(const TextCapture&) = delete;
    TextCapture& operator=(const TextCapture&) = delete;

    bool Start(HWND appHwnd);
    void Stop();

    // Queues a capture; a job that has not started yet is replaced (only the newest matters).
    void Post(const CaptureJob& job);

    // Trim, normalize line endings to '\n', cap at maxChars (never splitting a surrogate pair).
    static std::wstring Normalize(std::wstring text, size_t maxChars, bool& truncated);

private:
    enum class UiaOutcome { Text, Empty, Password };

    void ThreadMain();
    CaptureResult Capture(const CaptureJob& job);
    UiaOutcome CaptureViaUia(size_t maxChars, std::wstring& text);
    CaptureResult CaptureViaClipboard();
    bool EnsureUia();
    bool WaitForClipboardChange(DWORD sequenceBefore, DWORD timeoutMs);
    void PumpMessages();

    static LRESULT CALLBACK ClipboardWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    HWND m_appHwnd = nullptr;
    std::thread m_thread;
    wil::unique_event_nothrow m_wake;
    wil::srwlock m_lock;
    std::optional<CaptureJob> m_pending; // guarded by m_lock
    std::atomic<bool> m_stop{false};

    // Capture-thread state.
    wil::com_ptr_nothrow<IUIAutomation> m_uia;
    wil::com_ptr_nothrow<IUIAutomationTreeWalker> m_walker;
    HWND m_clipboardHwnd = nullptr;
    bool m_clipboardUpdated = false;
};
