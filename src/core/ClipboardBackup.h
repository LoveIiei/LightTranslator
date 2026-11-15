#pragma once
#include <windows.h>
#include <vector>

// Snapshot / restore of the clipboard around a capture (capture thread only).
//
// Only HGLOBAL-backed formats from an allow-list are copied: CF_UNICODETEXT, CF_DIBV5/CF_DIB,
// CF_HDROP and the registered "HTML Format", "Rich Text Format" and "PNG" formats.
// Synthesized formats (CF_TEXT, CF_OEMTEXT, CF_BITMAP, ...) are recreated by Windows and
// GDI-handle formats cannot be byte-copied, so both are skipped.
// Limitation: exotic formats (OLE objects, delayed-render private formats) do not survive.
class ClipboardBackup {
public:
    static constexpr size_t kMaxBytes = 16 * 1024 * 1024; // above this, only text is kept

    // Opens the clipboard (short retries) and copies the allow-listed formats.
    bool Capture(HWND owner);

    // Puts the snapshot back, tagged so it does not create a duplicate Win+V history /
    // cloud clipboard entry. An originally empty clipboard is restored by EmptyClipboard().
    bool Restore(HWND owner);

    static bool OpenWithRetry(HWND owner);

private:
    struct Item {
        UINT format = 0;
        std::vector<BYTE> data;
    };
    std::vector<Item> m_items;
    bool m_valid = false;
};
