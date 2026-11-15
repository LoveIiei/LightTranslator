#include "ui/SettingsDialog.h"

#include "app/Messages.h"
#include "core/Encoding.h"
#include "core/HotkeyManager.h"
#include "core/SecretStore.h"
#include "core/TranslatorClient.h"
#include "resource.h"

#include <commctrl.h>
#include <memory>

namespace {

constexpr COLORREF kErrorColor = RGB(196, 43, 28);

// HOTKEY_CLASS only knows Shift/Ctrl/Alt (hence the separate "Win +" checkbox).
BYTE ToHotkeyFlags(UINT modifiers)
{
    BYTE flags = 0;
    if (modifiers & MOD_SHIFT) flags |= HOTKEYF_SHIFT;
    if (modifiers & MOD_CONTROL) flags |= HOTKEYF_CONTROL;
    if (modifiers & MOD_ALT) flags |= HOTKEYF_ALT;
    return flags;
}

UINT FromHotkeyFlags(BYTE flags)
{
    UINT modifiers = 0;
    if (flags & HOTKEYF_SHIFT) modifiers |= MOD_SHIFT;
    if (flags & HOTKEYF_CONTROL) modifiers |= MOD_CONTROL;
    if (flags & HOTKEYF_ALT) modifiers |= MOD_ALT;
    return modifiers;
}

void AddComboItem(HWND combo, std::wstring_view text, const LanguageCode* data)
{
    const std::wstring label(text);
    const LRESULT index = SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    if (index >= 0) {
        SendMessageW(combo, CB_SETITEMDATA, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(data));
    }
}

void SelectComboByCode(HWND combo, std::string_view code, int fallbackIndex)
{
    const int count = static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0));
    for (int i = 0; i < count; ++i) {
        const auto* lang = reinterpret_cast<const LanguageCode*>(SendMessageW(combo, CB_GETITEMDATA, static_cast<WPARAM>(i), 0));
        if ((lang == nullptr && code.empty()) || (lang != nullptr && Languages::EqualsIgnoreCase(lang->code, code))) {
            SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(i), 0);
            return;
        }
    }
    SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(fallbackIndex), 0);
}

} // namespace

void SettingsDialog::Open(HINSTANCE instance, ISettingsHost* host, Reason reason)
{
    m_instance = instance;
    m_host = host;
    m_reason = reason;
    if (!m_hwnd) {
        INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_HOTKEY_CLASS | ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&icc);
        CreateDialogParamW(instance, MAKEINTRESOURCEW(IDD_SETTINGS), nullptr, &SettingsDialog::DialogProc,
                           reinterpret_cast<LPARAM>(this));
        if (!m_hwnd) {
            return;
        }
    } else {
        ApplyReason();
    }
    ShowWindow(m_hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(m_hwnd);
}

void SettingsDialog::Close()
{
    if (m_hwnd) {
        DestroyWindow(m_hwnd);
    }
}

INT_PTR CALLBACK SettingsDialog::DialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_INITDIALOG) {
        SetWindowLongPtrW(hwnd, DWLP_USER, lParam);
        auto* self = reinterpret_cast<SettingsDialog*>(lParam);
        self->m_hwnd = hwnd;
        self->OnInit();
        return TRUE;
    }
    if (auto* self = reinterpret_cast<SettingsDialog*>(GetWindowLongPtrW(hwnd, DWLP_USER))) {
        return self->HandleMessage(msg, wParam, lParam);
    }
    return FALSE;
}

INT_PTR SettingsDialog::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_PROVIDER:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                OnProviderChanged();
            }
            return TRUE;
        case IDC_TEST:
            OnTest();
            return TRUE;
        case IDOK:
            OnSave();
            return TRUE;
        case IDCANCEL:
            DestroyWindow(m_hwnd);
            return TRUE;
        }
        break;

    case WM_APP_TEST_DONE:
        OnTestDone(static_cast<UINT>(wParam), lParam);
        return TRUE;

    case WM_CTLCOLORSTATIC: {
        // Inline errors (failed test, hotkey conflict) are shown in red.
        const int id = GetDlgCtrlID(reinterpret_cast<HWND>(lParam));
        if ((id == IDC_TEST_RESULT && m_testError) || (id == IDC_HOTKEY_STATUS && m_hotkeyError)) {
            const HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, kErrorColor);
            SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_3DFACE));
        }
        break;
    }

    case WM_CLOSE:
        DestroyWindow(m_hwnd);
        return TRUE;

    case WM_DESTROY: {
        SetDlgItemTextW(m_hwnd, IDC_APIKEY, L""); // don't leave a typed key in the control's buffer
        m_hwnd = nullptr;
        ++m_testId; // a late test result is ignored
        if (m_host) {
            m_host->OnSettingsClosed();
        }
        return TRUE;
    }
    }
    return FALSE;
}

void SettingsDialog::OnInit()
{
    const Settings& settings = m_host->CurrentSettings();

    if (const HICON icon = LoadIconW(m_instance, MAKEINTRESOURCEW(IDI_APP))) {
        SendMessageW(m_hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
        SendMessageW(m_hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
    }

    const HWND providerCombo = GetDlgItem(m_hwnd, IDC_PROVIDER);
    int selected = 0;
    int index = 0;
    for (const auto& provider : Providers::All()) {
        const std::wstring name(provider->DisplayName());
        SendMessageW(providerCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
        if (provider->Id() == settings.providerId) {
            selected = index;
        }
        ++index;
    }
    SendMessageW(providerCombo, CB_SETCURSEL, static_cast<WPARAM>(selected), 0);

    SendDlgItemMessageW(m_hwnd, IDC_APIKEY, EM_SETLIMITTEXT, 255, 0);
    UpdateKeyCue();
    FillLanguages(settings.sourceLang, settings.targetLang);

    SendDlgItemMessageW(m_hwnd, IDC_HOTKEY, HKM_SETHOTKEY,
                        MAKEWORD(static_cast<BYTE>(settings.hotkeyVk), ToHotkeyFlags(settings.hotkeyModifiers)), 0);
    CheckDlgButton(m_hwnd, IDC_HOTKEY_WIN, (settings.hotkeyModifiers & MOD_WIN) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(m_hwnd, IDC_AUTOSTART, Settings::IsAutostartEnabled() ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(m_hwnd, IDC_FALLBACK, settings.allowClipboardFallback ? BST_CHECKED : BST_UNCHECKED);

    ApplyReason();
}

void SettingsDialog::ApplyReason()
{
    const Settings& settings = m_host->CurrentSettings();
    const std::wstring hotkey = HotkeyManager::Describe(settings.hotkeyModifiers, settings.hotkeyVk);
    switch (m_reason) {
    case Reason::FirstRun:
        // First run: explain instead of failing on the first hotkey press.
        SetDlgItemTextW(m_hwnd, IDC_INTRO,
                        (L"Welcome! LightTranslate needs an API key from DeepL or Google Cloud Translation. Choose a "
                         L"provider, paste your key, press Test, then Save. After that, select text in any app and press " +
                         hotkey + L".")
                            .c_str());
        break;
    case Reason::HotkeyConflict:
        ShowHotkeyConflict();
        [[fallthrough]];
    case Reason::Normal:
        if (GetWindowTextLengthW(GetDlgItem(m_hwnd, IDC_INTRO)) == 0) {
            SetDlgItemTextW(m_hwnd, IDC_INTRO,
                            (L"Select text in any app and press " + hotkey +
                             L" to translate it. API keys are stored in Windows Credential Manager.")
                                .c_str());
        }
        break;
    }
}

void SettingsDialog::ShowHotkeyConflict()
{
    const Settings& settings = m_host->CurrentSettings();
    SetStatus(IDC_HOTKEY_STATUS,
              HotkeyManager::Describe(settings.hotkeyModifiers, settings.hotkeyVk) +
                  L" is already used by another application. Choose a different hotkey.",
              true);
}

const ITranslationProvider& SettingsDialog::SelectedProvider() const
{
    const auto providers = Providers::All();
    const LRESULT index = SendDlgItemMessageW(m_hwnd, IDC_PROVIDER, CB_GETCURSEL, 0, 0);
    if (index >= 0 && static_cast<size_t>(index) < providers.size()) {
        return *providers[static_cast<size_t>(index)];
    }
    return *providers.front();
}

void SettingsDialog::FillLanguages(const std::string& source, const std::string& target)
{
    const ITranslationProvider& provider = SelectedProvider();
    const HWND sourceCombo = GetDlgItem(m_hwnd, IDC_SOURCE);
    const HWND targetCombo = GetDlgItem(m_hwnd, IDC_TARGET);
    SendMessageW(sourceCombo, CB_RESETCONTENT, 0, 0);
    SendMessageW(targetCombo, CB_RESETCONTENT, 0, 0);

    AddComboItem(sourceCombo, L"Detect automatically", nullptr);
    for (const LanguageCode& lang : provider.SupportedTargets()) {
        AddComboItem(sourceCombo, lang.name, &lang);
        AddComboItem(targetCombo, lang.name, &lang);
    }
    SelectComboByCode(sourceCombo, source, 0);
    SelectComboByCode(targetCombo, Providers::ResolveTarget(provider, target), 0);
}

std::string SettingsDialog::SelectedLanguage(int comboId) const
{
    const HWND combo = GetDlgItem(m_hwnd, comboId);
    const LRESULT index = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (index < 0) {
        return {};
    }
    const auto* lang = reinterpret_cast<const LanguageCode*>(SendMessageW(combo, CB_GETITEMDATA, static_cast<WPARAM>(index), 0));
    return lang ? std::string(lang->code) : std::string{};
}

void SettingsDialog::UpdateKeyCue()
{
    // The stored key is never loaded into the dialog; an empty box means "keep the saved key".
    const bool hasKey = SecretStore::Exists(SelectedProvider().Id());
    SendDlgItemMessageW(m_hwnd, IDC_APIKEY, EM_SETCUEBANNER, TRUE,
                        reinterpret_cast<LPARAM>(hasKey ? L"Saved — leave empty to keep" : L"Paste your API key"));
}

std::string SettingsDialog::TakeTypedKey()
{
    wchar_t buffer[300] = {};
    const UINT length = GetDlgItemTextW(m_hwnd, IDC_APIKEY, buffer, ARRAYSIZE(buffer));
    std::string key = Encoding::ToUtf8(std::wstring_view(buffer, length));
    SecureZeroMemory(buffer, sizeof(buffer));
    // Trim whitespace from copy/paste.
    while (!key.empty() && static_cast<unsigned char>(key.back()) <= ' ') {
        key.pop_back();
    }
    size_t start = 0;
    while (start < key.size() && static_cast<unsigned char>(key[start]) <= ' ') {
        ++start;
    }
    if (start > 0) {
        std::string trimmed = key.substr(start);
        SecretStore::SecureClear(key);
        key = std::move(trimmed);
    }
    return key;
}

void SettingsDialog::SetStatus(int controlId, const std::wstring& text, bool isError)
{
    if (controlId == IDC_TEST_RESULT) {
        m_testError = isError;
    } else {
        m_hotkeyError = isError;
    }
    SetDlgItemTextW(m_hwnd, controlId, text.c_str());
    InvalidateRect(GetDlgItem(m_hwnd, controlId), nullptr, TRUE);
}

void SettingsDialog::OnProviderChanged()
{
    const std::string source = SelectedLanguage(IDC_SOURCE);
    const std::string target = SelectedLanguage(IDC_TARGET);
    FillLanguages(source, target);
    SetDlgItemTextW(m_hwnd, IDC_APIKEY, L""); // the box always refers to the selected provider
    UpdateKeyCue();
    SetStatus(IDC_TEST_RESULT, L"", false);
}

void SettingsDialog::OnTest()
{
    std::string key = TakeTypedKey();
    SetStatus(IDC_TEST_RESULT, L"Testing…", false);
    EnableWindow(GetDlgItem(m_hwnd, IDC_TEST), FALSE);
    m_host->TestTranslation(m_hwnd, ++m_testId, SelectedProvider(), std::move(key), SelectedLanguage(IDC_TARGET));
}

void SettingsDialog::OnTestDone(UINT testId, LPARAM payload)
{
    const std::unique_ptr<TranslateOutcome> outcome(reinterpret_cast<TranslateOutcome*>(payload));
    if (!outcome || testId != m_testId) {
        return;
    }
    EnableWindow(GetDlgItem(m_hwnd, IDC_TEST), TRUE);
    if (outcome->status == TranslateStatus::Ok) {
        SetStatus(IDC_TEST_RESULT, L"Works: “Hello” → “" + outcome->result.text + L"”", false);
    } else {
        const FailureText failure = DescribeFailure(*outcome, SelectedProvider().DisplayName());
        SetStatus(IDC_TEST_RESULT, failure.hint.empty() ? failure.message : failure.message + L" " + failure.hint, true);
    }
}

void SettingsDialog::OnSave()
{
    const ITranslationProvider& provider = SelectedProvider();

    Settings updated = m_host->CurrentSettings();
    updated.providerId = std::wstring(provider.Id());
    updated.sourceLang = SelectedLanguage(IDC_SOURCE);
    updated.targetLang = SelectedLanguage(IDC_TARGET);
    updated.startWithWindows = IsDlgButtonChecked(m_hwnd, IDC_AUTOSTART) == BST_CHECKED;
    updated.allowClipboardFallback = IsDlgButtonChecked(m_hwnd, IDC_FALLBACK) == BST_CHECKED;

    const WORD hotkey = LOWORD(SendDlgItemMessageW(m_hwnd, IDC_HOTKEY, HKM_GETHOTKEY, 0, 0));
    updated.hotkeyVk = LOBYTE(hotkey);
    updated.hotkeyModifiers = FromHotkeyFlags(HIBYTE(hotkey));
    if (IsDlgButtonChecked(m_hwnd, IDC_HOTKEY_WIN) == BST_CHECKED) {
        updated.hotkeyModifiers |= MOD_WIN;
    }
    if (updated.hotkeyVk == 0 || updated.hotkeyModifiers == 0) {
        SetStatus(IDC_HOTKEY_STATUS, L"Choose a key together with at least one of Win, Ctrl, Alt or Shift.", true);
        return;
    }

    // API key -> Credential Manager (never the registry). Empty box = keep the saved key.
    bool keyChanged = false;
    std::string key = TakeTypedKey();
    if (!key.empty()) {
        const bool stored = SecretStore::Write(provider.Id(), key);
        SecretStore::SecureClear(key);
        if (!stored) {
            SetStatus(IDC_TEST_RESULT, L"The API key could not be saved to Credential Manager.", true);
            return;
        }
        keyChanged = true;
        SetDlgItemTextW(m_hwnd, IDC_APIKEY, L"");
        UpdateKeyCue();
    }

    const std::wstring problem = m_host->ApplySettings(updated, keyChanged);
    if (!problem.empty()) {
        SetStatus(IDC_HOTKEY_STATUS, problem, true); // e.g. hotkey conflict: stay open
        return;
    }
    DestroyWindow(m_hwnd);
}
