#include "ui/Theme.h"

namespace {

constexpr wchar_t kPersonalizeKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";

DWORD ReadPersonalizeDword(PCWSTR name, DWORD fallback)
{
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kPersonalizeKey, name, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS) {
        return value;
    }
    return fallback;
}

constexpr D2D1_COLOR_F Rgb(unsigned rgb, float alpha = 1.0f)
{
    return D2D1_COLOR_F{((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, alpha};
}

D2D1_COLOR_F Sys(int index, float alpha = 1.0f)
{
    const COLORREF c = GetSysColor(index);
    return D2D1_COLOR_F{GetRValue(c) / 255.0f, GetGValue(c) / 255.0f, GetBValue(c) / 255.0f, alpha};
}

} // namespace

namespace Theme {

bool TaskbarIsLight()
{
    return ReadPersonalizeDword(L"SystemUsesLightTheme", 1) != 0;
}

ThemeState QueryState()
{
    ThemeState state;
    state.dark = ReadPersonalizeDword(L"AppsUseLightTheme", 1) == 0;
    state.transparency = ReadPersonalizeDword(L"EnableTransparency", 1) != 0;

    HIGHCONTRASTW hc{};
    hc.cbSize = sizeof(hc);
    state.highContrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON);
    return state;
}

ThemePalette Palette(const ThemeState& state)
{
    ThemePalette p{};
    if (state.highContrast) {
        // Contrast themes: system colors only.
        p.background = Sys(COLOR_WINDOW);
        p.text = Sys(COLOR_WINDOWTEXT);
        p.secondaryText = Sys(COLOR_WINDOWTEXT);
        p.accent = Sys(COLOR_HOTLIGHT);
        p.selection = Sys(COLOR_HIGHLIGHT, 0.45f);
        p.separator = Sys(COLOR_WINDOWTEXT);
        p.hover = Sys(COLOR_HIGHLIGHT, 0.35f);
        p.error = Sys(COLOR_WINDOWTEXT);
        p.inputBackground = Sys(COLOR_WINDOW);
    } else if (state.dark) {
        p.background = Rgb(0x2C2C2C);
        p.text = Rgb(0xFFFFFF);
        p.secondaryText = Rgb(0xFFFFFF, 0.70f);
        p.accent = Rgb(0x60CDFF);
        p.selection = Rgb(0x2B88D8, 0.55f);
        p.separator = Rgb(0xFFFFFF, 0.10f);
        p.hover = Rgb(0xFFFFFF, 0.09f);
        p.error = Rgb(0xFF99A4);
        p.inputBackground = Rgb(0x383838);
    } else {
        p.background = Rgb(0xF9F9F9);
        p.text = Rgb(0x1A1A1A);
        p.secondaryText = Rgb(0x000000, 0.62f);
        p.accent = Rgb(0x005FB8);
        p.selection = Rgb(0x0078D4, 0.30f);
        p.separator = Rgb(0x000000, 0.10f);
        p.hover = Rgb(0x000000, 0.06f);
        p.error = Rgb(0xC42B1C);
        p.inputBackground = Rgb(0xFFFFFF);
    }
    return p;
}

COLORREF ToColorRef(const D2D1_COLOR_F& color)
{
    const auto channel = [](float v) { return static_cast<BYTE>(v <= 0 ? 0 : v >= 1 ? 255 : v * 255.0f + 0.5f); };
    return RGB(channel(color.r), channel(color.g), channel(color.b));
}

} // namespace Theme
