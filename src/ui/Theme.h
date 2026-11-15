#pragma once
#include <windows.h>
#include <d2d1.h>

// System theme queries (light / dark / high contrast / transparency). Read on demand; nothing is polled.

struct ThemeState {
    bool dark = false;         // apps use dark mode
    bool highContrast = false; // a contrast theme is active
    bool transparency = true;  // "Transparency effects" is on
    bool operator==(const ThemeState&) const = default;

    // Acrylic backdrop is only used when the user allows transparency and no contrast theme is on.
    bool WantsBackdrop() const { return transparency && !highContrast; }
};

struct ThemePalette {
    D2D1_COLOR_F background;    // solid fallback when there is no system backdrop
    D2D1_COLOR_F text;
    D2D1_COLOR_F secondaryText;
    D2D1_COLOR_F accent;
    D2D1_COLOR_F selection;
    D2D1_COLOR_F separator;
    D2D1_COLOR_F hover;
    D2D1_COLOR_F error;
    D2D1_COLOR_F inputBackground;
};

namespace Theme {

// Taskbar / system surfaces ("SystemUsesLightTheme"). Drives the tray icon variant.
bool TaskbarIsLight();

ThemeState QueryState();
ThemePalette Palette(const ThemeState& state);
COLORREF ToColorRef(const D2D1_COLOR_F& color);

} // namespace Theme
