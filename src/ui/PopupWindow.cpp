#include "ui/PopupWindow.h"

#include "app/AppInfo.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <uiautomation.h>
#include <windowsx.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <new>

namespace {

constexpr wchar_t kPopupClass[] = L"LightTranslate.Popup";
constexpr wchar_t kFontFamily[] = L"Segoe UI Variable Text";

// Layout metrics in DIPs.
constexpr float kWidth = 420.0f;
constexpr float kPadX = 16.0f;
constexpr float kHeaderH = 38.0f;
constexpr float kBodyTopPad = 10.0f;
constexpr float kBodyBottomPad = 8.0f;
constexpr float kFooterH = 42.0f;
constexpr float kButtonH = 28.0f;
constexpr float kButtonPadX = 12.0f;
constexpr float kButtonGap = 4.0f;
constexpr float kMinBodyH = 22.0f;
constexpr float kHintGap = 6.0f;
constexpr float kAnchorGap = 8.0f;
constexpr float kMaxHeightFraction = 0.60f; // of the monitor work area
constexpr float kInputTopPad = 8.0f;
constexpr float kInputH = 58.0f;            // editable source box (about three lines)
constexpr UINT kLanguageMenuBase = 1000;

// One-shot timers; both only ever run while the popup is visible.
constexpr UINT_PTR kEditTimerId = 1;        // re-translate ~300 ms after the last edit
constexpr UINT kEditDelayMs = 300;
constexpr UINT_PTR kCopiedTimerId = 2;      // "Copied" feedback
constexpr UINT kCopiedDelayMs = 1500;
constexpr UINT_PTR kEditSubclassId = 1;

bool Contains(const D2D1_RECT_F& r, float x, float y)
{
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

float LayoutWidth(IDWriteTextLayout* layout)
{
    DWRITE_TEXT_METRICS m{};
    return (layout && SUCCEEDED(layout->GetMetrics(&m))) ? m.widthIncludingTrailingWhitespace : 0.0f;
}

float LayoutHeight(IDWriteTextLayout* layout)
{
    DWRITE_TEXT_METRICS m{};
    return (layout && SUCCEEDED(layout->GetMetrics(&m))) ? m.height : 0.0f;
}

bool IsWordChar(wchar_t ch)
{
    return IsCharAlphaNumericW(ch) || ch == L'_' || ch == L'\'' || ch == 0x2019 || (ch >= 0xD800 && ch <= 0xDFFF);
}

// A normal clipboard write of our own output (user pressed Copy / Ctrl+C).
bool SetClipboardText(HWND owner, std::wstring_view text)
{
    if (!OpenClipboard(owner)) {
        return false;
    }
    auto close = wil::scope_exit([] { CloseClipboard(); });
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    wil::unique_hglobal memory(GlobalAlloc(GMEM_MOVEABLE, bytes));
    if (!memory) {
        return false;
    }
    auto* target = static_cast<wchar_t*>(GlobalLock(memory.get()));
    if (!target) {
        return false;
    }
    std::memcpy(target, text.data(), text.size() * sizeof(wchar_t));
    target[text.size()] = L'\0';
    GlobalUnlock(memory.get());
    if (!SetClipboardData(CF_UNICODETEXT, memory.get())) {
        return false;
    }
    memory.release(); // owned by the clipboard
    return true;
}

} // namespace

// ---- Minimal UI Automation provider ------------------------------------------------------
// Exposes the popup as one element whose Name is the current text, so Narrator can read it.
class PopupUiaProvider final : public IRawElementProviderSimple {
public:
    explicit PopupUiaProvider(HWND hwnd) : m_hwnd(hwnd) {}

    void SetName(std::wstring name)
    {
        auto guard = m_lock.lock_exclusive();
        m_name = std::move(name);
    }

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** object) override
    {
        if (!object) {
            return E_POINTER;
        }
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IRawElementProviderSimple)) {
            *object = static_cast<IRawElementProviderSimple*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return ++m_refs; }
    IFACEMETHODIMP_(ULONG) Release() override
    {
        const ULONG refs = --m_refs;
        if (refs == 0) {
            delete this;
        }
        return refs;
    }

    // IRawElementProviderSimple
    IFACEMETHODIMP get_ProviderOptions(ProviderOptions* options) override
    {
        *options = static_cast<ProviderOptions>(ProviderOptions_ServerSideProvider | ProviderOptions_UseComThreading);
        return S_OK;
    }
    IFACEMETHODIMP GetPatternProvider(PATTERNID, IUnknown** provider) override
    {
        *provider = nullptr;
        return S_OK;
    }
    IFACEMETHODIMP GetPropertyValue(PROPERTYID id, VARIANT* value) override
    {
        VariantInit(value);
        switch (id) {
        case UIA_NamePropertyId: {
            auto guard = m_lock.lock_shared();
            value->bstrVal = SysAllocStringLen(m_name.data(), static_cast<UINT>(m_name.size()));
            if (value->bstrVal) {
                value->vt = VT_BSTR;
            }
            break;
        }
        case UIA_ControlTypePropertyId:
            value->vt = VT_I4;
            value->lVal = UIA_PaneControlTypeId;
            break;
        case UIA_LocalizedControlTypePropertyId:
            value->bstrVal = SysAllocString(L"translation");
            if (value->bstrVal) {
                value->vt = VT_BSTR;
            }
            break;
        case UIA_IsContentElementPropertyId:
        case UIA_IsControlElementPropertyId:
        case UIA_IsKeyboardFocusablePropertyId:
            value->vt = VT_BOOL;
            value->boolVal = VARIANT_TRUE;
            break;
        }
        return S_OK;
    }
    IFACEMETHODIMP get_HostRawElementProvider(IRawElementProviderSimple** provider) override
    {
        return UiaHostProviderFromHwnd(m_hwnd, provider);
    }

private:
    ~PopupUiaProvider() = default;

    std::atomic<ULONG> m_refs{1};
    HWND m_hwnd;
    wil::srwlock m_lock;
    std::wstring m_name;
};

// ---- PopupWindow -------------------------------------------------------------------------

PopupWindow::PopupWindow() = default;

PopupWindow::~PopupWindow()
{
    Destroy();
}

bool PopupWindow::Create(HINSTANCE instance, IPopupHost* host)
{
    m_instance = instance;
    m_host = host;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = &PopupWindow::WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kPopupClass;
    if (!RegisterClassExW(&wc)) {
        return false;
    }
    // WS_EX_NOREDIRECTIONBITMAP: all content comes from the DirectComposition visual.
    m_hwnd.reset(CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP, kPopupClass, AppInfo::kName,
                                 WS_POPUP, 0, 0, 420, 120, nullptr, nullptr, instance, this));
    if (!m_hwnd) {
        return false;
    }
    m_dpi = GetDpiForWindow(m_hwnd.get());

    if (FAILED(m_renderer.Initialize(m_hwnd.get()))) {
        return false;
    }
    wchar_t locale[LOCALE_NAME_MAX_LENGTH] = L"en-us";
    GetUserDefaultLocaleName(locale, ARRAYSIZE(locale));
    IDWriteFactory* dwrite = m_renderer.DWrite();
    // DirectWrite font fallback covers CJK, Arabic, emoji, ... automatically.
    if (FAILED(dwrite->CreateTextFormat(kFontFamily, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                        DWRITE_FONT_STRETCH_NORMAL, 14.0f, locale, m_bodyFormat.put())) ||
        FAILED(dwrite->CreateTextFormat(kFontFamily, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                        DWRITE_FONT_STRETCH_NORMAL, 12.0f, locale, m_smallFormat.put()))) {
        return false;
    }

    m_uia.attach(new (std::nothrow) PopupUiaProvider(m_hwnd.get()));
    if (!CreateEdit()) {
        return false;
    }

    const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
    DwmSetWindowAttribute(m_hwnd.get(), DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
    RefreshTheme();
    return true;
}

void PopupWindow::Destroy()
{
    if (m_hwnd) {
        if (m_uia) {
            UiaReturnRawElementProvider(m_hwnd.get(), 0, 0, nullptr);
            UiaDisconnectProvider(m_uia.get());
        }
        m_visible = false;
        KillTimer(m_hwnd.get(), kEditTimerId);
        KillTimer(m_hwnd.get(), kCopiedTimerId);
        m_edit.reset(); // before its owner
        m_hwnd.reset();
    }
    m_uia.reset();
}

// ---- Editable source box -----------------------------------------------------------------

bool PopupWindow::CreateEdit()
{
    // A top-level EDIT owned by the popup: it stays above it and never shows in the taskbar.
    m_edit.reset(CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"EDIT", L"",
                                 WS_POPUP | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN, 0, 0, 100, 40, m_hwnd.get(),
                                 nullptr, m_instance, nullptr));
    if (!m_edit) {
        return false;
    }
    SetWindowSubclass(m_edit.get(), &PopupWindow::EditSubclassProc, kEditSubclassId, reinterpret_cast<DWORD_PTR>(this));
    SendMessageW(m_edit.get(), EM_SETLIMITTEXT, 0, 0); // default limit is 30k; captures can be larger
    UpdateEditFont();
    return true;
}

void PopupWindow::UpdateEditFont()
{
    if (!m_edit || m_editFontDpi == m_dpi) {
        return;
    }
    LOGFONTW lf{};
    lf.lfHeight = -MulDiv(13, static_cast<int>(m_dpi), 96);
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, kFontFamily);
    wil::unique_hfont font(CreateFontIndirectW(&lf));
    if (!font) {
        return;
    }
    SendMessageW(m_edit.get(), WM_SETFONT, reinterpret_cast<WPARAM>(font.get()), TRUE);
    m_editFont = std::move(font); // the previous font is released only after the control switched
    m_editFontDpi = m_dpi;
}

void PopupWindow::PositionEdit()
{
    if (!m_edit || !m_hwnd) {
        return;
    }
    const auto px = [this](float dip) { return static_cast<int>(std::lround(dip * static_cast<float>(m_dpi) / 96.0f)); };
    POINT origin{0, 0};
    ClientToScreen(m_hwnd.get(), &origin);
    // Inset so the rounded background drawn by the popup frames the (square) control.
    SetWindowPos(m_edit.get(), nullptr, origin.x + px(m_inputRect.left + 6), origin.y + px(m_inputRect.top + 4),
                 px(m_inputRect.right - m_inputRect.left - 12), px(m_inputRect.bottom - m_inputRect.top - 8),
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

std::wstring PopupWindow::EditText() const
{
    const int length = GetWindowTextLengthW(m_edit.get());
    if (length <= 0) {
        return {};
    }
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(m_edit.get(), text.data(), length + 1);
    text.resize(static_cast<size_t>(std::max(copied, 0)));
    return text;
}

void PopupWindow::SetSourceText(const std::wstring& text)
{
    if (!m_edit) {
        return;
    }
    // The EDIT control wants CRLF line breaks; our text is normalized to LF.
    std::wstring crlf;
    crlf.reserve(text.size() + 16);
    for (const wchar_t ch : text) {
        if (ch == L'\n') {
            crlf.push_back(L'\r');
        }
        crlf.push_back(ch);
    }
    KillTimer(m_hwnd.get(), kEditTimerId);
    SetWindowTextW(m_edit.get(), crlf.c_str());
    SendMessageW(m_edit.get(), EM_SETMODIFY, FALSE, 0); // programmatic change: not a user edit
}

void PopupWindow::SetPinned(bool pinned, bool hasPosition, POINT position)
{
    m_pinned = pinned;
    m_hasPinPosition = pinned && hasPosition;
    m_pinPosition = position;
}

void PopupWindow::OnEditChanged()
{
    // Debounce: (re)start a one-shot timer; the translation runs after a ~300 ms pause.
    if (m_visible) {
        SetTimer(m_hwnd.get(), kEditTimerId, kEditDelayMs, nullptr);
    }
}

void PopupWindow::OnEditDeactivated(HWND activated)
{
    // Focus left the input box for something that is not the popup itself.
    if (activated != m_hwnd.get() && m_visible && !m_pinned && !m_inMenu) {
        Hide();
    }
}

LRESULT CALLBACK PopupWindow::EditSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id,
                                               DWORD_PTR refData)
{
    auto* self = reinterpret_cast<PopupWindow*>(refData);
    switch (msg) {
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            self->Hide();
            return 0;
        }
        if (wParam == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            SendMessageW(hwnd, EM_SETSEL, 0, -1);
            return 0;
        }
        break;
    case WM_CHAR:
        if (wParam == VK_ESCAPE || wParam == 1 /* Ctrl+A */) {
            return 0; // already handled on key-down; avoid the beep
        }
        break;
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE) {
            self->OnEditDeactivated(reinterpret_cast<HWND>(lParam));
        }
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, &PopupWindow::EditSubclassProc, id);
        break;
    }

    const LRESULT result = DefSubclassProc(hwnd, msg, wParam, lParam);

    switch (msg) {
    case WM_CHAR:
    case WM_KEYDOWN:
    case WM_PASTE:
    case WM_CUT:
    case WM_CLEAR:
    case WM_UNDO:
    case WM_IME_COMPOSITION:
    case WM_IME_CHAR:
        // The control's modify flag is set by user edits only (typing, paste, cut, undo, IME).
        if (SendMessageW(hwnd, EM_GETMODIFY, 0, 0)) {
            SendMessageW(hwnd, EM_SETMODIFY, FALSE, 0);
            self->OnEditChanged();
        }
        break;
    }
    return result;
}

void PopupWindow::RefreshTheme()
{
    m_theme = Theme::QueryState();
    if (!m_backdropAllowed) {
        m_theme.transparency = false; // user opted for the solid background
    }
    m_palette = Theme::Palette(m_theme);
    if (!m_hwnd) {
        return;
    }
    const BOOL dark = m_theme.dark ? TRUE : FALSE;
    DwmSetWindowAttribute(m_hwnd.get(), DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

    // Acrylic is the backdrop meant for transient flyouts (Mica is for long-lived windows).
    // With "Transparency effects" off, a contrast theme on, or if DWM refuses the attribute,
    // fall back to an opaque theme-colored background.
    const bool wantBackdrop = m_theme.WantsBackdrop();
    const DWM_SYSTEMBACKDROP_TYPE backdrop = wantBackdrop ? DWMSBT_TRANSIENTWINDOW : DWMSBT_NONE;
    const HRESULT hr = DwmSetWindowAttribute(m_hwnd.get(), DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
    const bool useBackdrop = wantBackdrop && SUCCEEDED(hr);
    const MARGINS margins = useBackdrop ? MARGINS{-1, -1, -1, -1} : MARGINS{0, 0, 0, 0};
    DwmExtendFrameIntoClientArea(m_hwnd.get(), &margins);
    m_solid = !useBackdrop;

    m_editBrush.reset(CreateSolidBrush(Theme::ToColorRef(m_palette.inputBackground)));
    if (m_edit) {
        InvalidateRect(m_edit.get(), nullptr, TRUE);
    }
}

void PopupWindow::OnThemeChanged()
{
    RefreshTheme();
    if (m_visible) {
        Render();
    }
}

void PopupWindow::SetTargets(std::vector<PopupLanguage> targets)
{
    m_targets = std::move(targets);
}

void PopupWindow::SetHeader(std::wstring source, std::string targetCode, std::wstring targetName, std::wstring provider)
{
    m_sourceLabel = std::move(source);
    m_targetCode = std::move(targetCode);
    m_targetLabel = std::move(targetName);
    m_providerLabel = std::move(provider);
    if (m_visible) {
        Relayout(false);
    }
}

int PopupWindow::ToPx(float dip) const
{
    return static_cast<int>(std::ceil(dip * static_cast<float>(m_dpi) / 96.0f));
}

wil::com_ptr_nothrow<IDWriteTextLayout> PopupWindow::MakeLayout(std::wstring_view text, IDWriteTextFormat* format,
                                                                float maxWidth, bool wrap) const
{
    wil::com_ptr_nothrow<IDWriteTextLayout> layout;
    if (FAILED(m_renderer.DWrite()->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format,
                                                     wrap ? maxWidth : 4096.0f, 4096.0f, layout.put()))) {
        return nullptr;
    }
    layout->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    return layout;
}

void PopupWindow::RebuildLayouts()
{
    const float textWidth = kWidth - 2 * kPadX;

    // Header: "<source> [swap] <target> v"            <provider>
    m_sourceLayout = MakeLayout(m_sourceLabel, m_smallFormat.get(), 0, false);
    m_swapLayout = MakeLayout(L"⇄", m_smallFormat.get(), 0, false);
    m_targetLayout = MakeLayout(m_targetLabel + L" ▾", m_smallFormat.get(), 0, false);
    m_providerLayout = MakeLayout(m_providerLabel, m_smallFormat.get(), 0, false);

    // Body.
    const std::wstring_view body = (m_state == State::Loading) ? std::wstring_view(L"Translating…") : std::wstring_view(m_text);
    m_bodyLayout = MakeLayout(body, m_bodyFormat.get(), textWidth, true);
    if (m_bodyLayout && m_state == State::Result && m_rtl) {
        // Leading alignment in a right-to-left layout is the right edge.
        m_bodyLayout->SetReadingDirection(DWRITE_READING_DIRECTION_RIGHT_TO_LEFT);
        m_bodyLayout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }
    m_hintLayout = (m_state == State::Error && !m_hint.empty()) ? MakeLayout(m_hint, m_smallFormat.get(), textWidth, true)
                                                                : nullptr;

    // Footer.
    m_noteLayout = m_note.empty() ? nullptr : MakeLayout(m_note, m_smallFormat.get(), 0, false);
    m_copyLayout = MakeLayout(L"Copy", m_smallFormat.get(), 0, false);
    m_copiedLayout = MakeLayout(L"Copied", m_smallFormat.get(), 0, false);
    m_pinLayout = MakeLayout(m_pinned ? L"Pinned" : L"Pin", m_smallFormat.get(), 0, false);

    // Height grows with content up to 60% of the monitor work area; beyond that the body scrolls.
    m_contentHeight = LayoutHeight(m_bodyLayout.get()) + (m_hintLayout ? kHintGap + LayoutHeight(m_hintLayout.get()) : 0.0f);
    const float workHeight = ToDip(m_work.bottom - m_work.top);
    const float bodyTop = kHeaderH + kInputTopPad + kInputH + kBodyTopPad;
    const float chrome = bodyTop + kBodyBottomPad + kFooterH;
    const float maxBody = std::max(kMinBodyH * 3, workHeight * kMaxHeightFraction - chrome);
    const float visible = std::clamp(m_contentHeight, kMinBodyH, maxBody);
    m_maxScroll = std::max(0.0f, m_contentHeight - visible);
    m_scroll = std::clamp(m_scroll, 0.0f, m_maxScroll);

    m_inputRect = D2D1::RectF(kPadX - 6, kHeaderH + kInputTopPad, kWidth - kPadX + 6, kHeaderH + kInputTopPad + kInputH);
    m_bodyRect = D2D1::RectF(kPadX, bodyTop, kWidth - kPadX, bodyTop + visible);
    m_heightDip = m_bodyRect.bottom + kBodyBottomPad + kFooterH;

    // Header hit areas: [swap] and [target v] follow the source label.
    float x = kPadX + LayoutWidth(m_sourceLayout.get()) + 4;
    m_swapRect = D2D1::RectF(x, 7, x + LayoutWidth(m_swapLayout.get()) + 12, kHeaderH - 7);
    x = m_swapRect.right + 2;
    m_targetRect = D2D1::RectF(x, 7, x + LayoutWidth(m_targetLayout.get()) + 12, kHeaderH - 7);

    const float buttonTop = m_heightDip - kFooterH + (kFooterH - kButtonH) / 2 - 2;
    float right = kWidth - kPadX + 6;
    const float pinWidth = LayoutWidth(m_pinLayout.get()) + 2 * kButtonPadX;
    m_pinRect = D2D1::RectF(right - pinWidth, buttonTop, right, buttonTop + kButtonH);
    right -= pinWidth + kButtonGap;
    // Sized for the wider label so the "Copied" feedback does not shift the layout.
    const float copyWidth =
        std::max(LayoutWidth(m_copyLayout.get()), LayoutWidth(m_copiedLayout.get())) + 2 * kButtonPadX;
    m_copyRect = D2D1::RectF(right - copyWidth, buttonTop, right, buttonTop + kButtonH);
}

void PopupWindow::UpdateWindowBounds(bool placeAtAnchor)
{
    const int width = ToPx(kWidth);
    const int height = ToPx(m_heightDip);

    RECT current{};
    GetWindowRect(m_hwnd.get(), &current);
    int x = current.left;
    int y = current.top;
    if (placeAtAnchor) {
        // Below the anchor; flip above if it does not fit.
        const int gap = ToPx(kAnchorGap);
        x = m_anchor.left;
        y = m_anchor.bottom + gap;
        if (y + height > m_work.bottom) {
            const int above = m_anchor.top - gap - height;
            y = (above >= m_work.top) ? above : m_work.bottom - height;
        }
    }
    // Clamp to the work area of the anchor's monitor.
    x = std::clamp(x, static_cast<int>(m_work.left), std::max(static_cast<int>(m_work.left), static_cast<int>(m_work.right) - width));
    y = std::clamp(y, static_cast<int>(m_work.top), std::max(static_cast<int>(m_work.top), static_cast<int>(m_work.bottom) - height));

    m_placing = true;
    SetWindowPos(m_hwnd.get(), HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
    m_placing = false;
    PositionEdit();
}

void PopupWindow::Relayout(bool placeAtAnchor)
{
    if (!m_hwnd) {
        return;
    }
    if (!placeAtAnchor) {
        // The user (or pin) decides the position: stay on whatever monitor the window is on.
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(MonitorFromWindow(m_hwnd.get(), MONITOR_DEFAULTTONEAREST), &mi)) {
            m_work = mi.rcWork;
        }
    }
    RebuildLayouts();
    UpdateWindowBounds(placeAtAnchor);
    Render();
}

void PopupWindow::ShowLoading(const RECT& anchor)
{
    if (!m_hwnd) {
        return;
    }
    RefreshTheme();
    m_state = State::Loading;
    m_text.clear();
    m_hint.clear();
    m_note.clear();
    m_rtl = false;
    m_scroll = 0;
    ClearSelection();

    m_copied = false;
    SetSourceText(L"");

    // A pinned popup stays where it is (visible) or returns to its remembered position (hidden).
    const bool keepPosition = m_visible && m_pinned;
    const bool atPinPosition = !keepPosition && m_pinned && m_hasPinPosition;
    if (!keepPosition) {
        m_anchor = anchor;
        m_userMoved = atPinPosition;
        const POINT reference = atPinPosition ? m_pinPosition : POINT{anchor.left, anchor.bottom};
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(MonitorFromPoint(reference, MONITOR_DEFAULTTONEAREST), &mi);
        m_work = mi.rcWork;
        // Move onto the target monitor first, then read the DPI the window ended up with.
        const POINT first = atPinPosition ? m_pinPosition : POINT{m_work.left, m_work.top};
        m_placing = true;
        SetWindowPos(m_hwnd.get(), nullptr, first.x, first.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        m_placing = false;
        m_dpi = GetDpiForWindow(m_hwnd.get());
    }
    UpdateEditFont();
    UpdateAccessibleName(false);
    Relayout(!keepPosition && !atPinPosition);
    if (!m_visible) {
        m_visible = true;
        ShowWindow(m_hwnd.get(), SW_SHOWNOACTIVATE);
    }
    ShowWindow(m_edit.get(), SW_SHOWNOACTIVATE);
}

void PopupWindow::Activate()
{
    if (!m_hwnd || !m_visible || IsForeground()) {
        return;
    }
    SetForegroundWindow(m_hwnd.get());
    if (IsForeground()) {
        return;
    }
    // The foreground lock can refuse us once our injected Ctrl+C made the target app the last
    // input receiver. Attaching to the foreground thread's input queue lifts that, but never
    // attach to a hung window: sharing its queue would hang us too.
    const HWND foreground = GetForegroundWindow();
    const DWORD self = GetCurrentThreadId();
    const DWORD other = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    if (other != 0 && other != self && !IsHungAppWindow(foreground) && AttachThreadInput(self, other, TRUE)) {
        SetForegroundWindow(m_hwnd.get());
        AttachThreadInput(self, other, FALSE);
    }
}

void PopupWindow::SetLoading()
{
    m_state = State::Loading;
    m_text.clear();
    m_hint.clear();
    m_note.clear();
    m_scroll = 0;
    ClearSelection();
    UpdateAccessibleName(false);
    if (m_visible) {
        Relayout(!m_userMoved && !m_pinned);
    }
}

void PopupWindow::SetResult(std::wstring text, bool rtl, std::wstring note)
{
    m_state = State::Result;
    m_text = std::move(text);
    m_hint.clear();
    m_note = std::move(note);
    m_rtl = rtl;
    m_scroll = 0;
    ClearSelection();
    UpdateAccessibleName(true);
    if (m_visible) {
        Relayout(!m_userMoved && !m_pinned);
    }
}

void PopupWindow::SetError(std::wstring message, std::wstring hint)
{
    m_state = State::Error;
    m_text = std::move(message);
    m_hint = std::move(hint);
    m_note.clear();
    m_rtl = false;
    m_scroll = 0;
    ClearSelection();
    UpdateAccessibleName(true);
    if (m_visible) {
        Relayout(!m_userMoved && !m_pinned);
    }
}

void PopupWindow::Hide()
{
    if (!m_visible) {
        return;
    }
    m_visible = false; // set first: SW_HIDE re-enters through WM_ACTIVATE
    m_selecting = false;
    m_hover = Hit::None;
    if (GetCapture() == m_hwnd.get()) {
        ReleaseCapture();
    }
    m_copied = false;
    KillTimer(m_hwnd.get(), kEditTimerId); // no timers while hidden
    KillTimer(m_hwnd.get(), kCopiedTimerId);
    ShowWindow(m_edit.get(), SW_HIDE);
    ShowWindow(m_hwnd.get(), SW_HIDE); // D3D/D2D resources stay alive for an instant re-show
    if (m_host) {
        m_host->OnPopupHidden();
    }
}

void PopupWindow::UpdateAccessibleName(bool announce)
{
    if (!m_uia) {
        return;
    }
    std::wstring name;
    switch (m_state) {
    case State::Loading:
        name = L"Translating…";
        break;
    case State::Result:
        name = m_text;
        break;
    case State::Error:
        name = m_hint.empty() ? m_text : m_text + L" " + m_hint;
        break;
    }
    m_uia->SetName(name);
    if (announce && !name.empty() && UiaClientsAreListening()) {
        // Lets Narrator read the result without moving focus.
        wil::unique_bstr text(SysAllocStringLen(name.data(), static_cast<UINT>(name.size())));
        wil::unique_bstr activity(SysAllocString(L"LightTranslate.Result"));
        if (text && activity) {
            UiaRaiseNotificationEvent(m_uia.get(), NotificationKind_ActionCompleted, NotificationProcessing_ImportantMostRecent,
                                      text.get(), activity.get());
        }
    }
}

// ---- Drawing -----------------------------------------------------------------------------

void PopupWindow::Invalidate()
{
    if (m_hwnd && m_visible) {
        InvalidateRect(m_hwnd.get(), nullptr, FALSE);
    }
}

void PopupWindow::Render()
{
    if (!m_hwnd) {
        return;
    }
    RECT client{};
    GetClientRect(m_hwnd.get(), &client);
    if (client.right <= 0 || client.bottom <= 0) {
        return;
    }
    // Second attempt only after a device loss (EndDraw released everything).
    for (int attempt = 0; attempt < 2; ++attempt) {
        ID2D1DeviceContext* dc = m_renderer.BeginDraw(static_cast<UINT>(client.right), static_cast<UINT>(client.bottom),
                                                      static_cast<float>(m_dpi));
        if (!dc) {
            return;
        }
        Draw(dc);
        if (m_renderer.EndDraw()) {
            break;
        }
    }
}

void PopupWindow::Draw(ID2D1DeviceContext* dc)
{
    const ThemePalette& p = m_palette;
    dc->Clear(m_solid ? p.background : D2D1::ColorF(0, 0, 0, 0));

    const auto drawText = [&](IDWriteTextLayout* layout, float x, float y, const D2D1_COLOR_F& color) {
        if (layout) {
            dc->DrawTextLayout(D2D1::Point2F(x, y), layout, m_renderer.Brush(color), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
        }
    };
    const auto fillRounded = [&](const D2D1_RECT_F& rect, const D2D1_COLOR_F& color) {
        dc->FillRoundedRectangle(D2D1::RoundedRect(rect, 4.0f, 4.0f), m_renderer.Brush(color));
    };
    const auto centeredY = [](IDWriteTextLayout* layout, float top, float height) {
        return top + (height - LayoutHeight(layout)) / 2;
    };

    // Header.
    drawText(m_sourceLayout.get(), kPadX, centeredY(m_sourceLayout.get(), 0, kHeaderH), p.secondaryText);
    if (m_hover == Hit::Swap) {
        fillRounded(m_swapRect, p.hover);
    }
    drawText(m_swapLayout.get(), m_swapRect.left + 6, centeredY(m_swapLayout.get(), 0, kHeaderH), p.secondaryText);
    if (m_hover == Hit::Target) {
        fillRounded(m_targetRect, p.hover);
    }
    drawText(m_targetLayout.get(), m_targetRect.left + 6, centeredY(m_targetLayout.get(), 0, kHeaderH), p.text);
    const float providerWidth = LayoutWidth(m_providerLayout.get());
    drawText(m_providerLayout.get(), kWidth - kPadX - providerWidth, centeredY(m_providerLayout.get(), 0, kHeaderH),
             p.secondaryText);
    dc->FillRectangle(D2D1::RectF(0, kHeaderH - 1, kWidth, kHeaderH), m_renderer.Brush(p.separator));

    // Background plate of the source box (the EDIT control itself is a separate window on top).
    fillRounded(m_inputRect, p.inputBackground);

    // Body (clipped; scrolls once the text exceeds the maximum height).
    dc->PushAxisAlignedClip(D2D1::RectF(0, m_bodyRect.top, kWidth, m_bodyRect.bottom), D2D1_ANTIALIAS_MODE_ALIASED);
    const float originX = m_bodyRect.left;
    const float originY = m_bodyRect.top - m_scroll;
    if (m_bodyLayout) {
        if (m_state == State::Result && HasSelection()) {
            const UINT32 start = std::min(m_selAnchor, m_selCaret);
            const UINT32 length = std::max(m_selAnchor, m_selCaret) - start;
            UINT32 count = 0;
            m_bodyLayout->HitTestTextRange(start, length, originX, originY, nullptr, 0, &count);
            if (count > 0) {
                std::vector<DWRITE_HIT_TEST_METRICS> metrics(count);
                if (SUCCEEDED(m_bodyLayout->HitTestTextRange(start, length, originX, originY, metrics.data(), count, &count))) {
                    ID2D1SolidColorBrush* brush = m_renderer.Brush(p.selection);
                    for (UINT32 i = 0; i < count; ++i) {
                        const DWRITE_HIT_TEST_METRICS& m = metrics[i];
                        dc->FillRectangle(D2D1::RectF(m.left, m.top, m.left + m.width, m.top + m.height), brush);
                    }
                }
            }
        }
        const D2D1_COLOR_F& bodyColor =
            (m_state == State::Loading) ? p.secondaryText : (m_state == State::Error) ? p.error : p.text;
        drawText(m_bodyLayout.get(), originX, originY, bodyColor);
        if (m_hintLayout) {
            drawText(m_hintLayout.get(), originX, originY + LayoutHeight(m_bodyLayout.get()) + kHintGap, p.secondaryText);
        }
    }
    dc->PopAxisAlignedClip();

    if (m_maxScroll > 0) {
        // Scroll position indicator.
        const float track = m_bodyRect.bottom - m_bodyRect.top;
        const float thumb = std::max(18.0f, track * track / m_contentHeight);
        const float top = m_bodyRect.top + (track - thumb) * (m_scroll / m_maxScroll);
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(kWidth - 7, top, kWidth - 4, top + thumb), 1.5f, 1.5f),
                                 m_renderer.Brush(p.secondaryText));
    }

    // Footer.
    const float footerTop = m_heightDip - kFooterH;
    if (m_noteLayout) {
        drawText(m_noteLayout.get(), kPadX, centeredY(m_noteLayout.get(), footerTop, kFooterH) - 2, p.secondaryText);
    }
    const auto drawButton = [&](const D2D1_RECT_F& rect, IDWriteTextLayout* label, bool hovered, const D2D1_COLOR_F& color) {
        if (hovered) {
            fillRounded(rect, p.hover);
        }
        drawText(label, rect.left + kButtonPadX, centeredY(label, rect.top, rect.bottom - rect.top), color);
    };
    drawButton(m_copyRect, m_copied ? m_copiedLayout.get() : m_copyLayout.get(), m_hover == Hit::Copy,
               m_copied ? p.accent : p.text);
    drawButton(m_pinRect, m_pinLayout.get(), m_hover == Hit::Pin, m_pinned ? p.accent : p.text);
}

// ---- Input -------------------------------------------------------------------------------

PopupWindow::Hit PopupWindow::HitTest(float x, float y) const
{
    if (Contains(m_swapRect, x, y)) {
        return Hit::Swap;
    }
    if (Contains(m_targetRect, x, y)) {
        return Hit::Target;
    }
    if (Contains(m_copyRect, x, y)) {
        return Hit::Copy;
    }
    if (Contains(m_pinRect, x, y)) {
        return Hit::Pin;
    }
    if (m_state == State::Result && !m_text.empty() && y >= m_bodyRect.top && y < m_bodyRect.bottom) {
        return Hit::Body;
    }
    return Hit::Drag;
}

UINT32 PopupWindow::TextPosFromPoint(float x, float y) const
{
    if (!m_bodyLayout) {
        return 0;
    }
    BOOL trailing = FALSE;
    BOOL inside = FALSE;
    DWRITE_HIT_TEST_METRICS metrics{};
    if (FAILED(m_bodyLayout->HitTestPoint(x - m_bodyRect.left, y - m_bodyRect.top + m_scroll, &trailing, &inside, &metrics))) {
        return 0;
    }
    const UINT32 pos = metrics.textPosition + (trailing ? metrics.length : 0);
    return std::min(pos, static_cast<UINT32>(m_text.size()));
}

void PopupWindow::SelectWordAt(UINT32 pos)
{
    const UINT32 size = static_cast<UINT32>(m_text.size());
    if (size == 0) {
        return;
    }
    pos = std::min(pos, size - 1);
    UINT32 start = pos;
    UINT32 end = pos;
    if (IsWordChar(m_text[pos])) {
        while (start > 0 && IsWordChar(m_text[start - 1])) {
            --start;
        }
        while (end < size && IsWordChar(m_text[end])) {
            ++end;
        }
    } else {
        end = pos + 1;
    }
    m_selAnchor = start;
    m_selCaret = end;
}

void PopupWindow::ScrollBy(float deltaDip)
{
    const float scroll = std::clamp(m_scroll + deltaDip, 0.0f, m_maxScroll);
    if (scroll != m_scroll) {
        m_scroll = scroll;
        Invalidate();
    }
}

void PopupWindow::OnMouseMove(float x, float y)
{
    if (!m_trackingLeave) {
        TRACKMOUSEEVENT tme{};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = m_hwnd.get();
        m_trackingLeave = TrackMouseEvent(&tme) != FALSE;
    }
    if (m_selecting) {
        const UINT32 pos = TextPosFromPoint(x, y);
        if (pos != m_selCaret) {
            m_selCaret = pos;
            Invalidate();
        }
        return;
    }
    const Hit hit = HitTest(x, y);
    if (hit != m_hover) {
        m_hover = hit;
        Invalidate();
    }
}

void PopupWindow::OnLButtonDown(float x, float y, bool shift)
{
    switch (HitTest(x, y)) {
    case Hit::Swap:
        if (m_host) {
            m_host->OnPopupSwapLanguages();
        }
        break;
    case Hit::Target:
        ShowLanguageMenu();
        break;
    case Hit::Copy:
        CopyToClipboard();
        break;
    case Hit::Pin:
        TogglePin();
        break;
    case Hit::Body: {
        const UINT32 pos = TextPosFromPoint(x, y);
        if (!shift) {
            m_selAnchor = pos;
        }
        m_selCaret = pos;
        m_selecting = true;
        SetCapture(m_hwnd.get());
        Invalidate();
        break;
    }
    case Hit::Drag:
        // Let the system run a normal window move from any empty area.
        if (HasSelection()) {
            ClearSelection();
            Invalidate();
        }
        ReleaseCapture();
        SendMessageW(m_hwnd.get(), WM_NCLBUTTONDOWN, HTCAPTION, 0);
        break;
    case Hit::None:
        break;
    }
}

void PopupWindow::OnLButtonDblClk(float x, float y)
{
    if (HitTest(x, y) == Hit::Body) {
        SelectWordAt(TextPosFromPoint(x, y));
        Invalidate();
    } else {
        OnLButtonDown(x, y, false);
    }
}

void PopupWindow::OnKeyDown(WPARAM vk)
{
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    switch (vk) {
    case VK_ESCAPE:
        Hide();
        break;
    case 'C':
    case VK_INSERT:
        if (ctrl) {
            CopyToClipboard();
        }
        break;
    case 'A':
        if (ctrl && m_state == State::Result) {
            m_selAnchor = 0;
            m_selCaret = static_cast<UINT32>(m_text.size());
            Invalidate();
        }
        break;
    case VK_UP:
        ScrollBy(-20.0f);
        break;
    case VK_DOWN:
        ScrollBy(20.0f);
        break;
    case VK_PRIOR:
        ScrollBy(-(m_bodyRect.bottom - m_bodyRect.top));
        break;
    case VK_NEXT:
        ScrollBy(m_bodyRect.bottom - m_bodyRect.top);
        break;
    }
}

void PopupWindow::ShowLanguageMenu()
{
    if (m_targets.empty()) {
        return;
    }
    wil::unique_hmenu menu(CreatePopupMenu());
    if (!menu) {
        return;
    }
    for (size_t i = 0; i < m_targets.size(); ++i) {
        UINT flags = MF_STRING;
        if (m_targets[i].code == m_targetCode) {
            flags |= MF_CHECKED;
        }
        if (i > 0 && i % 16 == 0) {
            flags |= MF_MENUBARBREAK; // columns instead of one very tall menu
        }
        AppendMenuW(menu.get(), flags, kLanguageMenuBase + static_cast<UINT>(i), m_targets[i].name.c_str());
    }
    POINT pt{ToPx(m_targetRect.left), ToPx(m_targetRect.bottom)};
    ClientToScreen(m_hwnd.get(), &pt);

    m_inMenu = true;
    const UINT cmd = static_cast<UINT>(TrackPopupMenuEx(menu.get(), TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN,
                                                        pt.x, pt.y, m_hwnd.get(), nullptr));
    m_inMenu = false;

    if (cmd >= kLanguageMenuBase && cmd < kLanguageMenuBase + m_targets.size()) {
        if (m_host) {
            m_host->OnPopupTargetChosen(m_targets[cmd - kLanguageMenuBase].code);
        }
    } else if (!m_pinned && !IsForeground()) {
        Hide(); // the menu was dismissed by clicking another application
    }
}

void PopupWindow::TogglePin()
{
    m_pinned = !m_pinned;
    RECT bounds{};
    GetWindowRect(m_hwnd.get(), &bounds);
    m_hasPinPosition = m_pinned;
    m_pinPosition = POINT{bounds.left, bounds.top};
    Relayout(false);
    if (m_host) {
        m_host->OnPopupPinChanged(m_pinned, m_pinPosition); // remembered across restarts
    }
}

void PopupWindow::CopyToClipboard()
{
    if (m_state == State::Loading || m_text.empty()) {
        return;
    }
    // Ctrl+C / Copy: the selection, or the whole translation if nothing is selected.
    std::wstring_view text = m_text;
    if (m_state == State::Result && HasSelection()) {
        const UINT32 start = std::min(m_selAnchor, m_selCaret);
        text = text.substr(start, std::max(m_selAnchor, m_selCaret) - start);
    }
    if (SetClipboardText(m_hwnd.get(), text)) {
        m_copied = true; // brief "Copied" feedback, cleared by a one-shot timer
        SetTimer(m_hwnd.get(), kCopiedTimerId, kCopiedDelayMs, nullptr);
        Invalidate();
    }
}

// ---- Window procedure --------------------------------------------------------------------

LRESULT CALLBACK PopupWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    if (auto* self = reinterpret_cast<PopupWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA))) {
        return self->HandleMessage(hwnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT PopupWindow::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        if (m_hwnd) {
            Render();
        }
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;

    case WM_GETOBJECT:
        if (m_uia && static_cast<long>(lParam) == static_cast<long>(UiaRootObjectId)) {
            return UiaReturnRawElementProvider(hwnd, wParam, lParam, m_uia.get());
        }
        break;

    case WM_ACTIVATE:
        // Hide when focus goes elsewhere, unless pinned (or our own language menu / input box
        // is what took the focus).
        if (LOWORD(wParam) == WA_INACTIVE && m_visible && !m_pinned && !m_inMenu &&
            (!m_edit || reinterpret_cast<HWND>(lParam) != m_edit.get())) {
            Hide();
        }
        break; // DefWindowProc gives the window keyboard focus on activation (Esc / Ctrl+C need it)

    case WM_TIMER:
        // Both timers are one-shot: kill first, then act.
        KillTimer(hwnd, wParam);
        if (wParam == kEditTimerId) {
            if (m_visible && m_host) {
                m_host->OnPopupSourceEdited(EditText());
            }
        } else if (wParam == kCopiedTimerId) {
            m_copied = false;
            Invalidate();
        }
        return 0;

    case WM_CTLCOLOREDIT:
        if (m_edit && reinterpret_cast<HWND>(lParam) == m_edit.get() && m_editBrush) {
            const HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, Theme::ToColorRef(m_palette.text));
            SetBkColor(dc, Theme::ToColorRef(m_palette.inputBackground));
            return reinterpret_cast<LRESULT>(m_editBrush.get());
        }
        break;

    case WM_WINDOWPOSCHANGED:
        PositionEdit(); // follow the popup while it is dragged
        break;

    case WM_DPICHANGED:
        m_dpi = HIWORD(wParam);
        UpdateEditFont();
        if (!m_placing && m_hwnd) {
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            m_placing = true;
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            m_placing = false;
            Relayout(false);
        }
        return 0;

    case WM_EXITSIZEMOVE:
        m_userMoved = true;
        if (m_pinned) {
            // Pinned mode remembers where the user put the popup.
            RECT bounds{};
            GetWindowRect(hwnd, &bounds);
            m_hasPinPosition = true;
            m_pinPosition = POINT{bounds.left, bounds.top};
            if (m_host) {
                m_host->OnPopupPinChanged(true, m_pinPosition);
            }
        }
        return 0;

    case WM_MOUSEMOVE:
        OnMouseMove(ToDip(GET_X_LPARAM(lParam)), ToDip(GET_Y_LPARAM(lParam)));
        return 0;

    case WM_MOUSELEAVE:
        m_trackingLeave = false;
        if (m_hover != Hit::None) {
            m_hover = Hit::None;
            Invalidate();
        }
        return 0;

    case WM_LBUTTONDOWN:
        OnLButtonDown(ToDip(GET_X_LPARAM(lParam)), ToDip(GET_Y_LPARAM(lParam)), (wParam & MK_SHIFT) != 0);
        return 0;

    case WM_LBUTTONDBLCLK:
        OnLButtonDblClk(ToDip(GET_X_LPARAM(lParam)), ToDip(GET_Y_LPARAM(lParam)));
        return 0;

    case WM_LBUTTONUP:
        if (m_selecting) {
            m_selecting = false;
            ReleaseCapture();
        }
        return 0;

    case WM_CAPTURECHANGED:
        m_selecting = false;
        return 0;

    case WM_MOUSEWHEEL:
        ScrollBy(-static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA * 48.0f);
        return 0;

    case WM_KEYDOWN:
        OnKeyDown(wParam);
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            PCWSTR cursor = IDC_ARROW;
            if (m_hover == Hit::Body || m_selecting) {
                cursor = IDC_IBEAM;
            } else if (m_hover == Hit::Swap || m_hover == Hit::Target || m_hover == Hit::Copy || m_hover == Hit::Pin) {
                cursor = IDC_HAND;
            }
            SetCursor(LoadCursorW(nullptr, cursor));
            return TRUE;
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
