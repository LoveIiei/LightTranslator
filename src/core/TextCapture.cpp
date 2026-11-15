#include "core/TextCapture.h"

#include "core/ClipboardBackup.h"

#include <memory>

namespace {

constexpr wchar_t kClipboardWindowClass[] = L"LightTranslate.CaptureWindow";
constexpr DWORD kModifierWaitMs = 1000;
constexpr DWORD kClipboardWaitMs = 400;
constexpr int kUiaAncestorDepth = 4;

bool IsSpace(wchar_t ch)
{
    return ch == L' ' || ch == L'\t' || ch == L'\n' || ch == L'\r' || ch == L'\f' || ch == L'\v' || ch == 0x00A0 ||
           ch == 0x3000 || ch == 0xFEFF;
}

bool AnyModifierDown()
{
    for (const int vk : {VK_CONTROL, VK_MENU, VK_SHIFT, VK_LWIN, VK_RWIN}) {
        if (GetAsyncKeyState(vk) & 0x8000) {
            return true;
        }
    }
    return false;
}

// Never injects key-ups for held modifiers: an injected Win key-up can open Start, and a held
// Win would turn our Ctrl+C into Win+Ctrl+C (color-filter toggle). We just wait (bounded).
bool WaitForModifierRelease()
{
    const ULONGLONG deadline = GetTickCount64() + kModifierWaitMs;
    while (AnyModifierDown()) {
        if (GetTickCount64() >= deadline) {
            return false;
        }
        Sleep(10);
    }
    return true;
}

bool SendCtrlC()
{
    INPUT inputs[4] = {};
    const WORD keys[4] = {VK_CONTROL, 'C', 'C', VK_CONTROL};
    const DWORD flags[4] = {0, 0, KEYEVENTF_KEYUP, KEYEVENTF_KEYUP};
    for (int i = 0; i < 4; ++i) {
        inputs[i].type = INPUT_KEYBOARD;
        inputs[i].ki.wVk = keys[i];
        inputs[i].ki.wScan = static_cast<WORD>(MapVirtualKeyW(keys[i], MAPVK_VK_TO_VSC));
        inputs[i].ki.dwFlags = flags[i];
    }
    return SendInput(ARRAYSIZE(inputs), inputs, sizeof(INPUT)) == ARRAYSIZE(inputs);
}

enum class ClipboardRead { Text, Secret, NoText };

// Reads CF_UNICODETEXT after a copy. The source app may still be writing, so retry briefly.
ClipboardRead ReadClipboardText(HWND owner, std::wstring& text)
{
    const UINT excludeFormat = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (OpenClipboard(owner)) {
            auto close = wil::scope_exit([] { CloseClipboard(); });
            if (excludeFormat != 0 && IsClipboardFormatAvailable(excludeFormat)) {
                return ClipboardRead::Secret; // password managers mark secrets this way
            }
            if (const HANDLE handle = GetClipboardData(CF_UNICODETEXT)) {
                if (const auto* data = static_cast<const wchar_t*>(GlobalLock(handle))) {
                    const size_t capacity = GlobalSize(handle) / sizeof(wchar_t);
                    text.assign(data, wcsnlen(data, capacity));
                    GlobalUnlock(handle);
                    return ClipboardRead::Text;
                }
            }
        }
        Sleep(15);
    }
    return ClipboardRead::NoText;
}

} // namespace

TextCapture::~TextCapture()
{
    Stop();
}

bool TextCapture::Start(HWND appHwnd)
{
    if (m_thread.joinable()) {
        return true;
    }
    m_appHwnd = appHwnd;
    m_stop = false;
    if (!m_wake.try_create(wil::EventOptions::None, nullptr)) {
        return false;
    }
    m_thread = std::thread([this] { ThreadMain(); });
    return true;
}

void TextCapture::Stop()
{
    if (!m_thread.joinable()) {
        return;
    }
    m_stop = true;
    m_wake.SetEvent();
    m_thread.join();
}

void TextCapture::Post(const CaptureJob& job)
{
    {
        auto guard = m_lock.lock_exclusive();
        m_pending = job;
    }
    m_wake.SetEvent();
}

std::wstring TextCapture::Normalize(std::wstring text, size_t maxChars, bool& truncated)
{
    truncated = false;

    // Line endings: \r\n and lone \r -> \n. Also drop embedded NULs.
    std::wstring out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        const wchar_t ch = text[i];
        if (ch == L'\r') {
            out.push_back(L'\n');
            if (i + 1 < text.size() && text[i + 1] == L'\n') {
                ++i;
            }
        } else if (ch != L'\0') {
            out.push_back(ch);
        }
    }

    size_t begin = 0;
    size_t end = out.size();
    while (begin < end && IsSpace(out[begin])) {
        ++begin;
    }
    while (end > begin && IsSpace(out[end - 1])) {
        --end;
    }
    out = out.substr(begin, end - begin);

    if (out.size() > maxChars) {
        size_t cut = maxChars;
        if (cut > 0 && IS_HIGH_SURROGATE(out[cut - 1])) {
            --cut; // don't split a surrogate pair
        }
        out.resize(cut);
        while (!out.empty() && IsSpace(out.back())) {
            out.pop_back();
        }
        truncated = true;
    }
    return out;
}

LRESULT CALLBACK TextCapture::ClipboardWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_CLIPBOARDUPDATE) {
        if (auto* self = reinterpret_cast<TextCapture*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA))) {
            self->m_clipboardUpdated = true;
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void TextCapture::PumpMessages()
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        DispatchMessageW(&msg);
    }
}

void TextCapture::ThreadMain()
{
    const HRESULT coInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto uninit = wil::scope_exit([coInit] {
        if (SUCCEEDED(coInit)) {
            CoUninitialize();
        }
    });

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &TextCapture::ClipboardWndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kClipboardWindowClass;
    RegisterClassExW(&wc);
    wil::unique_hwnd window(
        CreateWindowExW(0, kClipboardWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr));
    m_clipboardHwnd = window.get();
    if (m_clipboardHwnd) {
        SetWindowLongPtrW(m_clipboardHwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    }

    const HANDLE wake = m_wake.get();
    while (!m_stop) {
        // Blocks until a job arrives (or a window message needs dispatching): 0% CPU while idle.
        const DWORD wait = MsgWaitForMultipleObjects(1, &wake, FALSE, INFINITE, QS_ALLINPUT);
        if (wait == WAIT_OBJECT_0) {
            if (m_stop) {
                break;
            }
            std::optional<CaptureJob> job;
            {
                auto guard = m_lock.lock_exclusive();
                job.swap(m_pending);
            }
            if (job) {
                auto result = std::make_unique<CaptureResult>(Capture(*job));
                if (PostMessageW(m_appHwnd, WM_APP_CAPTURE_DONE, job->requestId,
                                 reinterpret_cast<LPARAM>(result.get()))) {
                    result.release(); // the UI thread owns it now
                }
            }
        } else if (wait == WAIT_OBJECT_0 + 1) {
            PumpMessages();
        } else {
            break; // WAIT_FAILED
        }
    }

    m_walker.reset();
    m_uia.reset();
    m_clipboardHwnd = nullptr;
}

CaptureResult TextCapture::Capture(const CaptureJob& job)
{
    CaptureResult result;

    std::wstring text;
    const UiaOutcome uia = CaptureViaUia(job.maxChars, text);
    if (uia == UiaOutcome::Password) {
        result.status = CaptureStatus::PasswordField;
        return result;
    }
    if (uia == UiaOutcome::Text) {
        result.text = Normalize(std::move(text), job.maxChars, result.truncated);
    }

    if (result.text.empty()) {
        if (!job.allowClipboardFallback) {
            result.status = CaptureStatus::NoSelectionFallbackOff;
            return result;
        }
        result = CaptureViaClipboard();
        if (result.status != CaptureStatus::Ok) {
            return result;
        }
        result.text = Normalize(std::move(result.text), job.maxChars, result.truncated);
        if (result.text.empty()) {
            result.status = CaptureStatus::NoSelection;
            return result;
        }
    }
    result.status = CaptureStatus::Ok;
    return result;
}

bool TextCapture::EnsureUia()
{
    if (m_uia) {
        return true;
    }
    // CUIAutomation8 exposes IUIAutomation2+ (timeouts); fall back to the base class.
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_uia))) &&
        FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_uia)))) {
        return false;
    }
    // A hung target application must not be able to hang us.
    if (auto uia2 = m_uia.try_query<IUIAutomation2>()) {
        uia2->put_ConnectionTimeout(1000);
        uia2->put_TransactionTimeout(500);
    }
    m_uia->get_ControlViewWalker(&m_walker);
    return true;
}

TextCapture::UiaOutcome TextCapture::CaptureViaUia(size_t maxChars, std::wstring& text)
{
    if (!EnsureUia()) {
        return UiaOutcome::Empty;
    }
    wil::com_ptr_nothrow<IUIAutomationElement> element;
    if (FAILED(m_uia->GetFocusedElement(&element)) || !element) {
        return UiaOutcome::Empty;
    }
    BOOL isPassword = FALSE;
    if (SUCCEEDED(element->get_CurrentIsPassword(&isPassword)) && isPassword) {
        return UiaOutcome::Password;
    }

    // The focused element itself often supports TextPattern (Notepad, Word, Terminal); in
    // browsers it can be a child of the document that does, so look at a few ancestors too.
    const int limit = static_cast<int>(maxChars) + 1; // +1 so truncation can be detected
    for (int depth = 0; depth < kUiaAncestorDepth && element; ++depth) {
        wil::com_ptr_nothrow<IUIAutomationTextPattern> pattern;
        if (SUCCEEDED(element->GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(&pattern))) && pattern) {
            wil::com_ptr_nothrow<IUIAutomationTextRangeArray> ranges;
            int count = 0;
            if (SUCCEEDED(pattern->GetSelection(&ranges)) && ranges && SUCCEEDED(ranges->get_Length(&count))) {
                for (int i = 0; i < count; ++i) {
                    wil::com_ptr_nothrow<IUIAutomationTextRange> range;
                    wil::unique_bstr part;
                    if (SUCCEEDED(ranges->GetElement(i, &range)) && range && SUCCEEDED(range->GetText(limit, &part)) &&
                        part && SysStringLen(part.get()) > 0) {
                        if (!text.empty()) {
                            text.push_back(L'\n');
                        }
                        text.append(part.get(), SysStringLen(part.get()));
                    }
                }
            }
            break; // this element owns the text; an empty selection here is final for tier 1
        }
        if (!m_walker) {
            break;
        }
        wil::com_ptr_nothrow<IUIAutomationElement> parent;
        if (FAILED(m_walker->GetParentElement(element.get(), &parent))) {
            break;
        }
        element = std::move(parent);
    }
    return text.empty() ? UiaOutcome::Empty : UiaOutcome::Text;
}

bool TextCapture::WaitForClipboardChange(DWORD sequenceBefore, DWORD timeoutMs)
{
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        PumpMessages();
        if (m_clipboardUpdated || GetClipboardSequenceNumber() != sequenceBefore) {
            return true;
        }
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) {
            return false;
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, static_cast<DWORD>(deadline - now), QS_ALLINPUT);
    }
}

CaptureResult TextCapture::CaptureViaClipboard()
{
    CaptureResult result;
    result.viaClipboard = true;

    if (!m_clipboardHwnd) {
        result.status = CaptureStatus::ClipboardBusy;
        return result;
    }
    if (!WaitForModifierRelease()) {
        result.status = CaptureStatus::ModifiersHeld;
        return result;
    }

    ClipboardBackup backup;
    if (!backup.Capture(m_clipboardHwnd)) {
        result.status = CaptureStatus::ClipboardBusy;
        return result;
    }
    // Taken after the snapshot: reading delay-rendered data can itself bump the sequence number.
    const DWORD sequenceBefore = GetClipboardSequenceNumber();

    // Listen for this capture only; there is no clipboard monitoring at any other time.
    m_clipboardUpdated = false;
    AddClipboardFormatListener(m_clipboardHwnd);
    const bool sent = SendCtrlC();
    const bool changed = sent && WaitForClipboardChange(sequenceBefore, kClipboardWaitMs);
    RemoveClipboardFormatListener(m_clipboardHwnd);

    if (!changed) {
        // Nothing selected, or the target is elevated (UIPI silently drops our input).
        // The clipboard was never modified, so there is nothing to restore.
        result.status = CaptureStatus::NoSelection;
        return result;
    }

    std::wstring text;
    const ClipboardRead read = ReadClipboardText(m_clipboardHwnd, text);
    backup.Restore(m_clipboardHwnd);

    switch (read) {
    case ClipboardRead::Secret:
        result.status = CaptureStatus::SecretContent;
        break;
    case ClipboardRead::NoText:
        result.status = CaptureStatus::NoSelection;
        break;
    case ClipboardRead::Text:
        result.status = CaptureStatus::Ok;
        result.text = std::move(text);
        break;
    }
    return result;
}
