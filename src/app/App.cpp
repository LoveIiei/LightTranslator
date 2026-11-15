#include "app/App.h"

#include "app/AppInfo.h"
#include "core/Encoding.h"
#include "core/SecretStore.h"
#include "providers/Languages.h"
#include "ui/Theme.h"

#include <windowsx.h>

namespace {

enum TrayCommand : UINT {
    kCmdSettings = 1,
    kCmdPause,
    kCmdAutostart,
    kCmdQuit,
};

constexpr size_t kCacheEntries = 256;
constexpr size_t kCacheBytes = 2 * 1024 * 1024;
constexpr wchar_t kKeySeparator = L'\x1F';

size_t ResultBytes(const TranslateResult& result)
{
    return result.text.size() * sizeof(wchar_t) + result.detectedSourceLang.size();
}

// User-facing text for a capture that produced no text. Never silent.
std::wstring CaptureErrorText(CaptureStatus status)
{
    switch (status) {
    case CaptureStatus::PasswordField:
        return L"Password fields are not translated.";
    case CaptureStatus::SecretContent:
        return L"The copied content is marked as secret and was not translated.";
    case CaptureStatus::ModifiersHeld:
        return L"Release the hotkey keys and try again.";
    case CaptureStatus::ClipboardBusy:
        return L"The clipboard is in use by another application. Try again.";
    case CaptureStatus::NoSelectionFallbackOff:
        return L"No selected text found. This app may need the clipboard fallback (enable it in Settings).";
    case CaptureStatus::NoSelection:
    case CaptureStatus::Ok:
        break;
    }
    return L"No selected text found (apps running as administrator can't be read).";
}

std::wstring LanguageLabel(const std::string& code)
{
    std::wstring name(Languages::DisplayName(code));
    return name.empty() ? Encoding::FromUtf8(code) : name;
}

} // namespace

App::App(HINSTANCE instance) : m_instance(instance), m_cache(kCacheEntries, kCacheBytes, &ResultBytes) {}

int App::Run()
{
    m_settings.Load();
    m_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");

    if (!CreateAppWindow()) {
        return 1;
    }
    m_tray.Add(m_hwnd.get(), m_instance, Theme::TaskbarIsLight());
    UpdateTrayTooltip();

    // The popup is created once and stays hidden; a failure here is not recoverable.
    if (!m_popup.Create(m_instance, this)) {
        MessageBoxW(nullptr, L"LightTranslate could not create its popup window.", AppInfo::kName, MB_ICONERROR);
        return 1;
    }
    RefreshPopupLanguages();
    m_popup.SetPinned(m_settings.popupPinned, m_settings.hasPinPosition, POINT{m_settings.pinX, m_settings.pinY});
    m_popup.SetBackdropAllowed(m_settings.useBackdrop);
    m_sessionSource = m_settings.sourceLang;
    m_sessionTarget = m_settings.targetLang;

    m_capture.Start(m_hwnd.get());
    m_translator.Start();
    RegisterHotkeyOrReport();

    // First run: no key yet -> open Settings with a short explanation instead of failing later.
    if (!SecretStore::Exists(Provider().Id())) {
        OpenSettings(SettingsDialog::Reason::FirstRun);
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (const HWND dialog = m_settingsDialog.Hwnd(); dialog && IsDialogMessageW(dialog, &msg)) {
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

bool App::CreateAppWindow()
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &App::WndProc;
    wc.hInstance = m_instance;
    wc.lpszClassName = AppInfo::kWindowClass;
    if (!RegisterClassExW(&wc)) {
        return false;
    }
    // A real (never shown) top-level window: message-only windows don't get TaskbarCreated,
    // WM_SETTINGCHANGE or WM_ENDSESSION broadcasts.
    m_hwnd.reset(CreateWindowExW(WS_EX_TOOLWINDOW, AppInfo::kWindowClass, AppInfo::kName, WS_POPUP, 0, 0, 0, 0, nullptr,
                                 nullptr, m_instance, this));
    return static_cast<bool>(m_hwnd);
}

LRESULT CALLBACK App::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    if (auto* self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA))) {
        return self->HandleMessage(hwnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT App::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == m_taskbarCreatedMsg && msg != 0) {
        // Explorer restarted: the notification area is empty again.
        m_tray.Add(hwnd, m_instance, Theme::TaskbarIsLight());
        return 0;
    }

    switch (msg) {
    case WM_APP_TRAY:
        // NOTIFYICON_VERSION_4: LOWORD(lParam) = event, wParam = anchor x/y.
        switch (LOWORD(lParam)) {
        case WM_CONTEXTMENU:
            ShowTrayMenu(POINT{GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam)});
            break;
        case NIN_SELECT:
        case NIN_KEYSELECT:
            OpenSettings();
            break;
        }
        return 0;

    case WM_APP_OPEN_SETTINGS:
        OpenSettings();
        return 0;

    case WM_HOTKEY:
        if (wParam == HotkeyManager::kHotkeyId) {
            OnHotkey();
        }
        return 0;

    case WM_APP_CAPTURE_DONE:
        // Re-wrap first so a stale result is still freed.
        OnCaptureDone(static_cast<UINT>(wParam), std::unique_ptr<CaptureResult>(reinterpret_cast<CaptureResult*>(lParam)));
        return 0;

    case WM_APP_TRANSLATE_DONE:
        OnTranslateDone(static_cast<UINT>(wParam),
                        std::unique_ptr<TranslateOutcome>(reinterpret_cast<TranslateOutcome*>(lParam)));
        return 0;

    case WM_SETTINGCHANGE:
        // Light/dark and transparency arrive as "ImmersiveColorSet"; contrast themes as SPI_SETHIGHCONTRAST.
        if (wParam == SPI_SETHIGHCONTRAST ||
            (lParam != 0 && wcscmp(reinterpret_cast<PCWSTR>(lParam), L"ImmersiveColorSet") == 0)) {
            OnThemeChanged();
        }
        return 0;

    case WM_SYSCOLORCHANGE:
    case WM_THEMECHANGED:
        OnThemeChanged();
        return 0;

    case WM_ENDSESSION:
        if (wParam) {
            Shutdown();
        }
        return 0;

    case WM_DESTROY:
        Shutdown();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---- Tray --------------------------------------------------------------------------------

void App::ShowTrayMenu(POINT pt)
{
    wil::unique_hmenu menu(CreatePopupMenu());
    if (!menu) {
        return;
    }
    AppendMenuW(menu.get(), MF_STRING, kCmdSettings, L"Settings…");
    AppendMenuW(menu.get(), MF_STRING | (m_paused ? MF_CHECKED : 0), kCmdPause, L"Pause hotkey");
    AppendMenuW(menu.get(), MF_STRING | (Settings::IsAutostartEnabled() ? MF_CHECKED : 0), kCmdAutostart,
                L"Start with Windows");
    AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu.get(), MF_STRING, kCmdQuit, L"Quit");

    // Required so the menu dismisses when the user clicks elsewhere.
    SetForegroundWindow(m_hwnd.get());
    const UINT flags = TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY |
                       (GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN);
    const UINT cmd = static_cast<UINT>(TrackPopupMenuEx(menu.get(), flags, pt.x, pt.y, m_hwnd.get(), nullptr));
    PostMessageW(m_hwnd.get(), WM_NULL, 0, 0);

    switch (cmd) {
    case kCmdSettings:
        OpenSettings();
        break;
    case kCmdPause:
        TogglePause();
        break;
    case kCmdAutostart:
        ToggleAutostart();
        break;
    case kCmdQuit:
        m_hwnd.reset(); // DestroyWindow -> WM_DESTROY -> Shutdown + PostQuitMessage
        break;
    }
}

void App::OpenSettings(SettingsDialog::Reason reason)
{
    m_settingsDialog.Open(m_instance, this, reason);
}

void App::TogglePause()
{
    m_paused = !m_paused;
    if (m_paused) {
        m_hotkey.Unregister();
    } else {
        RegisterHotkeyOrReport();
    }
    UpdateTrayTooltip();
}

void App::ToggleAutostart()
{
    const bool enable = !Settings::IsAutostartEnabled();
    if (Settings::SetAutostart(enable)) {
        m_settings.startWithWindows = enable;
        m_settings.Save();
    }
}

void App::OnThemeChanged()
{
    m_tray.SetTheme(Theme::TaskbarIsLight());
    m_popup.OnThemeChanged();
}

void App::Shutdown()
{
    if (m_shutDown) {
        return;
    }
    m_shutDown = true;
    m_hotkey.Unregister();
    m_settingsDialog.Close();
    m_capture.Stop();
    m_translator.Stop();
    m_popup.Destroy();
    m_cache.Clear();
    m_tray.Remove();
}

// ---- Hotkey ------------------------------------------------------------------------------

bool App::RegisterHotkeyOrReport()
{
    if (m_paused) {
        return true;
    }
    if (m_hotkey.Register(m_hwnd.get(), m_settings.hotkeyModifiers, m_settings.hotkeyVk)) {
        UpdateTrayTooltip();
        return true;
    }
    // Never fail silently: notification + Settings with the conflict shown inline.
    const std::wstring name = HotkeyManager::Describe(m_settings.hotkeyModifiers, m_settings.hotkeyVk);
    const std::wstring text = (m_hotkey.LastError() == ERROR_HOTKEY_ALREADY_REGISTERED)
                                  ? name + L" is already used by another application. Choose a different hotkey in Settings."
                                  : name + L" could not be registered. Choose a different hotkey in Settings.";
    m_tray.ShowBalloon(L"Hotkey unavailable", text, true);
    UpdateTrayTooltip();
    OpenSettings(SettingsDialog::Reason::HotkeyConflict);
    return false;
}

void App::UpdateTrayTooltip()
{
    std::wstring tip = AppInfo::kName;
    if (m_paused) {
        tip += L" (hotkey paused)";
    } else if (m_hotkey.IsRegistered()) {
        tip += L" (" + HotkeyManager::Describe(m_settings.hotkeyModifiers, m_settings.hotkeyVk) + L")";
    } else {
        tip += L" (hotkey unavailable)";
    }
    m_tray.SetTooltip(tip);
}

void App::OnHotkey()
{
    // Pressing the hotkey while the popup has focus dismisses it. (A pinned popup that is not
    // focused falls through: the user selected new text elsewhere.)
    if (m_popup.IsVisible() && m_popup.IsForeground()) {
        m_popup.Hide();
        return;
    }

    // 1. Anchor: the caret of the foreground thread if it has one, else the mouse cursor.
    const HWND foreground = GetForegroundWindow();
    bool haveAnchor = false;
    GUITHREADINFO gui{};
    gui.cbSize = sizeof(gui);
    if (foreground && GetGUIThreadInfo(GetWindowThreadProcessId(foreground, nullptr), &gui) && gui.hwndCaret &&
        !IsRectEmpty(&gui.rcCaret)) {
        POINT topLeft{gui.rcCaret.left, gui.rcCaret.top};
        POINT bottomRight{gui.rcCaret.right, gui.rcCaret.bottom};
        if (ClientToScreen(gui.hwndCaret, &topLeft) && ClientToScreen(gui.hwndCaret, &bottomRight)) {
            m_anchor = RECT{topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
            haveAnchor = true;
        }
    }
    if (!haveAnchor) {
        POINT cursor{};
        GetCursorPos(&cursor);
        m_anchor = RECT{cursor.x, cursor.y, cursor.x, cursor.y};
    }

    // 2. New request id (older results become stale), cancel any in-flight translation, then
    //    hand the job to the capture thread.
    ++m_requestId;
    m_translator.Cancel();
    m_sourceText.clear();
    m_truncated = false;
    m_haveResult = false;
    m_sessionSource = m_settings.sourceLang;
    m_sessionTarget = m_settings.targetLang;

    CaptureJob job;
    job.requestId = m_requestId;
    job.allowClipboardFallback = m_settings.allowClipboardFallback;
    job.maxChars = m_settings.maxChars;
    m_capture.Post(job);

    // 3. Show the popup right away (Loading). It must not take focus yet: the capture thread
    //    still needs the foreground application to own the focus and the selection.
    UpdatePopupHeader({});
    m_popup.ShowLoading(m_anchor);
}

void App::OnCaptureDone(UINT requestId, std::unique_ptr<CaptureResult> result)
{
    if (!result || requestId != m_requestId || !m_popup.IsVisible()) {
        return; // stale
    }
    // The capture no longer needs the other application's focus: take it so Esc / Ctrl+C work.
    m_popup.Activate();

    if (result->status != CaptureStatus::Ok) {
        const bool fallbackOff = result->status == CaptureStatus::NoSelectionFallbackOff;
        m_popup.SetError(CaptureErrorText(result->status), fallbackOff ? L"Tray icon → Settings…" : L"");
        return;
    }
    m_sourceText = std::move(result->text);
    m_truncated = result->truncated;
    m_popup.SetSourceText(m_sourceText); // editable: the user can correct it and re-translate
    StartTranslation();
}

// ---- Translation -------------------------------------------------------------------------

const ITranslationProvider& App::Provider() const
{
    return Providers::FindOrDefault(m_settings.providerId);
}

std::string App::EffectiveTarget() const
{
    return Providers::ResolveTarget(Provider(), m_sessionTarget);
}

void App::RefreshPopupLanguages()
{
    std::vector<PopupLanguage> targets;
    for (const LanguageCode& lang : Provider().SupportedTargets()) {
        targets.push_back(PopupLanguage{std::string(lang.code), std::wstring(lang.name)});
    }
    m_popup.SetTargets(std::move(targets));
}

void App::UpdatePopupHeader(const std::string& detectedSource)
{
    std::wstring source;
    if (!detectedSource.empty()) {
        source = LanguageLabel(detectedSource);
    } else if (!m_sessionSource.empty()) {
        source = LanguageLabel(m_sessionSource);
    } else {
        source = L"Auto-detect";
    }
    const std::string target = EffectiveTarget();
    m_popup.SetHeader(std::move(source), target, LanguageLabel(target), std::wstring(Provider().DisplayName()));
}

void App::StartTranslation()
{
    if (m_sourceText.empty()) {
        return;
    }
    const ITranslationProvider& provider = Provider();
    const std::string target = EffectiveTarget();

    // Cache key = provider id + source + target + full source text.
    std::wstring key(provider.Id());
    key += kKeySeparator;
    key += Encoding::FromUtf8(m_sessionSource);
    key += kKeySeparator;
    key += Encoding::FromUtf8(target);
    key += kKeySeparator;
    key += m_sourceText;

    if (const TranslateResult* cached = m_cache.Find(key)) {
        ShowTranslation(*cached); // no network traffic
        return;
    }
    m_pendingCacheKey = std::move(key);
    m_popup.SetLoading();

    TranslateJob job;
    job.requestId = m_requestId;
    job.provider = &provider;
    job.request.text = m_sourceText;
    job.request.sourceLang = m_sessionSource;
    job.request.targetLang = target;
    if (m_settings.deeplPlan == L"free" || m_settings.deeplPlan == L"pro") {
        job.request.plan = Encoding::ToUtf8(m_settings.deeplPlan);
    }
    job.replyTo = m_hwnd.get();
    job.replyMessage = WM_APP_TRANSLATE_DONE;
    m_translator.Submit(std::move(job));
}

void App::OnTranslateDone(UINT requestId, std::unique_ptr<TranslateOutcome> outcome)
{
    if (!outcome || requestId != m_requestId || !m_popup.IsVisible()) {
        return; // stale: only the newest request may update the popup
    }
    if (outcome->status != TranslateStatus::Ok) {
        FailureText failure = DescribeFailure(*outcome, Provider().DisplayName());
        m_popup.SetError(std::move(failure.message), std::move(failure.hint));
        return;
    }
    ShowTranslation(outcome->result);
    if (!m_pendingCacheKey.empty()) {
        m_cache.Put(std::move(m_pendingCacheKey), std::move(outcome->result));
        m_pendingCacheKey.clear();
    }
}

void App::ShowTranslation(const TranslateResult& result)
{
    m_lastResult = result; // kept for "swap languages"
    m_haveResult = true;
    UpdatePopupHeader(result.detectedSourceLang);
    std::wstring note;
    if (m_truncated) {
        note = L"Truncated to " + std::to_wstring(m_settings.maxChars) + L" characters";
    }
    m_popup.SetResult(result.text, Languages::IsRtl(EffectiveTarget()), std::move(note));
}

// ---- IPopupHost --------------------------------------------------------------------------

void App::OnPopupHidden()
{
    ++m_requestId; // anything still in flight is now stale
    m_translator.Cancel();
}

void App::OnPopupTargetChosen(const std::string& code)
{
    m_sessionTarget = code;
    m_settings.targetLang = code; // an explicit choice is remembered
    m_settings.Save();
    ++m_requestId;
    m_haveResult = false;
    UpdatePopupHeader({});
    StartTranslation();
}

void App::OnPopupSourceEdited(const std::wstring& text)
{
    bool truncated = false;
    std::wstring normalized = TextCapture::Normalize(text, m_settings.maxChars, truncated);
    if (normalized == m_sourceText) {
        return; // whitespace-only change
    }
    m_sourceText = std::move(normalized);
    m_truncated = truncated;
    m_haveResult = false;
    ++m_requestId; // whatever was in flight is stale now
    m_translator.Cancel();
    if (m_sourceText.empty()) {
        UpdatePopupHeader({});
        m_popup.SetResult(L"", false, L"");
        return;
    }
    StartTranslation();
}

void App::OnPopupSwapLanguages()
{
    // The languages trade places at any time, also with an empty box or after an error. If a
    // translation is showing it becomes the new source text (like the web translators).
    const bool sourceWasExplicit = !m_sessionSource.empty();
    const std::string previousTarget = EffectiveTarget();
    std::string previousSource = m_sessionSource;
    if (previousSource.empty() && m_haveResult) {
        previousSource = m_lastResult.detectedSourceLang; // auto-detect: what the provider found
    }
    if (previousSource.empty()) {
        // Auto-detect with nothing detected yet: English is the most useful guess.
        previousSource = "en";
    }
    const std::string newTarget = Providers::ResolveTarget(Provider(), previousSource);
    if (Languages::EqualsIgnoreCase(newTarget, previousTarget)) {
        return; // both sides are the same language: nothing to swap
    }
    m_sessionTarget = newTarget;
    m_sessionSource = previousTarget;

    // A fixed language pair is remembered in its new direction (otherwise the next hotkey
    // press would flip it back). With auto-detect the swap lasts for this popup session only,
    // so auto-detect is not silently turned off.
    if (sourceWasExplicit) {
        m_settings.sourceLang = m_sessionSource;
        m_settings.targetLang = m_sessionTarget;
        m_settings.Save();
    }

    ++m_requestId; // whatever was in flight is stale now
    m_translator.Cancel();
    if (m_haveResult && !m_lastResult.text.empty()) {
        bool truncated = false;
        m_sourceText = TextCapture::Normalize(m_lastResult.text, m_settings.maxChars, truncated);
        m_truncated = truncated;
        m_popup.SetSourceText(m_sourceText);
    }
    m_haveResult = false;

    UpdatePopupHeader({});
    StartTranslation(); // no-op while the source box is empty
}

void App::OnPopupPinChanged(bool pinned, POINT position)
{
    m_settings.popupPinned = pinned;
    m_settings.hasPinPosition = pinned;
    m_settings.pinX = position.x;
    m_settings.pinY = position.y;
    m_settings.Save();
}

// ---- ISettingsHost -----------------------------------------------------------------------

std::wstring App::ApplySettings(const Settings& updated, bool apiKeyChanged)
{
    const Settings previous = m_settings;
    m_settings = updated;

    if (Settings::SetAutostart(updated.startWithWindows)) {
        m_settings.startWithWindows = updated.startWithWindows;
    } else {
        m_settings.startWithWindows = Settings::IsAutostartEnabled();
    }

    // The cache is keyed by provider/languages, but a new key or provider can change results
    // (plan, glossary, account), so drop everything.
    if (apiKeyChanged || previous.providerId != m_settings.providerId || previous.sourceLang != m_settings.sourceLang ||
        previous.targetLang != m_settings.targetLang) {
        m_cache.Clear();
    }
    RefreshPopupLanguages();

    std::wstring problem;
    const bool hotkeyChanged =
        previous.hotkeyModifiers != m_settings.hotkeyModifiers || previous.hotkeyVk != m_settings.hotkeyVk;
    if (!m_paused && (hotkeyChanged || !m_hotkey.IsRegistered())) {
        if (!m_hotkey.Register(m_hwnd.get(), m_settings.hotkeyModifiers, m_settings.hotkeyVk)) {
            problem = HotkeyManager::Describe(m_settings.hotkeyModifiers, m_settings.hotkeyVk) +
                      ((m_hotkey.LastError() == ERROR_HOTKEY_ALREADY_REGISTERED)
                           ? L" is already used by another application. Choose a different hotkey."
                           : L" could not be registered. Choose a different hotkey.");
            // Keep the previous (working) hotkey rather than leaving the app without one.
            m_settings.hotkeyModifiers = previous.hotkeyModifiers;
            m_settings.hotkeyVk = previous.hotkeyVk;
            m_hotkey.Register(m_hwnd.get(), m_settings.hotkeyModifiers, m_settings.hotkeyVk);
        }
    }
    m_settings.Save();
    UpdateTrayTooltip();
    return problem;
}

void App::TestTranslation(HWND replyTo, UINT testId, const ITranslationProvider& provider, std::string apiKey,
                          const std::string& targetLang)
{
    TranslateJob job;
    job.requestId = testId;
    job.provider = &provider;
    job.request.text = L"Hello";
    job.request.targetLang = Providers::ResolveTarget(provider, targetLang);
    if (m_settings.deeplPlan == L"free" || m_settings.deeplPlan == L"pro") {
        job.request.plan = Encoding::ToUtf8(m_settings.deeplPlan);
    }
    job.replyTo = replyTo;
    job.replyMessage = WM_APP_TEST_DONE;
    job.apiKeyOverride = std::move(apiKey);
    m_translator.Submit(std::move(job));
}
