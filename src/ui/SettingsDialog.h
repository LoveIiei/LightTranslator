#pragma once
#include <windows.h>
#include <string>

#include "app/Settings.h"
#include "providers/ITranslationProvider.h"

// What the dialog needs from the app (UI thread).
class ISettingsHost {
public:
    virtual const Settings& CurrentSettings() const = 0;

    // Persists and applies the settings (registry, autostart, hotkey re-registration, cache).
    // Returns an empty string on success, or a message to show inline (e.g. hotkey conflict).
    virtual std::wstring ApplySettings(const Settings& updated, bool apiKeyChanged) = 0;

    // Translates "Hello" on the network thread; the result is posted to `replyTo` as
    // WM_APP_TEST_DONE (wParam = testId). An empty apiKey means "use the stored key".
    virtual void TestTranslation(HWND replyTo, UINT testId, const ITranslationProvider& provider, std::string apiKey,
                                 const std::string& targetLang) = 0;

    virtual void OnSettingsClosed() = 0;

protected:
    ~ISettingsHost() = default;
};

// Modeless Settings dialog (res/app.rc, IDD_SETTINGS), opened from the tray.
class SettingsDialog {
public:
    enum class Reason { Normal, HotkeyConflict, FirstRun };

    // Opens the dialog, or brings the existing one to the front.
    void Open(HINSTANCE instance, ISettingsHost* host, Reason reason);
    void Close();
    HWND Hwnd() const { return m_hwnd; }

private:
    static INT_PTR CALLBACK DialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    INT_PTR HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    void OnInit();
    void OnProviderChanged();
    void OnTest();
    void OnTestDone(UINT testId, LPARAM payload);
    void OnSave();

    const ITranslationProvider& SelectedProvider() const;
    void FillLanguages(const std::string& source, const std::string& target);
    std::string SelectedLanguage(int comboId) const;
    void UpdateKeyCue();
    std::string TakeTypedKey();
    void SetStatus(int controlId, const std::wstring& text, bool isError);
    void ShowHotkeyConflict();
    void ApplyReason();

    HWND m_hwnd = nullptr;
    HINSTANCE m_instance = nullptr;
    ISettingsHost* m_host = nullptr;
    Reason m_reason = Reason::Normal;
    UINT m_testId = 0;
    bool m_testError = false;
    bool m_hotkeyError = false;
};
