#pragma once
#include <windows.h>
#include <string>

// Private window messages routed through the hidden app window (UI thread).
// Worker threads post results as: PostMessageW(appHwnd, WM_APP_*, requestId, (LPARAM)payload.release()).
// The UI thread re-wraps the pointer in std::unique_ptr and drops it if requestId is stale.

inline constexpr UINT WM_APP_TRAY = WM_APP + 1;          // tray icon callback (NOTIFYICON_VERSION_4)
inline constexpr UINT WM_APP_OPEN_SETTINGS = WM_APP + 2; // posted by a second instance
inline constexpr UINT WM_APP_CAPTURE_DONE = WM_APP + 3;  // wParam = requestId, lParam = CaptureResult*
// wParam = requestId, lParam = TranslateOutcome* (payload struct: core/TranslatorClient.h)
inline constexpr UINT WM_APP_TRANSLATE_DONE = WM_APP + 4; // -> app window
inline constexpr UINT WM_APP_TEST_DONE = WM_APP + 5;      // -> Settings dialog ("Test" button)

// ---- WM_APP_CAPTURE_DONE payload ---------------------------------------------------------

enum class CaptureStatus {
    Ok,
    NoSelection,            // nothing selected, or the target runs elevated (UIPI blocks us)
    NoSelectionFallbackOff, // UI Automation found nothing and the clipboard fallback is disabled
    PasswordField,          // focused element is a password box: refused
    SecretContent,          // clipboard content was marked ExcludeClipboardContentFromMonitorProcessing
    ModifiersHeld,          // hotkey modifiers were still held after 1 s
    ClipboardBusy,          // could not open the clipboard for the snapshot
};

struct CaptureResult {
    CaptureStatus status = CaptureStatus::NoSelection;
    std::wstring text;         // normalized selection (status == Ok)
    bool truncated = false;    // text was cut at the character limit
    bool viaClipboard = false; // tier 2 was used
};
