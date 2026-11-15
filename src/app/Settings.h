#pragma once
#include <windows.h>
#include <string>

// Non-secret settings, stored under HKCU\Software\LightTranslate.
// API keys never go here; they live in Credential Manager (core/SecretStore).
struct Settings {
    UINT hotkeyModifiers = MOD_WIN | MOD_ALT; // MOD_* flags (MOD_NOREPEAT is added at registration)
    UINT hotkeyVk = 'T';
    std::wstring providerId = L"deepl";
    std::string sourceLang;                   // canonical code; empty = auto-detect
    std::string targetLang = "en-US";         // canonical code
    bool allowClipboardFallback = true;
    std::wstring deeplPlan = L"auto";         // auto | free | pro
    bool startWithWindows = false;
    UINT maxChars = 5000;                     // captured text is cut at this many UTF-16 units
    bool useBackdrop = true;                  // registry-only: 0 forces the solid popup background
    bool popupPinned = false;                 // pinned mode survives restarts...
    bool hasPinPosition = false;              // ...and remembers where the popup was
    int pinX = 0;
    int pinY = 0;

    void Load();
    void Save() const;

    // HKCU\...\CurrentVersion\Run entry holding the quoted exe path.
    static bool IsAutostartEnabled();
    static bool SetAutostart(bool enable);
};
