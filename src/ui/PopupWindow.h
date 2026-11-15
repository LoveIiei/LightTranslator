#pragma once
#include <windows.h>
#include <ole2.h>
#include <string>
#include <string_view>
#include <vector>
#include <wil/com.h>
#include <wil/resource.h>

#include "ui/PopupRenderer.h"
#include "ui/Theme.h"

struct PopupLanguage {
    std::string code;  // canonical language code
    std::wstring name; // display name
};

// Callbacks from the popup into the app (UI thread).
class IPopupHost {
public:
    virtual void OnPopupHidden() = 0;                                // cancel in-flight work
    virtual void OnPopupTargetChosen(const std::string& code) = 0;   // language menu selection
    virtual void OnPopupSourceEdited(const std::wstring& text) = 0;  // input box changed (debounced)
    virtual void OnPopupSwapLanguages() = 0;                         // swap button
    virtual void OnPopupPinChanged(bool pinned, POINT position) = 0; // pin toggled / pinned popup moved

protected:
    ~IPopupHost() = default;
};

class PopupUiaProvider;

// The translation flyout. Created once at startup and kept hidden; all D3D/D2D resources stay
// alive between shows. Draws only on WM_PAINT / state changes; no timers.
class PopupWindow {
public:
    enum class State { Loading, Result, Error };

    PopupWindow();
    ~PopupWindow();
    PopupWindow(const PopupWindow&) = delete;
    PopupWindow& operator=(const PopupWindow&) = delete;

    bool Create(HINSTANCE instance, IPopupHost* host);
    void Destroy();

    void SetTargets(std::vector<PopupLanguage> targets);
    void SetHeader(std::wstring source, std::string targetCode, std::wstring targetName, std::wstring provider);

    // Shows the popup near the anchor in the Loading state WITHOUT taking focus: the capture
    // still needs the foreground application to keep focus. Call Activate() once it is done.
    void ShowLoading(const RECT& anchor);
    void Activate();

    void SetLoading();
    void SetResult(std::wstring text, bool rtl, std::wstring note);
    void SetError(std::wstring message, std::wstring hint);
    void Hide();
    void OnThemeChanged();

    // Text shown in the editable source box (does not trigger OnPopupSourceEdited).
    void SetSourceText(const std::wstring& text);
    // Restores the pinned state (and remembered position) from settings.
    void SetPinned(bool pinned, bool hasPosition, POINT position);
    // false = always paint the solid theme background instead of the Acrylic backdrop.
    void SetBackdropAllowed(bool allowed)
    {
        m_backdropAllowed = allowed;
        RefreshTheme();
    }

    bool IsVisible() const { return m_visible; }
    bool IsForeground() const
    {
        const HWND foreground = GetForegroundWindow();
        return m_hwnd && foreground && (foreground == m_hwnd.get() || foreground == m_edit.get());
    }
    HWND Hwnd() const { return m_hwnd.get(); }

private:
    enum class Hit { None, Drag, Swap, Target, Body, Copy, Pin };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void RefreshTheme();
    void RebuildLayouts();
    void UpdateWindowBounds(bool placeAtAnchor);
    void Relayout(bool placeAtAnchor);
    void Render();
    void Draw(ID2D1DeviceContext* dc);
    void Invalidate();
    void UpdateAccessibleName(bool announce);

    wil::com_ptr_nothrow<IDWriteTextLayout> MakeLayout(std::wstring_view text, IDWriteTextFormat* format, float maxWidth,
                                                       bool wrap) const;

    float ToDip(int px) const { return px * 96.0f / static_cast<float>(m_dpi); }
    int ToPx(float dip) const;
    Hit HitTest(float x, float y) const;
    UINT32 TextPosFromPoint(float x, float y) const;
    bool HasSelection() const { return m_selAnchor != m_selCaret; }
    void ClearSelection() { m_selAnchor = m_selCaret = 0; m_selecting = false; }
    void SelectWordAt(UINT32 pos);
    void ScrollBy(float deltaDip);

    void OnMouseMove(float x, float y);
    void OnLButtonDown(float x, float y, bool shift);
    void OnLButtonDblClk(float x, float y);
    void OnKeyDown(WPARAM vk);
    void ShowLanguageMenu();
    void TogglePin();
    void CopyToClipboard();

    // Editable source box. WS_EX_NOREDIRECTIONBITMAP windows cannot host GDI child controls,
    // so the EDIT control lives in its own owned popup window placed over the input area.
    bool CreateEdit();
    void PositionEdit();
    void UpdateEditFont();
    void OnEditChanged();
    void OnEditDeactivated(HWND activated);
    std::wstring EditText() const;
    static LRESULT CALLBACK EditSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR id,
                                             DWORD_PTR refData);

    HINSTANCE m_instance = nullptr;
    IPopupHost* m_host = nullptr;
    wil::unique_hwnd m_hwnd;
    PopupRenderer m_renderer;
    wil::com_ptr_nothrow<PopupUiaProvider> m_uia;

    wil::unique_hwnd m_edit;
    wil::unique_hfont m_editFont;
    wil::unique_hbrush m_editBrush;
    UINT m_editFontDpi = 0;

    wil::com_ptr_nothrow<IDWriteTextLayout> m_swapLayout;
    wil::com_ptr_nothrow<IDWriteTextLayout> m_copiedLayout;
    D2D1_RECT_F m_inputRect{};
    D2D1_RECT_F m_swapRect{};
    bool m_copied = false; // "Copied" feedback, reset by a one-shot timer
    bool m_hasPinPosition = false;
    POINT m_pinPosition{};

    wil::com_ptr_nothrow<IDWriteTextFormat> m_bodyFormat;
    wil::com_ptr_nothrow<IDWriteTextFormat> m_smallFormat;
    wil::com_ptr_nothrow<IDWriteTextLayout> m_sourceLayout;
    wil::com_ptr_nothrow<IDWriteTextLayout> m_targetLayout;
    wil::com_ptr_nothrow<IDWriteTextLayout> m_providerLayout;
    wil::com_ptr_nothrow<IDWriteTextLayout> m_bodyLayout;
    wil::com_ptr_nothrow<IDWriteTextLayout> m_hintLayout;
    wil::com_ptr_nothrow<IDWriteTextLayout> m_noteLayout;
    wil::com_ptr_nothrow<IDWriteTextLayout> m_copyLayout;
    wil::com_ptr_nothrow<IDWriteTextLayout> m_pinLayout;

    ThemeState m_theme;
    ThemePalette m_palette{};
    bool m_solid = true; // no system backdrop: paint an opaque theme background
    bool m_backdropAllowed = true;

    State m_state = State::Loading;
    std::wstring m_text; // translation (Result) or error message (Error)
    std::wstring m_hint;
    std::wstring m_note;
    bool m_rtl = false;
    std::wstring m_sourceLabel;
    std::wstring m_targetLabel;
    std::wstring m_providerLabel;
    std::string m_targetCode;
    std::vector<PopupLanguage> m_targets;

    UINT m_dpi = 96;
    RECT m_anchor{};
    RECT m_work{};
    bool m_visible = false;
    bool m_pinned = false;
    bool m_userMoved = false;
    bool m_inMenu = false;
    bool m_placing = false;

    // Layout, in DIPs.
    float m_heightDip = 120.0f;
    float m_contentHeight = 0;
    float m_scroll = 0;
    float m_maxScroll = 0;
    D2D1_RECT_F m_bodyRect{};
    D2D1_RECT_F m_targetRect{};
    D2D1_RECT_F m_copyRect{};
    D2D1_RECT_F m_pinRect{};

    UINT32 m_selAnchor = 0;
    UINT32 m_selCaret = 0;
    bool m_selecting = false;
    Hit m_hover = Hit::None;
    bool m_trackingLeave = false;
};
