#pragma once
#include <windows.h>
#include <memory>
#include <string>
#include <wil/resource.h>

#include "app/Messages.h"
#include "app/Settings.h"
#include "core/HotkeyManager.h"
#include "core/LruCache.h"
#include "core/TextCapture.h"
#include "core/TranslatorClient.h"
#include "providers/ITranslationProvider.h"
#include "ui/PopupWindow.h"
#include "ui/SettingsDialog.h"
#include "ui/TrayIcon.h"

// Owns all modules and routes WM_APP_* messages on the UI thread.
class App final : private IPopupHost, private ISettingsHost {
public:
    explicit App(HINSTANCE instance);
    int Run();

private:
    // IPopupHost
    void OnPopupHidden() override;
    void OnPopupTargetChosen(const std::string& code) override;
    void OnPopupSourceEdited(const std::wstring& text) override;
    void OnPopupSwapLanguages() override;
    void OnPopupPinChanged(bool pinned, POINT position) override;

    // ISettingsHost
    const Settings& CurrentSettings() const override { return m_settings; }
    std::wstring ApplySettings(const Settings& updated, bool apiKeyChanged) override;
    void TestTranslation(HWND replyTo, UINT testId, const ITranslationProvider& provider, std::string apiKey,
                         const std::string& targetLang) override;
    void OnSettingsClosed() override {}

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    bool CreateAppWindow();
    void ShowTrayMenu(POINT pt);
    void OpenSettings(SettingsDialog::Reason reason = SettingsDialog::Reason::Normal);
    void TogglePause();
    void ToggleAutostart();
    void OnThemeChanged();
    void Shutdown();

    // Registers the configured hotkey; on conflict shows a notification and opens Settings.
    bool RegisterHotkeyOrReport();
    void UpdateTrayTooltip();
    void OnHotkey();
    void OnCaptureDone(UINT requestId, std::unique_ptr<CaptureResult> result);
    void OnTranslateDone(UINT requestId, std::unique_ptr<TranslateOutcome> outcome);

    const ITranslationProvider& Provider() const;
    std::string EffectiveTarget() const;
    void RefreshPopupLanguages();
    void UpdatePopupHeader(const std::string& detectedSource);
    void StartTranslation();
    void ShowTranslation(const TranslateResult& result);

    HINSTANCE m_instance;
    wil::unique_hwnd m_hwnd; // hidden top-level window (not message-only: it must receive broadcasts)
    UINT m_taskbarCreatedMsg = 0;
    Settings m_settings;
    TrayIcon m_tray;
    HotkeyManager m_hotkey;
    TextCapture m_capture;
    TranslatorClient m_translator;
    PopupWindow m_popup;
    SettingsDialog m_settingsDialog;
    bool m_paused = false;
    bool m_shutDown = false;

    UINT m_requestId = 0; // bumped per hotkey press / re-translate; results with another id are stale
    RECT m_anchor{};      // caret rect (screen) or a zero-size rect at the cursor

    // Current popup session. Source/target start from the settings on every hotkey press;
    // the language menu persists the target, and "swap" persists a fixed pair in its new
    // direction (with auto-detect as the source, a swap lasts for this session only).
    std::wstring m_sourceText;
    bool m_truncated = false;
    std::wstring m_pendingCacheKey;
    std::string m_sessionSource; // empty = auto-detect
    std::string m_sessionTarget;
    TranslateResult m_lastResult;
    bool m_haveResult = false;

    // Translations live in RAM only: 256 entries / ~2 MB, cleared on provider/key/language change and on quit.
    LruCache<TranslateResult> m_cache;
};
