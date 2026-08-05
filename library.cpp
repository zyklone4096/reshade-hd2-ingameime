#include "library.h"
#include "input.h"

#include <windows.h>
#include <imm.h>

#include <reshade.hpp>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <cstdarg>
#include <cstdio>
#include <utility>
#include <vector>

#ifdef NAME
#undef NAME
#endif

#ifdef DESCRIPTION
#undef DESCRIPTION
#endif

extern "C" __declspec(dllexport) const char *NAME = "Helldivers 2 Ingame IME";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Chinese IME Support";

static HWND g_hWnd = nullptr;
static WNDPROC g_OriginalWndProc = nullptr;
static bool g_InputMode = false;
static bool g_ImeOpen = false;
static bool g_PendingInputModeOnEnterUp = false;
static bool g_PendingSubmitOnEnterUp = false;
static bool g_PendingPasteOnShortcutRelease = false;
static WPARAM g_PendingPasteKey = 0;
static WPARAM g_PendingPasteModifierKey = 0;
static bool g_PendingPasteKeyReleased = false;
static bool g_PendingPasteModifierReleased = false;
static bool g_RestoreImeAfterPaste = false;
static HKL g_GameplayKeyboardLayout = nullptr;
static HKL g_TextKeyboardLayout = nullptr;
static bool g_ChangingKeyboardLayout = false;
static bool g_WindowActive = false;
static bool g_GameplayLayoutActive = false;

struct TextSubmission {
    std::wstring text;
    bool sendEnter;
    bool useAltCode;
};

static std::wstring g_LastResultText;
static std::deque<TextSubmission> g_PendingSubmissions;
static std::mutex g_SubmissionMutex;
static std::condition_variable g_SubmissionCv;
static HANDLE g_SubmissionThread = nullptr;
static std::atomic<bool> g_StopSubmissionThread = false;
constexpr UINT WM_APP_SUBMIT_TEXT = WM_APP + 1;

#ifndef RESHADE_HD2_ENABLE_IME_HWND_DIAGNOSTICS
#define RESHADE_HD2_ENABLE_IME_HWND_DIAGNOSTICS 0
#endif

static constexpr bool kEnableImeHwndDiagnostics = RESHADE_HD2_ENABLE_IME_HWND_DIAGNOSTICS != 0;

static void AppendDiagnosticFormat(std::wstring &message, const wchar_t *format, ...) {
    wchar_t buffer[2048] = {};
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(buffer, _countof(buffer), _TRUNCATE, format, args);
    va_end(args);
    message += buffer;
}

static const wchar_t *MessageName(UINT msg) {
    switch (msg) {
        case WM_KEYDOWN: return L"WM_KEYDOWN";
        case WM_KEYUP: return L"WM_KEYUP";
        case WM_SYSKEYDOWN: return L"WM_SYSKEYDOWN";
        case WM_SYSKEYUP: return L"WM_SYSKEYUP";
        case WM_CHAR: return L"WM_CHAR";
        case WM_IME_CHAR: return L"WM_IME_CHAR";
        case WM_IME_SETCONTEXT: return L"WM_IME_SETCONTEXT";
        case WM_IME_STARTCOMPOSITION: return L"WM_IME_STARTCOMPOSITION";
        case WM_IME_COMPOSITION: return L"WM_IME_COMPOSITION";
        case WM_IME_ENDCOMPOSITION: return L"WM_IME_ENDCOMPOSITION";
        case WM_IME_NOTIFY: return L"WM_IME_NOTIFY";
        case WM_INPUTLANGCHANGE: return L"WM_INPUTLANGCHANGE";
        case WM_INPUTLANGCHANGEREQUEST: return L"WM_INPUTLANGCHANGEREQUEST";
        case WM_ACTIVATE: return L"WM_ACTIVATE";
        case WM_ACTIVATEAPP: return L"WM_ACTIVATEAPP";
        case WM_SETFOCUS: return L"WM_SETFOCUS";
        case WM_KILLFOCUS: return L"WM_KILLFOCUS";
        case WM_APP_SUBMIT_TEXT: return L"WM_APP_SUBMIT_TEXT";
        default: return L"message";
    }
}

static bool ShouldLogDiagnosticMessage(UINT msg) {
    switch (msg) {
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
        case WM_CHAR:
        case WM_IME_CHAR:
        case WM_IME_SETCONTEXT:
        case WM_IME_STARTCOMPOSITION:
        case WM_IME_COMPOSITION:
        case WM_IME_ENDCOMPOSITION:
        case WM_IME_NOTIFY:
        case WM_INPUTLANGCHANGE:
        case WM_INPUTLANGCHANGEREQUEST:
        case WM_ACTIVATE:
        case WM_ACTIVATEAPP:
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            return true;
        default:
            return false;
    }
}

static const wchar_t *FirstCodePointClass(const wchar_t *text, std::size_t length) {
    if (!text || length == 0)
        return L"empty";

    const wchar_t first = text[0];
    if (first < 0x20 || first == 0x7F)
        return L"ascii-control";
    if (first < 0x80)
        return L"ascii-printable";
    if (first >= 0x4E00 && first <= 0x9FFF)
        return L"cjk-unified";
    if (first >= 0xD800 && first <= 0xDFFF)
        return L"surrogate";
    return L"non-ascii";
}

static const wchar_t *BoolText(bool value) {
    return value ? L"1" : L"0";
}

static std::wstring WindowClassName(HWND hwnd) {
    if (!hwnd)
        return L"<null>";

    wchar_t className[128] = {};
    const int length = GetClassNameW(hwnd, className, _countof(className));
    if (length <= 0)
        return L"<unavailable>";

    return className;
}

static const wchar_t *CodePointClass(wchar_t value) {
    const wchar_t text[] = {value, L'\0'};
    return FirstCodePointClass(text, 1);
}

static const wchar_t *AnsiImeCharClass(WPARAM wParam, std::size_t &unitCount) {
    char bytes[2] = {};
    unitCount = 0;
    if ((wParam & 0xFF00) != 0)
        bytes[unitCount++] = static_cast<char>((wParam >> 8) & 0xFF);
    if ((wParam & 0x00FF) != 0)
        bytes[unitCount++] = static_cast<char>(wParam & 0xFF);

    if (unitCount == 0)
        return L"empty";

    wchar_t converted[2] = {};
    const int convertedLength = MultiByteToWideChar(936, 0, bytes, static_cast<int>(unitCount), converted,
                                                    _countof(converted));
    if (convertedLength <= 0)
        return L"conversion-failed";

    return FirstCodePointClass(converted, static_cast<std::size_t>(convertedLength));
}

static std::wstring GetDiagnosticFilePath() {
    wchar_t tempPath[MAX_PATH] = {};
    const DWORD length = GetTempPathW(_countof(tempPath), tempPath);
    if (length == 0 || length >= _countof(tempPath))
        return L"reshade_hd2_ingameime_diagnostics.log";

    std::wstring path(tempPath);
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/')
        path += L'\\';
    path += L"reshade_hd2_ingameime_diagnostics.log";
    return path;
}

static void AppendDiagnosticFileLine(const std::wstring &line) {
    const std::wstring path = GetDiagnosticFilePath();
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA | GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;

    DWORD written = 0;
    if (GetFileSize(file, nullptr) == 0) {
        const BYTE bom[] = {0xFF, 0xFE};
        WriteFile(file, bom, sizeof(bom), &written, nullptr);
    }

    WriteFile(file, line.data(), static_cast<DWORD>(line.size() * sizeof(wchar_t)), &written, nullptr);
    CloseHandle(file);
}

static void LogImeHwndDiagnostic(const std::wstring &message) {
    if (!kEnableImeHwndDiagnostics)
        return;

    const std::wstring line = L"[reshade-hd2-ime] " + message + L"\r\n";
    OutputDebugStringW(line.c_str());
    AppendDiagnosticFileLine(line);
}

static void LogSubmissionDiagnostic(const wchar_t *label, HWND hwnd, const TextSubmission &submission,
                                    std::size_t queueDepth = 0) {
    if (!kEnableImeHwndDiagnostics)
        return;

    std::wstring message;
    AppendDiagnosticFormat(message,
                           L"%s hwnd=%p text_empty=%s utf16_length=%zu first_codepoint_class=%s use_alt_code=%s send_enter=%s queue_depth=%zu foreground=%p hooked_hwnd=%p",
                           label,
                           static_cast<void *>(hwnd),
                           BoolText(submission.text.empty()),
                           submission.text.length(),
                           FirstCodePointClass(submission.text.c_str(), submission.text.length()),
                           BoolText(submission.useAltCode),
                           BoolText(submission.sendEnter),
                           queueDepth,
                           static_cast<void *>(GetForegroundWindow()),
                           static_cast<void *>(g_hWnd));
    LogImeHwndDiagnostic(message);
}

static void LogSingleHwndIdentity(const wchar_t *snapshotLabel, const wchar_t *role, HWND hwnd) {
    if (!kEnableImeHwndDiagnostics)
        return;

    const DWORD windowThread = hwnd ? GetWindowThreadProcessId(hwnd, nullptr) : 0;
    const BOOL unicodeWindow = hwnd ? IsWindowUnicode(hwnd) : FALSE;
    HWND defaultImeHwnd = hwnd ? ImmGetDefaultIMEWnd(hwnd) : nullptr;
    const DWORD defaultImeThread = defaultImeHwnd ? GetWindowThreadProcessId(defaultImeHwnd, nullptr) : 0;
    HIMC context = hwnd ? ImmGetContext(hwnd) : nullptr;
    const bool hasContext = context != nullptr;
    BOOL imeOpen = FALSE;
    BOOL conversionOk = FALSE;
    DWORD conversion = 0;
    DWORD sentence = 0;
    BOOL releaseOk = FALSE;

    if (context) {
        imeOpen = ImmGetOpenStatus(context);
        conversionOk = ImmGetConversionStatus(context, &conversion, &sentence);
        releaseOk = ImmReleaseContext(hwnd, context);
    }

    std::wstring message;
    AppendDiagnosticFormat(message,
                           L"%s hwnd_identity role=%s hwnd=%p class=%s thread_id=%lu is_unicode=%s default_ime_hwnd=%p default_ime_class=%s default_ime_thread_id=%lu himc=%p himc_present=%s ime_open=%s conv_ok=%s conversion=0x%08lx sentence=0x%08lx release_ok=%s",
                           snapshotLabel,
                           role,
                           static_cast<void *>(hwnd),
                           WindowClassName(hwnd).c_str(),
                           windowThread,
                           BoolText(unicodeWindow != FALSE),
                           static_cast<void *>(defaultImeHwnd),
                           WindowClassName(defaultImeHwnd).c_str(),
                           defaultImeThread,
                           static_cast<void *>(context),
                           BoolText(hasContext),
                           BoolText(imeOpen != FALSE),
                           BoolText(conversionOk != FALSE),
                           conversion,
                           sentence,
                           BoolText(releaseOk != FALSE));
    LogImeHwndDiagnostic(message);
}

static void LogGuiThreadInfo(const wchar_t *label, DWORD threadId) {
    if (!kEnableImeHwndDiagnostics || threadId == 0)
        return;

    GUITHREADINFO guiInfo = {};
    guiInfo.cbSize = sizeof(guiInfo);
    const BOOL ok = GetGUIThreadInfo(threadId, &guiInfo);

    std::wstring message;
    AppendDiagnosticFormat(message,
                           L"%s gui_thread_info thread_id=%lu ok=%s hwndActive=%p hwndFocus=%p hwndCapture=%p hwndMenuOwner=%p hwndMoveSize=%p hwndCaret=%p caret_left=%ld caret_top=%ld caret_right=%ld caret_bottom=%ld flags=0x%08lx",
                           label,
                           threadId,
                           BoolText(ok != FALSE),
                           static_cast<void *>(ok ? guiInfo.hwndActive : nullptr),
                           static_cast<void *>(ok ? guiInfo.hwndFocus : nullptr),
                           static_cast<void *>(ok ? guiInfo.hwndCapture : nullptr),
                           static_cast<void *>(ok ? guiInfo.hwndMenuOwner : nullptr),
                           static_cast<void *>(ok ? guiInfo.hwndMoveSize : nullptr),
                           static_cast<void *>(ok ? guiInfo.hwndCaret : nullptr),
                           ok ? guiInfo.rcCaret.left : 0,
                           ok ? guiInfo.rcCaret.top : 0,
                           ok ? guiInfo.rcCaret.right : 0,
                           ok ? guiInfo.rcCaret.bottom : 0,
                           ok ? guiInfo.flags : 0);
    LogImeHwndDiagnostic(message);

    if (ok)
        LogSingleHwndIdentity(label, L"gui_focus", guiInfo.hwndFocus);
}

static void LogImeHwndSnapshot(HWND hwnd, const wchar_t *label) {
    if (!kEnableImeHwndDiagnostics)
        return;

    HWND foreground = GetForegroundWindow();
    HWND focus = GetFocus();
    HWND active = GetActiveWindow();
    const DWORD currentThread = GetCurrentThreadId();
    const DWORD windowThread = hwnd ? GetWindowThreadProcessId(hwnd, nullptr) : 0;
    const DWORD foregroundThread = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    const DWORD focusThread = focus ? GetWindowThreadProcessId(focus, nullptr) : 0;
    const DWORD activeThread = active ? GetWindowThreadProcessId(active, nullptr) : 0;
    HKL activeHkl = GetKeyboardLayout(0);
    HIMC context = hwnd ? ImmGetContext(hwnd) : nullptr;
    const bool hasContext = context != nullptr;
    BOOL imeOpen = FALSE;
    BOOL conversionOk = FALSE;
    DWORD conversion = 0;
    DWORD sentence = 0;
    BOOL releaseOk = FALSE;

    if (context) {
        imeOpen = ImmGetOpenStatus(context);
        conversionOk = ImmGetConversionStatus(context, &conversion, &sentence);
        releaseOk = ImmReleaseContext(hwnd, context);
    }

    std::wstring message;
    AppendDiagnosticFormat(message,
                           L"%s hwnd=%p hooked_hwnd=%p foreground=%p active=%p focus=%p current_tid=%lu window_tid=%lu foreground_tid=%lu active_tid=%lu focus_tid=%lu hkl=%p himc=%p himc_present=%d ime_open=%d conv_ok=%d conversion=0x%08lx sentence=0x%08lx release_ok=%d input_mode=%d pending_enter_input=%d pending_enter_submit=%d pending_paste=%d ime_open_cached=%d",
                           label,
                           static_cast<void *>(hwnd),
                           static_cast<void *>(g_hWnd),
                           static_cast<void *>(foreground),
                           static_cast<void *>(active),
                           static_cast<void *>(focus),
                           currentThread,
                           windowThread,
                           foregroundThread,
                           activeThread,
                           focusThread,
                           static_cast<void *>(activeHkl),
                           static_cast<void *>(context),
                           hasContext ? 1 : 0,
                           imeOpen ? 1 : 0,
                           conversionOk ? 1 : 0,
                           conversion,
                           sentence,
                           releaseOk ? 1 : 0,
                           g_InputMode ? 1 : 0,
                           g_PendingInputModeOnEnterUp ? 1 : 0,
                           g_PendingSubmitOnEnterUp ? 1 : 0,
                           g_PendingPasteOnShortcutRelease ? 1 : 0,
                           g_ImeOpen ? 1 : 0);
    LogImeHwndDiagnostic(message);
    LogSingleHwndIdentity(label, L"message", hwnd);
    if (g_hWnd != hwnd)
        LogSingleHwndIdentity(label, L"hooked", g_hWnd);
    if (focus && focus != hwnd && focus != g_hWnd)
        LogSingleHwndIdentity(label, L"focus", focus);
    if (active && active != hwnd && active != g_hWnd && active != focus)
        LogSingleHwndIdentity(label, L"active", active);
    if (foreground && foreground != hwnd && foreground != g_hWnd && foreground != focus && foreground != active)
        LogSingleHwndIdentity(label, L"foreground", foreground);
    LogGuiThreadInfo(label, windowThread);
    if (foregroundThread != 0 && foregroundThread != windowThread)
        LogGuiThreadInfo(label, foregroundThread);
}

static void LogTextBearingMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    const BOOL unicodeWindow = hwnd ? IsWindowUnicode(hwnd) : FALSE;
    std::size_t unitCount = 0;
    const wchar_t *firstClass = L"empty";
    const wchar_t *status = L"empty";

    if (msg == WM_CHAR) {
        unitCount = 1;
        const auto character = static_cast<wchar_t>(wParam & 0xFFFF);
        firstClass = CodePointClass(character);
        status = character == 0 ? L"empty" : L"code_unit_present";
    } else if (unicodeWindow) {
        unitCount = 1;
        const auto character = static_cast<wchar_t>(wParam & 0xFFFF);
        firstClass = CodePointClass(character);
        status = character == 0 ? L"empty" : L"unicode_code_unit_present";
    } else {
        firstClass = AnsiImeCharClass(wParam, unitCount);
        status = unitCount == 0 ? L"empty" : L"ansi_units_present";
    }

    std::wstring message;
    AppendDiagnosticFormat(message,
                           L"observed msg=%s(0x%04x) hwnd=%p text_message=1 unit_count=%zu first_codepoint_class=%s status=%s repeat_count=%lu is_unicode_window=%s input_mode=%d pending_enter_input=%d pending_enter_submit=%d",
                           MessageName(msg),
                           msg,
                           static_cast<void *>(hwnd),
                           unitCount,
                           firstClass,
                           status,
                           static_cast<DWORD>(lParam & 0xFFFF),
                           BoolText(unicodeWindow != FALSE),
                           g_InputMode ? 1 : 0,
                           g_PendingInputModeOnEnterUp ? 1 : 0,
                           g_PendingSubmitOnEnterUp ? 1 : 0);
    LogImeHwndDiagnostic(message);
}

static void LogImeHwndMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (!kEnableImeHwndDiagnostics || !ShouldLogDiagnosticMessage(msg))
        return;

    if (msg == WM_CHAR || msg == WM_IME_CHAR) {
        LogTextBearingMessage(hwnd, msg, wParam, lParam);
        LogImeHwndSnapshot(hwnd, MessageName(msg));
        return;
    }

    std::wstring message;
    AppendDiagnosticFormat(message,
                           L"observed msg=%s(0x%04x) hwnd=%p wParam=0x%Ix lParam=0x%Ix gcs_result=%d gcs_comp=%d input_mode=%d pending_enter_input=%d pending_enter_submit=%d",
                           MessageName(msg),
                           msg,
                           static_cast<void *>(hwnd),
                           static_cast<std::uintptr_t>(wParam),
                           static_cast<std::uintptr_t>(lParam),
                           (msg == WM_IME_COMPOSITION && (lParam & GCS_RESULTSTR)) ? 1 : 0,
                           (msg == WM_IME_COMPOSITION && (lParam & GCS_COMPSTR)) ? 1 : 0,
                           g_InputMode ? 1 : 0,
                           g_PendingInputModeOnEnterUp ? 1 : 0,
                           g_PendingSubmitOnEnterUp ? 1 : 0);
    LogImeHwndDiagnostic(message);
    LogImeHwndSnapshot(hwnd, MessageName(msg));
}

static void LogImeResultSummary(HWND hwnd, LONG byteLength, LONG copiedBytes, const wchar_t *text,
                                std::size_t utf16Length) {
    if (!kEnableImeHwndDiagnostics)
        return;

    std::wstring message;
    AppendDiagnosticFormat(message,
                           L"GCS_RESULTSTR hwnd=%p byte_length=%ld copied_bytes=%ld utf16_length=%zu status=%s first_codepoint_class=%s",
                           static_cast<void *>(hwnd),
                           byteLength,
                           copiedBytes,
                           utf16Length,
                           utf16Length == 0 ? L"empty" : L"non-empty",
                           FirstCodePointClass(text, utf16Length));
    LogImeHwndDiagnostic(message);
}

static void LogImeCharCapture(HWND hwnd, WPARAM, bool captured, const std::wstring &capturedText,
                              const wchar_t *status) {
    if (!kEnableImeHwndDiagnostics)
        return;

    std::wstring message;
    AppendDiagnosticFormat(message,
                           L"WM_IME_CHAR_CAPTURE hwnd=%p captured=%s appended_length=%zu total_utf16_length=%zu status=%s first_codepoint_class=%s is_unicode_window=%s",
                           static_cast<void *>(hwnd),
                           BoolText(captured),
                           capturedText.length(),
                           g_LastResultText.length(),
                           status,
                           FirstCodePointClass(capturedText.c_str(), capturedText.length()),
                           BoolText(IsWindowUnicode(hwnd) != FALSE));
    LogImeHwndDiagnostic(message);
}

static bool TryCaptureImeChar(HWND hwnd, WPARAM wParam) {
    std::wstring capturedText;
    const bool unicodeWindow = IsWindowUnicode(hwnd) != FALSE;

    if (unicodeWindow) {
        const auto character = static_cast<wchar_t>(wParam & 0xFFFF);
        if (character < 0x80) {
            LogImeCharCapture(hwnd, wParam, false, capturedText, L"unicode_ascii_or_control_ignored");
            return false;
        }

        capturedText.assign(1, character);
    } else {
        char bytes[2] = {};
        int byteCount = 0;
        if ((wParam & 0xFF00) != 0)
            bytes[byteCount++] = static_cast<char>((wParam >> 8) & 0xFF);
        if ((wParam & 0x00FF) != 0)
            bytes[byteCount++] = static_cast<char>(wParam & 0xFF);

        if (byteCount == 0 || (byteCount == 1 && static_cast<unsigned char>(bytes[0]) < 0x80)) {
            LogImeCharCapture(hwnd, wParam, false, capturedText, L"ansi_ascii_or_empty_ignored");
            return false;
        }

        wchar_t converted[2] = {};
        const int convertedLength = MultiByteToWideChar(936, 0, bytes, byteCount, converted, _countof(converted));
        if (convertedLength <= 0) {
            LogImeCharCapture(hwnd, wParam, false, capturedText, L"ansi_cp936_convert_failed");
            return false;
        }

        capturedText.assign(converted, convertedLength);
    }

    g_LastResultText += capturedText;
    LogImeCharCapture(hwnd, wParam, true, capturedText, L"non_ascii_appended");
    return true;
}

static void LogImeCaptureSummary(HWND hwnd, const wchar_t *label, DWORD gcsFlag, LONG byteLength, LONG copiedBytes,
                                 const wchar_t *text, std::size_t utf16Length, const wchar_t *status) {
    if (!kEnableImeHwndDiagnostics)
        return;

    std::wstring message;
    AppendDiagnosticFormat(message,
                           L"%s hwnd=%p gcs_flag=0x%08lx byte_length=%ld copied_bytes=%ld utf16_length=%zu status=%s first_codepoint_class=%s",
                           label,
                           static_cast<void *>(hwnd),
                           gcsFlag,
                           byteLength,
                           copiedBytes,
                           utf16Length,
                           status,
                           FirstCodePointClass(text, utf16Length));
    LogImeHwndDiagnostic(message);
}

static bool TryCaptureImeCompositionString(HWND hwnd, DWORD gcsFlag, const wchar_t *label) {
    HIMC context = ImmGetContext(hwnd);
    if (!context) {
        LogImeCaptureSummary(hwnd, label, gcsFlag, 0, 0, nullptr, 0, L"no_context");
        return false;
    }

    const LONG size = ImmGetCompositionStringW(context, gcsFlag, nullptr, 0);
    if (size <= 0) {
        LogImeCaptureSummary(hwnd, label, gcsFlag, size, 0, nullptr, 0, size == 0 ? L"empty" : L"error_size");
        ImmReleaseContext(hwnd, context);
        return false;
    }

    std::vector<wchar_t> buffer((size / sizeof(wchar_t)) + 1, L'\0');
    const LONG copied = ImmGetCompositionStringW(context, gcsFlag, buffer.data(), size);
    ImmReleaseContext(hwnd, context);

    if (copied <= 0) {
        LogImeCaptureSummary(hwnd, label, gcsFlag, size, copied, nullptr, 0, copied == 0 ? L"empty" : L"error_copy");
        return false;
    }

    const std::size_t utf16Length = copied / sizeof(wchar_t);
    g_LastResultText.assign(buffer.data(), utf16Length);
    LogImeCaptureSummary(hwnd, label, gcsFlag, size, copied, g_LastResultText.c_str(), g_LastResultText.length(),
                         gcsFlag == GCS_COMPSTR ? L"fallback_preedit_non_empty" : L"fallback_non_empty");
    return !g_LastResultText.empty();
}

static void CompleteImeCompositionForSubmit(HWND hwnd) {
    LogImeHwndSnapshot(hwnd, L"CompleteImeCompositionForSubmit:before");

    HIMC context = ImmGetContext(hwnd);
    if (!context) {
        LogImeHwndDiagnostic(L"CompleteImeCompositionForSubmit:no_context");
        LogImeHwndSnapshot(hwnd, L"CompleteImeCompositionForSubmit:after_no_context");
        return;
    }

    const BOOL completeOk = ImmNotifyIME(context, NI_COMPOSITIONSTR, CPS_COMPLETE, 0);
    if (kEnableImeHwndDiagnostics) {
        std::wstring message;
        AppendDiagnosticFormat(message,
                               L"CompleteImeCompositionForSubmit notify hwnd=%p himc=%p complete_ok=%s",
                               static_cast<void *>(hwnd),
                               static_cast<void *>(context),
                               BoolText(completeOk != FALSE));
        LogImeHwndDiagnostic(message);
    }

    ImmReleaseContext(hwnd, context);
    LogImeHwndSnapshot(hwnd, L"CompleteImeCompositionForSubmit:after");
}

static void CaptureSubmitTextFallback(HWND hwnd) {
    if (!g_LastResultText.empty()) {
        TextSubmission summary = {g_LastResultText, true, true};
        LogSubmissionDiagnostic(L"CaptureSubmitTextFallback:skip_existing_last_result", hwnd, summary);
        return;
    }

    LogImeHwndSnapshot(hwnd, L"CaptureSubmitTextFallback:before_GCS_RESULTSTR");
    TryCaptureImeCompositionString(hwnd, GCS_RESULTSTR, L"CaptureSubmitTextFallback:GCS_RESULTSTR");
    if (!g_LastResultText.empty()) {
        TextSubmission summary = {g_LastResultText, true, true};
        LogSubmissionDiagnostic(L"CaptureSubmitTextFallback:resultstr_populated_last_result", hwnd, summary);
        return;
    }

    LogImeHwndSnapshot(hwnd, L"CaptureSubmitTextFallback:before_GCS_COMPSTR");
    TryCaptureImeCompositionString(hwnd, GCS_COMPSTR, L"CaptureSubmitTextFallback:GCS_COMPSTR_preedit_fallback");
    TextSubmission summary = {g_LastResultText, true, true};
    LogSubmissionDiagnostic(g_LastResultText.empty()
                                ? L"CaptureSubmitTextFallback:last_result_empty_after_fallback"
                                : L"CaptureSubmitTextFallback:compstr_populated_last_result",
                            hwnd, summary);
}


static void SetImeEnabled(HWND hwnd, bool enabled);

static void SetImeEnabled(HWND hwnd, bool enabled, bool activateGameplayLayout);

static void SetInputMode(HWND hwnd, bool enabled);

static HKL GetGameplayKeyboardLayout() {
    if (!g_GameplayKeyboardLayout)
        g_GameplayKeyboardLayout = LoadKeyboardLayoutW(L"00000409", KLF_NOTELLSHELL);

    return g_GameplayKeyboardLayout;
}

static void ActivateKeyboardLayoutForGameplay() {
    const HKL gameplayLayout = GetGameplayKeyboardLayout();
    if (!gameplayLayout)
        return;

    const HKL currentLayout = GetKeyboardLayout(0);
    if (currentLayout == gameplayLayout) {
        g_GameplayLayoutActive = true;
        return;
    }

    if (!g_GameplayLayoutActive && currentLayout)
        g_TextKeyboardLayout = currentLayout;

    g_ChangingKeyboardLayout = true;
    ActivateKeyboardLayout(gameplayLayout, 0);
    g_ChangingKeyboardLayout = false;
    g_GameplayLayoutActive = true;
}

static void RestoreTextKeyboardLayout() {
    g_GameplayLayoutActive = false;

    if (!g_TextKeyboardLayout)
        return;

    if (GetKeyboardLayout(0) == g_TextKeyboardLayout)
        return;

    g_ChangingKeyboardLayout = true;
    ActivateKeyboardLayout(g_TextKeyboardLayout, 0);
    g_ChangingKeyboardLayout = false;
}

static void RestoreKeyboardLayoutForWindow(HWND targetHwnd) {
    if (!g_TextKeyboardLayout)
        return;

    if (!targetHwnd)
        targetHwnd = GetForegroundWindow();

    if (!targetHwnd || targetHwnd == g_hWnd)
        return;

    const DWORD currentThread = GetCurrentThreadId();
    const DWORD targetThread = GetWindowThreadProcessId(targetHwnd, nullptr);
    const bool attached = targetThread != 0 && targetThread != currentThread &&
                          AttachThreadInput(currentThread, targetThread, TRUE) != FALSE;

    g_ChangingKeyboardLayout = true;
    ActivateKeyboardLayout(g_TextKeyboardLayout, 0);
    g_ChangingKeyboardLayout = false;

    if (attached)
        AttachThreadInput(currentThread, targetThread, FALSE);

    PostMessageW(targetHwnd, WM_INPUTLANGCHANGEREQUEST, 0, reinterpret_cast<LPARAM>(g_TextKeyboardLayout));
}

static void RestoreLayoutAfterGameplay(HWND targetHwnd) {
    RestoreTextKeyboardLayout();
    RestoreKeyboardLayoutForWindow(targetHwnd);
}

static DWORD WINAPI RestoreLayoutAfterGameplayWorker(LPVOID) {
    Sleep(50);
    RestoreKeyboardLayoutForWindow(GetForegroundWindow());
    Sleep(150);
    RestoreKeyboardLayoutForWindow(GetForegroundWindow());
    return 0;
}

static void ScheduleRestoreLayoutAfterGameplay(HWND targetHwnd) {
    RestoreLayoutAfterGameplay(targetHwnd);

    HANDLE thread = CreateThread(nullptr, 0, &RestoreLayoutAfterGameplayWorker, nullptr, 0, nullptr);
    if (thread)
        CloseHandle(thread);
}

static void ActivateGameplayLayoutWhenSafe() {
    ActivateKeyboardLayoutForGameplay();
}

static bool IsModifierKey(WPARAM key) {
    return key == VK_SHIFT || key == VK_LSHIFT || key == VK_RSHIFT ||
           key == VK_CONTROL || key == VK_LCONTROL || key == VK_RCONTROL ||
           key == VK_MENU || key == VK_LMENU || key == VK_RMENU ||
           key == VK_LWIN || key == VK_RWIN;
}

static DWORD WINAPI RunSubmissionWorker(LPVOID) {
    for (;;) {
        TextSubmission submission;
        {
            std::unique_lock<std::mutex> lock(g_SubmissionMutex);
            g_SubmissionCv.wait(lock, [] {
                return g_StopSubmissionThread.load() || !g_PendingSubmissions.empty();
            });

            if (g_StopSubmissionThread.load())
                return 0;

            submission = std::move(g_PendingSubmissions.front());
            g_PendingSubmissions.pop_front();
        }

        if (submission.text.empty() && !submission.sendEnter)
            continue;

        LogSubmissionDiagnostic(L"RunSubmissionWorker:dequeued", g_hWnd, submission);

        if (GetForegroundWindow() != g_hWnd) {
            LogSubmissionDiagnostic(L"RunSubmissionWorker:skip_foreground_mismatch", g_hWnd, submission);
            continue;
        }

        if (!submission.text.empty()) {
            LogSubmissionDiagnostic(L"RunSubmissionWorker:before_SendText", g_hWnd, submission);
            SendText(submission.text, submission.useAltCode);
            LogSubmissionDiagnostic(L"RunSubmissionWorker:after_SendText", g_hWnd, submission);
        } else {
            LogSubmissionDiagnostic(L"RunSubmissionWorker:no_text_send_enter_only", g_hWnd, submission);
        }

        if (!submission.sendEnter && GetForegroundWindow() == g_hWnd) {
            LogSubmissionDiagnostic(L"RunSubmissionWorker:post_WM_APP_SUBMIT_TEXT_restore", g_hWnd, submission);
            PostMessage(g_hWnd, WM_APP_SUBMIT_TEXT, 1, 0);
        }

        if (submission.sendEnter && GetForegroundWindow() == g_hWnd) {
            LogSubmissionDiagnostic(L"RunSubmissionWorker:post_WM_APP_SUBMIT_TEXT_enter", g_hWnd, submission);
            PostMessage(g_hWnd, WM_APP_SUBMIT_TEXT, 2, 0);
        } else if (submission.sendEnter) {
            LogSubmissionDiagnostic(L"RunSubmissionWorker:skip_enter_post_foreground_mismatch", g_hWnd, submission);
        }
    }
}

static void EnsureSubmissionWorkerRunning() {
    if (g_SubmissionThread)
        return;

    g_StopSubmissionThread = false;
    g_SubmissionThread = CreateThread(nullptr, 0, &RunSubmissionWorker, nullptr, 0, nullptr);
}

static void CancelTextSubmission() {
    std::lock_guard<std::mutex> lock(g_SubmissionMutex);
    g_PendingSubmissions.clear();
}

static void StopSubmissionWorker() {
    {
        std::lock_guard<std::mutex> lock(g_SubmissionMutex);
        g_PendingSubmissions.clear();
        g_StopSubmissionThread = true;
    }
    g_SubmissionCv.notify_all();

    if (g_SubmissionThread) {
        WaitForSingleObject(g_SubmissionThread, INFINITE);
        CloseHandle(g_SubmissionThread);
        g_SubmissionThread = nullptr;
    }
}

static void QueueTextSubmission(HWND hwnd, std::wstring text, bool sendEnter, bool useAltCode) {
    TextSubmission submission = {std::move(text), sendEnter, useAltCode};
    if (submission.text.empty() && !submission.sendEnter) {
        LogSubmissionDiagnostic(L"QueueTextSubmission:skip_empty_no_enter", hwnd, submission);
        return;
    }

    LogSubmissionDiagnostic(L"QueueTextSubmission:before_enqueue", hwnd, submission);

    EnsureSubmissionWorkerRunning();

    std::size_t queueDepth = 0;
    TextSubmission queuedSummary = {submission.text, sendEnter, useAltCode};
    {
        std::lock_guard<std::mutex> lock(g_SubmissionMutex);
        g_PendingSubmissions.push_back(std::move(submission));
        queueDepth = g_PendingSubmissions.size();
    }

    LogSubmissionDiagnostic(L"QueueTextSubmission:after_enqueue", hwnd, queuedSummary, queueDepth);

    g_SubmissionCv.notify_one();

    PostMessage(hwnd, WM_APP_SUBMIT_TEXT, 0, 0);
    LogSubmissionDiagnostic(L"QueueTextSubmission:posted_worker_wakeup", hwnd, queuedSummary, queueDepth);
}

static void QueueSubmitResultText(HWND hwnd) {
    TextSubmission summary = {g_LastResultText, true, true};
    LogSubmissionDiagnostic(L"QueueSubmitResultText:last_result", hwnd, summary);
    QueueTextSubmission(hwnd, std::move(g_LastResultText), true, true);
    g_LastResultText.clear();
}

static void QueuePasteText(HWND hwnd) {
    g_RestoreImeAfterPaste = g_InputMode;
    if (g_RestoreImeAfterPaste)
        SetImeEnabled(hwnd, false);

    QueueTextSubmission(hwnd, GetClipboard(), false, false);
}

static void ClearPendingPaste() {
    g_PendingPasteOnShortcutRelease = false;
    g_PendingPasteKey = 0;
    g_PendingPasteModifierKey = 0;
    g_PendingPasteKeyReleased = false;
    g_PendingPasteModifierReleased = false;
}

static void BeginPendingPaste(WPARAM key) {
    g_PendingPasteOnShortcutRelease = true;
    g_PendingPasteKey = key;
    g_PendingPasteModifierKey = (key == 'V') ? VK_CONTROL : VK_SHIFT;
    g_PendingPasteKeyReleased = false;
    g_PendingPasteModifierReleased = false;
}

static bool TryQueuePendingPaste(HWND hwnd) {
    if (!g_PendingPasteOnShortcutRelease)
        return false;

    if (!g_PendingPasteKeyReleased || !g_PendingPasteModifierReleased)
        return false;

    ClearPendingPaste();
    QueuePasteText(hwnd);
    return true;
}

static void SendSubmitEnter(HWND hwnd) {
    TextSubmission summary = {std::wstring(), true, true};
    LogSubmissionDiagnostic(L"SendSubmitEnter:before_original_proc", hwnd, summary);
    const UINT scan_code = MapVirtualKeyW(VK_RETURN, MAPVK_VK_TO_VSC);
    const LPARAM keydown_lparam = 1 | (static_cast<LPARAM>(scan_code) << 16);
    const LPARAM keyup_lparam = keydown_lparam | (1LL << 30) | (1LL << 31);

    CallWindowProc(g_OriginalWndProc, hwnd, WM_KEYDOWN, VK_RETURN, keydown_lparam);
    CallWindowProc(g_OriginalWndProc, hwnd, WM_KEYUP, VK_RETURN, keyup_lparam);
    LogSubmissionDiagnostic(L"SendSubmitEnter:after_original_proc", hwnd, summary);
}

static WPARAM RestoreImeProcessedKey(HWND hwnd, WPARAM key, LPARAM lParam) {
    if (key != VK_PROCESSKEY)
        return key;

    const UINT originalKey = ImmGetVirtualKey(hwnd);
    if (originalKey == 0 || originalKey == VK_PROCESSKEY) {
        UINT scanCode = (static_cast<UINT>(lParam) >> 16) & 0xFF;
        if ((lParam & (1UL << 24)) != 0)
            scanCode |= 0xE000;

        const UINT mappedKey = MapVirtualKeyW(scanCode, MAPVK_VSC_TO_VK_EX);
        if (mappedKey != 0 && mappedKey != VK_PROCESSKEY)
            return static_cast<WPARAM>(mappedKey);

        return key;
    }

    return static_cast<WPARAM>(originalKey);
}

static WPARAM ResolveImeProcessedControlKey(HWND hwnd, WPARAM key, LPARAM lParam) {
    if (key != VK_PROCESSKEY)
        return key;

    const UINT originalKey = ImmGetVirtualKey(hwnd);
    if (originalKey == VK_RETURN || originalKey == VK_ESCAPE)
        return static_cast<WPARAM>(originalKey);

    UINT scanCode = (static_cast<UINT>(lParam) >> 16) & 0xFF;
    if ((lParam & (1UL << 24)) != 0)
        scanCode |= 0xE000;

    const UINT mappedKey = MapVirtualKeyW(scanCode, MAPVK_VSC_TO_VK_EX);
    if (mappedKey == VK_RETURN || mappedKey == VK_ESCAPE)
        return static_cast<WPARAM>(mappedKey);

    return key;
}

static void CancelImeComposition(HWND hwnd) {
    HIMC context = ImmGetContext(hwnd);
    if (context) {
        ImmNotifyIME(context, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
        ImmNotifyIME(context, NI_CLOSECANDIDATE, 0, 0);
        ImmReleaseContext(hwnd, context);
    }

    SetImeEnabled(hwnd, false, false);
}

static LRESULT HandleEnterKey(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    LogImeHwndSnapshot(hwnd, L"HandleEnterKey:keydown:before");
    if (!g_InputMode) {
        g_PendingInputModeOnEnterUp = true;
        g_LastResultText.clear();
        LogImeHwndSnapshot(hwnd, L"HandleEnterKey:first_enter_passthrough");
        return CallWindowProc(g_OriginalWndProc, hwnd, msg, wParam, lParam);
    }

    CompleteImeCompositionForSubmit(hwnd);
    CaptureSubmitTextFallback(hwnd);
    SetImeEnabled(hwnd, false);
    g_InputMode = false;
    g_PendingInputModeOnEnterUp = false;
    g_PendingSubmitOnEnterUp = true;
    LogImeHwndSnapshot(hwnd, L"HandleEnterKey:submit_pending");
    return 0;
}

static LRESULT HandleEscapeKey(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    LogImeHwndSnapshot(hwnd, L"HandleEscapeKey:before_clear");
    SetImeEnabled(hwnd, false);
    g_InputMode = false;
    g_PendingInputModeOnEnterUp = false;
    g_PendingSubmitOnEnterUp = false;
    ClearPendingPaste();
    CancelTextSubmission();
    g_LastResultText.clear();
    LogImeHwndSnapshot(hwnd, L"HandleEscapeKey:before_original_proc");
    const LRESULT result = CallWindowProc(g_OriginalWndProc, hwnd, msg, wParam, lParam);
    LogImeHwndSnapshot(hwnd, L"HandleEscapeKey:after_original_proc");
    return result;
}

static bool IsPasteShortcut(WPARAM key) {
    const bool ctrlDown = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shiftDown = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    return (key == 'V' && ctrlDown) || (key == VK_INSERT && shiftDown);
}

static bool IsSyntheticAltCodeKeyMessage(UINT msg, WPARAM key) {
    if (msg != WM_KEYDOWN && msg != WM_KEYUP && msg != WM_SYSKEYDOWN && msg != WM_SYSKEYUP)
        return false;

    if (static_cast<std::uintptr_t>(GetMessageExtraInfo()) != kSyntheticInputExtraInfo)
        return false;

    return key == VK_MENU || key == VK_LMENU || key == VK_RMENU ||
           key == VK_CONTROL || key == VK_LCONTROL || key == VK_RCONTROL ||
           key == VK_SHIFT || key == VK_LSHIFT || key == VK_RSHIFT ||
           key == VK_LWIN || key == VK_RWIN || key == VK_NUMLOCK ||
           (key >= VK_NUMPAD0 && key <= VK_NUMPAD9);
}

static HIMC EnsureImeContext(HWND hwnd) {
    LogImeHwndSnapshot(hwnd, L"EnsureImeContext:before");

    const BOOL defaultRestoreOk = ImmAssociateContextEx(hwnd, nullptr, IACE_DEFAULT);
    HIMC context = ImmGetContext(hwnd);
    if (kEnableImeHwndDiagnostics) {
        std::wstring associateMessage;
        AppendDiagnosticFormat(associateMessage,
                               L"EnsureImeContext default_restore hwnd=%p restore_ok=%s himc_after_default=%p himc_present_after_default=%s",
                               static_cast<void *>(hwnd),
                               BoolText(defaultRestoreOk != FALSE),
                               static_cast<void *>(context),
                               BoolText(context != nullptr));
        LogImeHwndDiagnostic(associateMessage);
    }

    LogImeHwndSnapshot(hwnd, L"EnsureImeContext:after_default_restore");
    return context;
}

static void SetImeWindowPositionsForGame(HWND hwnd, HIMC context) {
    RECT clientRect = {};
    const BOOL clientOk = GetClientRect(hwnd, &clientRect);
    POINT compositionPoint = {0, 0};
    POINT candidatePoint = {16, 16};
    LONG clientWidth = 0;
    LONG clientHeight = 0;

    if (clientOk) {
        clientWidth = clientRect.right - clientRect.left;
        clientHeight = clientRect.bottom - clientRect.top;
        compositionPoint.x = 16;
        compositionPoint.y = clientHeight > 32 ? clientHeight - 32 : 0;
        candidatePoint.x = compositionPoint.x;
        candidatePoint.y = compositionPoint.y;
    }

    COMPOSITIONFORM compositionForm = {};
    compositionForm.dwStyle = CFS_POINT;
    compositionForm.ptCurrentPos = compositionPoint;
    const BOOL compositionOk = ImmSetCompositionWindow(context, &compositionForm);

    CANDIDATEFORM candidateForm = {};
    candidateForm.dwIndex = 0;
    candidateForm.dwStyle = CFS_CANDIDATEPOS;
    candidateForm.ptCurrentPos = candidatePoint;
    const BOOL candidateOk = ImmSetCandidateWindow(context, &candidateForm);

    if (kEnableImeHwndDiagnostics) {
        std::wstring compositionMessage;
        AppendDiagnosticFormat(compositionMessage,
                               L"SetImeWindowPositionsForGame composition hwnd=%p himc=%p client_ok=%s client_width=%ld client_height=%ld point_x=%ld point_y=%ld style=0x%08lx set_ok=%s",
                               static_cast<void *>(hwnd),
                               static_cast<void *>(context),
                               BoolText(clientOk != FALSE),
                               clientWidth,
                               clientHeight,
                               compositionPoint.x,
                               compositionPoint.y,
                               compositionForm.dwStyle,
                               BoolText(compositionOk != FALSE));
        LogImeHwndDiagnostic(compositionMessage);

        std::wstring candidateMessage;
        AppendDiagnosticFormat(candidateMessage,
                               L"SetImeWindowPositionsForGame candidate hwnd=%p himc=%p client_ok=%s client_width=%ld client_height=%ld point_x=%ld point_y=%ld index=%lu style=0x%08lx set_ok=%s",
                               static_cast<void *>(hwnd),
                               static_cast<void *>(context),
                               BoolText(clientOk != FALSE),
                               clientWidth,
                               clientHeight,
                               candidatePoint.x,
                               candidatePoint.y,
                               candidateForm.dwIndex,
                               candidateForm.dwStyle,
                               BoolText(candidateOk != FALSE));
        LogImeHwndDiagnostic(candidateMessage);
    }
}

static void SetImeEnabled(HWND hwnd, bool enabled) {
    SetImeEnabled(hwnd, enabled, true);
}

static void SetImeEnabled(HWND hwnd, bool enabled, bool activateGameplayLayout) {
    LogImeHwndSnapshot(hwnd, enabled ? L"SetImeEnabled:enable:before" : L"SetImeEnabled:disable:before");

    if (enabled)
        RestoreTextKeyboardLayout();

    HIMC context = enabled ? EnsureImeContext(hwnd) : ImmGetContext(hwnd);
    if (!context) {
        if (!enabled) {
            ImmAssociateContextEx(hwnd, nullptr, 0);
            ImmAssociateContextEx(hwnd, nullptr, IACE_CHILDREN);
            if (activateGameplayLayout)
                ActivateGameplayLayoutWhenSafe();
        }
        g_ImeOpen = false;
        LogImeHwndSnapshot(hwnd, enabled ? L"SetImeEnabled:enable:no_context" : L"SetImeEnabled:disable:no_context");
        return;
    }

    if (enabled) {
        DWORD conversion = 0;
        DWORD sentence = 0;
        const BOOL conversionOk = ImmGetConversionStatus(context, &conversion, &sentence);
        if (conversionOk) {
            const DWORD requestedConversion = conversion | IME_CMODE_NATIVE;
            const BOOL setConversionOk = ImmSetConversionStatus(context, requestedConversion, sentence);
            if (kEnableImeHwndDiagnostics) {
                std::wstring conversionMessage;
                AppendDiagnosticFormat(conversionMessage,
                                       L"SetImeEnabled conversion hwnd=%p himc=%p get_ok=%d before=0x%08lx requested=0x%08lx sentence=0x%08lx set_ok=%d",
                                       static_cast<void *>(hwnd),
                                       static_cast<void *>(context),
                                       conversionOk ? 1 : 0,
                                       conversion,
                                       requestedConversion,
                                       sentence,
                                       setConversionOk ? 1 : 0);
                LogImeHwndDiagnostic(conversionMessage);
            }
        } else if (kEnableImeHwndDiagnostics) {
            std::wstring conversionMessage;
            AppendDiagnosticFormat(conversionMessage,
                                   L"SetImeEnabled conversion hwnd=%p himc=%p get_ok=0",
                                   static_cast<void *>(hwnd),
                                   static_cast<void *>(context));
            LogImeHwndDiagnostic(conversionMessage);
        }
    }

    const BOOL setOpenOk = ImmSetOpenStatus(context, enabled ? TRUE : FALSE);
    if (kEnableImeHwndDiagnostics) {
        std::wstring openMessage;
        AppendDiagnosticFormat(openMessage,
                               L"SetImeEnabled open hwnd=%p himc=%p requested=%d set_ok=%d open_after=%d",
                               static_cast<void *>(hwnd),
                               static_cast<void *>(context),
                               enabled ? 1 : 0,
                               setOpenOk ? 1 : 0,
                               ImmGetOpenStatus(context) ? 1 : 0);
        LogImeHwndDiagnostic(openMessage);
    }
    if (enabled)
        SetImeWindowPositionsForGame(hwnd, context);
    ImmReleaseContext(hwnd, context);

    if (!enabled) {
        ImmAssociateContextEx(hwnd, nullptr, 0);
        ImmAssociateContextEx(hwnd, nullptr, IACE_CHILDREN);
        if (activateGameplayLayout)
            ActivateGameplayLayoutWhenSafe();
    }

    g_ImeOpen = enabled;
    LogImeHwndSnapshot(hwnd, enabled ? L"SetImeEnabled:enable:after" : L"SetImeEnabled:disable:after");
}

static void SetInputMode(HWND hwnd, bool enabled) {
    g_InputMode = enabled;
    SetImeEnabled(hwnd, enabled);
}

static void EnforceImeState(HWND hwnd) {
    SetImeEnabled(hwnd, g_InputMode);
}

static void DetachImeContext() {
    if (!g_hWnd)
        return;

    ImmAssociateContextEx(g_hWnd, nullptr, 0);
    g_ImeOpen = false;
}

static void RestoreWindowProc() {
    if (g_hWnd && g_OriginalWndProc) {
        SetWindowLongPtr(g_hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_OriginalWndProc));
        g_OriginalWndProc = nullptr;
    }
}

LRESULT CALLBACK ReplaceWindowFunc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (!(g_hWnd && g_OriginalWndProc))
        return DefWindowProc(hwnd, msg, wParam, lParam);

    LogImeHwndMessage(hwnd, msg, wParam, lParam);

    if (IsSyntheticAltCodeKeyMessage(msg, wParam))
        return 0;

    switch (msg) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            WPARAM controlKey = wParam;
            if (!g_InputMode) {
                wParam = RestoreImeProcessedKey(hwnd, wParam, lParam);
                controlKey = wParam;
                if (!IsModifierKey(wParam))
                    ActivateKeyboardLayoutForGameplay();
                CancelImeComposition(hwnd);
            } else {
                controlKey = ResolveImeProcessedControlKey(hwnd, wParam, lParam);
            }

            if (controlKey == VK_RETURN && g_PendingInputModeOnEnterUp)
                return 0;

            if ((lParam & (1UL << 30)) != 0)
                break;

            if (controlKey == VK_RETURN) {
                return HandleEnterKey(hwnd, msg, controlKey, lParam);
            }

            if (controlKey == VK_ESCAPE)
                return HandleEscapeKey(hwnd, msg, controlKey, lParam);

            if (g_InputMode && IsPasteShortcut(wParam)) {
                BeginPendingPaste(wParam);
                return 0;
            }
            break;
        }
        case WM_KEYUP:
        case WM_SYSKEYUP: {
            WPARAM controlKey = wParam;
            if (!g_InputMode) {
                wParam = RestoreImeProcessedKey(hwnd, wParam, lParam);
                controlKey = wParam;
                CancelImeComposition(hwnd);
            } else {
                controlKey = ResolveImeProcessedControlKey(hwnd, wParam, lParam);
            }

            if (controlKey == VK_RETURN && g_PendingInputModeOnEnterUp) {
                LogImeHwndSnapshot(hwnd, L"first_enter_keyup:before_original_proc");
                g_PendingInputModeOnEnterUp = false;
                const LRESULT result = CallWindowProc(g_OriginalWndProc, hwnd, msg, controlKey, lParam);
                LogImeHwndSnapshot(hwnd, L"first_enter_keyup:after_original_proc_before_input_mode");
                SetInputMode(hwnd, true);
                LogImeHwndSnapshot(hwnd, L"first_enter_keyup:after_input_mode");
                return result;
            }

            if (g_PendingPasteOnShortcutRelease) {
                if (wParam == g_PendingPasteKey) {
                    g_PendingPasteKeyReleased = true;
                    TryQueuePendingPaste(hwnd);
                    return 0;
                }

                if (wParam == g_PendingPasteModifierKey) {
                    g_PendingPasteModifierReleased = true;
                    if (TryQueuePendingPaste(hwnd))
                        return 0;
                }
            }

            if (controlKey == VK_RETURN && g_PendingSubmitOnEnterUp) {
                LogImeHwndSnapshot(hwnd, L"second_enter_keyup_submit:before_queue");
                g_PendingSubmitOnEnterUp = false;
                QueueSubmitResultText(hwnd);
                LogImeHwndSnapshot(hwnd, L"second_enter_keyup_submit:after_queue");
                return 0;
            }
            break;
        }
        case WM_APP_SUBMIT_TEXT:
            if (kEnableImeHwndDiagnostics) {
                std::wstring message;
                AppendDiagnosticFormat(message,
                                       L"WM_APP_SUBMIT_TEXT received hwnd=%p wParam=0x%Ix foreground=%p hooked_hwnd=%p restore_ime_after_paste=%d input_mode=%d",
                                       static_cast<void *>(hwnd),
                                       static_cast<std::uintptr_t>(wParam),
                                       static_cast<void *>(GetForegroundWindow()),
                                       static_cast<void *>(g_hWnd),
                                       g_RestoreImeAfterPaste ? 1 : 0,
                                       g_InputMode ? 1 : 0);
                LogImeHwndDiagnostic(message);
            }
            if (wParam == 2) {
                if (GetForegroundWindow() == hwnd)
                    SendSubmitEnter(hwnd);
                else
                    LogImeHwndDiagnostic(L"WM_APP_SUBMIT_TEXT enter skipped because foreground does not match hwnd");
                return 0;
            }

            if (wParam == 1) {
                if (g_RestoreImeAfterPaste && g_InputMode)
                    SetImeEnabled(hwnd, true);
                g_RestoreImeAfterPaste = false;
            }
            return 0;
        case WM_GETDLGCODE: {
            const auto dlg_code = static_cast<LRESULT>(CallWindowProc(g_OriginalWndProc, hwnd, msg, wParam, lParam));
            return dlg_code | DLGC_WANTALLKEYS;
        }
        case WM_IME_CHAR:
            if (g_InputMode && TryCaptureImeChar(hwnd, wParam))
                return 0;
            break;
        case WM_IME_COMPOSITION: {
            if (!g_InputMode) {
                LogImeHwndSnapshot(hwnd, L"WM_IME_COMPOSITION:not_input_mode_cancel");
                CancelImeComposition(hwnd);
                return 0;
            }

            if ((GCS_RESULTSTR & lParam) == 0) {
                TextSubmission summary = {g_LastResultText, true, true};
                LogSubmissionDiagnostic(L"WM_IME_COMPOSITION:no_resultstr", hwnd, summary);
            }

            if (GCS_RESULTSTR & lParam) {
                if (HIMC context = ImmGetContext(hwnd)) {
                    const LONG size = ImmGetCompositionStringW(context, GCS_RESULTSTR, nullptr, 0);
                    if (size > 0) {
                        std::vector<wchar_t> buffer((size / sizeof(wchar_t)) + 1, L'\0');
                        const LONG copied = ImmGetCompositionStringW(context, GCS_RESULTSTR, buffer.data(), size);
                        if (copied >= 0) {
                            g_LastResultText.assign(buffer.data(), copied / sizeof(wchar_t));
                            LogImeResultSummary(hwnd, size, copied, g_LastResultText.c_str(),
                                                g_LastResultText.length());
                        } else {
                            LogImeResultSummary(hwnd, size, copied, nullptr, 0);
                        }
                    } else {
                        g_LastResultText.clear();
                        LogImeResultSummary(hwnd, size, 0, nullptr, 0);
                    }

                    ImmReleaseContext(hwnd, context);
                } else {
                    LogImeHwndSnapshot(hwnd, L"WM_IME_COMPOSITION:GCS_RESULTSTR:no_context");
                }
            }
            break;
        }
        case WM_IME_STARTCOMPOSITION:
            if (!g_InputMode) {
                CancelImeComposition(hwnd);
                return 0;
            }
            break;
        case WM_IME_NOTIFY:
            if (!g_InputMode) {
                CancelImeComposition(hwnd);
                return 0;
            }
            break;
        case WM_IME_SETCONTEXT:
            if (kEnableImeHwndDiagnostics) {
                std::wstring message;
                AppendDiagnosticFormat(message,
                                       L"WM_IME_SETCONTEXT:chain_to_original hwnd=%p active=%s lParam=0x%Ix input_mode=%s pending_enter_input=%s pending_enter_submit=%s changing_keyboard_layout=%s ime_open_cached=%s",
                                       static_cast<void *>(hwnd),
                                       BoolText(wParam != 0),
                                       static_cast<std::uintptr_t>(lParam),
                                       BoolText(g_InputMode),
                                       BoolText(g_PendingInputModeOnEnterUp),
                                       BoolText(g_PendingSubmitOnEnterUp),
                                       BoolText(g_ChangingKeyboardLayout),
                                       BoolText(g_ImeOpen));
                LogImeHwndDiagnostic(message);
            }
            return CallWindowProc(g_OriginalWndProc, hwnd, msg, wParam, lParam);
        case WM_INPUTLANGCHANGE:
            if (g_ChangingKeyboardLayout)
                return 0;

            if (!g_InputMode)
                return 0;
            break;
        case WM_INPUTLANGCHANGEREQUEST:
            if (!g_InputMode) {
                SetImeEnabled(hwnd, false, false);
                return 0; // reject
            }
            break;
        case WM_ACTIVATE:
            g_WindowActive = LOWORD(wParam) != WA_INACTIVE;
            if (!g_WindowActive) {
                g_PendingInputModeOnEnterUp = false;
                SetImeEnabled(hwnd, false, false);
                ScheduleRestoreLayoutAfterGameplay(reinterpret_cast<HWND>(lParam));
                break;
            }

            if (!g_InputMode)
                CancelImeComposition(hwnd);
            break;
        case WM_ACTIVATEAPP:
            g_WindowActive = wParam != FALSE;
            if (!g_WindowActive) {
                g_PendingInputModeOnEnterUp = false;
                SetImeEnabled(hwnd, false, false);
                ScheduleRestoreLayoutAfterGameplay(GetForegroundWindow());
                break;
            }

            if (!g_InputMode)
                CancelImeComposition(hwnd);
            break;
        case WM_SETFOCUS:
            g_WindowActive = true;
            if (g_InputMode)
                EnforceImeState(hwnd);
            else
                SetImeEnabled(hwnd, false, false);
            break;
        case WM_KILLFOCUS:
            g_WindowActive = false;
            g_PendingInputModeOnEnterUp = false;
            ClearPendingPaste();
            CancelTextSubmission();
            g_RestoreImeAfterPaste = false;
            SetImeEnabled(hwnd, false, false);
            ScheduleRestoreLayoutAfterGameplay(reinterpret_cast<HWND>(wParam));
            break;
        default: break;
    }
    return CallWindowProc(g_OriginalWndProc, hwnd, msg, wParam, lParam);
}

static void OnReshadePresent(reshade::api::effect_runtime *runtime) {
    auto hwnd = static_cast<HWND>(runtime->get_hwnd());
    if (!hwnd)
        return;

    if (g_hWnd == hwnd && g_OriginalWndProc)
        return;

    LogImeHwndSnapshot(hwnd, L"OnReshadePresent:before_subclass");
    RestoreWindowProc();
    DetachImeContext();
    CancelTextSubmission();

    g_hWnd = hwnd;
    g_OriginalWndProc = reinterpret_cast<WNDPROC>(GetWindowLongPtr(hwnd, GWLP_WNDPROC));
    if (!g_OriginalWndProc) {
        LogImeHwndDiagnostic(L"OnReshadePresent GetWindowLongPtr returned null original wndproc");
        DetachImeContext();
        g_hWnd = nullptr;
        return;
    }

    SetLastError(ERROR_SUCCESS);
    const LONG_PTR previousWndProc =
            SetWindowLongPtr(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ReplaceWindowFunc));
    const DWORD setWindowLongError = GetLastError();
    if (kEnableImeHwndDiagnostics) {
        std::wstring message;
        AppendDiagnosticFormat(message,
                               L"OnReshadePresent subclass hwnd=%p original_wndproc=%p new_wndproc=%p returned_previous=%p last_error=%lu",
                               static_cast<void *>(hwnd),
                               reinterpret_cast<void *>(g_OriginalWndProc),
                               reinterpret_cast<void *>(ReplaceWindowFunc),
                               reinterpret_cast<void *>(previousWndProc),
                               setWindowLongError);
        LogImeHwndDiagnostic(message);
    }
    LogImeHwndSnapshot(hwnd, L"OnReshadePresent:after_subclass");
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    using namespace reshade;
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH:
            if (!register_addon(hinstDLL)) {
                return FALSE;
            }
            register_event<addon_event::reshade_present>(&OnReshadePresent);
            break;
        case DLL_PROCESS_DETACH:
            unregister_event<addon_event::reshade_present>(&OnReshadePresent);
            RestoreWindowProc();
            DetachImeContext();
            g_hWnd = nullptr;
            g_InputMode = false;
            g_PendingInputModeOnEnterUp = false;
            g_PendingSubmitOnEnterUp = false;
            ClearPendingPaste();
            g_RestoreImeAfterPaste = false;
            g_TextKeyboardLayout = nullptr;
            g_ChangingKeyboardLayout = false;
            g_WindowActive = false;
            g_GameplayLayoutActive = false;
            StopSubmissionWorker();
            g_LastResultText.clear();
            unregister_addon(hinstDLL);
            break;
        default: break;
    }
    return TRUE;
}
