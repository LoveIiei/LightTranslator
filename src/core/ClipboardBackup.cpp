#include "core/ClipboardBackup.h"

#include <wil/resource.h>
#include <algorithm>
#include <cstring>

namespace {

void SetDwordFormat(PCWSTR formatName, DWORD value)
{
    const UINT format = RegisterClipboardFormatW(formatName);
    if (format == 0) {
        return;
    }
    wil::unique_hglobal memory(GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD)));
    if (!memory) {
        return;
    }
    if (void* p = GlobalLock(memory.get())) {
        std::memcpy(p, &value, sizeof(value));
        GlobalUnlock(memory.get());
        if (SetClipboardData(format, memory.get())) {
            memory.release(); // the clipboard owns it now
        }
    }
}

} // namespace

bool ClipboardBackup::OpenWithRetry(HWND owner)
{
    // The clipboard may be briefly held by another process (clipboard managers, RDP, ...).
    for (int attempt = 0; attempt < 8; ++attempt) {
        if (OpenClipboard(owner)) {
            return true;
        }
        Sleep(20);
    }
    return false;
}

bool ClipboardBackup::Capture(HWND owner)
{
    m_items.clear();
    m_valid = false;
    if (!OpenWithRetry(owner)) {
        return false;
    }
    auto close = wil::scope_exit([] { CloseClipboard(); });

    const UINT html = RegisterClipboardFormatW(L"HTML Format");
    const UINT rtf = RegisterClipboardFormatW(L"Rich Text Format");
    const UINT png = RegisterClipboardFormatW(L"PNG");

    size_t total = 0;
    bool overCap = false;
    bool haveDib = false;
    // Formats are enumerated in the order they were placed; synthesized ones come last.
    for (UINT format = EnumClipboardFormats(0); format != 0; format = EnumClipboardFormats(format)) {
        const bool isText = format == CF_UNICODETEXT;
        const bool isDib = format == CF_DIB || format == CF_DIBV5;
        const bool allowed = isText || isDib || format == CF_HDROP || (html && format == html) ||
                             (rtf && format == rtf) || (png && format == png);
        if (!allowed) {
            continue;
        }
        if (isDib) {
            if (haveDib) {
                continue; // the second DIB flavour is synthesized from the first
            }
            haveDib = true;
        }
        if (overCap && !isText) {
            continue;
        }
        const HANDLE handle = GetClipboardData(format);
        if (!handle) {
            continue;
        }
        const SIZE_T size = GlobalSize(handle);
        if (size == 0) {
            continue;
        }
        if (!isText && total + size > kMaxBytes) {
            overCap = true;
            continue;
        }
        const void* source = GlobalLock(handle);
        if (!source) {
            continue;
        }
        Item item;
        item.format = format;
        item.data.assign(static_cast<const BYTE*>(source), static_cast<const BYTE*>(source) + size);
        GlobalUnlock(handle);
        total += size;
        m_items.push_back(std::move(item));
    }
    if (overCap) {
        std::erase_if(m_items, [](const Item& item) { return item.format != CF_UNICODETEXT; });
    }
    m_valid = true;
    return true;
}

bool ClipboardBackup::Restore(HWND owner)
{
    if (!m_valid) {
        return false;
    }
    if (!OpenWithRetry(owner)) {
        return false;
    }
    auto close = wil::scope_exit([] { CloseClipboard(); });

    EmptyClipboard();
    if (m_items.empty()) {
        return true; // the clipboard was empty before the capture
    }
    for (const Item& item : m_items) {
        wil::unique_hglobal memory(GlobalAlloc(GMEM_MOVEABLE, item.data.size()));
        if (!memory) {
            continue;
        }
        void* target = GlobalLock(memory.get());
        if (!target) {
            continue;
        }
        std::memcpy(target, item.data.data(), item.data.size());
        GlobalUnlock(memory.get());
        if (SetClipboardData(item.format, memory.get())) {
            memory.release(); // the clipboard owns it now
        }
    }
    // The restored data was already seen by clipboard history once; don't add it again.
    SetDwordFormat(L"CanIncludeInClipboardHistory", 0);
    SetDwordFormat(L"CanUploadToCloudClipboard", 0);
    return true;
}
