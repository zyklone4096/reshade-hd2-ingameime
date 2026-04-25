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

static HIMC g_CreatedContext = nullptr;
static HWND g_hWnd = nullptr;
static WNDPROC g_OriginalWndProc = nullptr;
static bool g_InputMode = false;
static bool g_ImeOpen = false;
static bool g_PendingSubmitOnEnterUp = false;
static bool g_PendingPasteOnShortcutRelease = false;
static WPARAM g_PendingPasteKey = 0;
static WPARAM g_PendingPasteModifierKey = 0;
static bool g_PendingPasteKeyReleased = false;
static bool g_PendingPasteModifierReleased = false;
static bool g_RestoreImeAfterPaste = false;
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

static void SetImeEnabled(HWND hwnd, bool enabled);
static void SetInputMode(HWND hwnd, bool enabled);

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

        if (submission.text.empty())
            continue;

        if (GetForegroundWindow() != g_hWnd)
            continue;

        SendText(submission.text, submission.useAltCode);

        if (!submission.sendEnter && GetForegroundWindow() == g_hWnd)
            PostMessage(g_hWnd, WM_APP_SUBMIT_TEXT, 1, 0);

        if (submission.sendEnter && GetForegroundWindow() == g_hWnd)
            PostMessage(g_hWnd, WM_APP_SUBMIT_TEXT, 2, 0);
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
    if (text.empty())
        return;

    EnsureSubmissionWorkerRunning();

    {
        std::lock_guard<std::mutex> lock(g_SubmissionMutex);
        TextSubmission submission = {std::move(text), sendEnter, useAltCode};
        g_PendingSubmissions.push_back(std::move(submission));
    }

    g_SubmissionCv.notify_one();

    PostMessage(hwnd, WM_APP_SUBMIT_TEXT, 0, 0);
}

static void QueueSubmitResultText(HWND hwnd) {
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
    const UINT scan_code = MapVirtualKeyW(VK_RETURN, MAPVK_VK_TO_VSC);
    const LPARAM keydown_lparam = 1 | (static_cast<LPARAM>(scan_code) << 16);
    const LPARAM keyup_lparam = keydown_lparam | (1LL << 30) | (1LL << 31);

    CallWindowProc(g_OriginalWndProc, hwnd, WM_KEYDOWN, VK_RETURN, keydown_lparam);
    CallWindowProc(g_OriginalWndProc, hwnd, WM_KEYUP, VK_RETURN, keyup_lparam);
}

static LRESULT HandleEnterKey(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (!g_InputMode) {
        const LRESULT result = CallWindowProc(g_OriginalWndProc, hwnd, msg, wParam, lParam);
        SetInputMode(hwnd, true);
        return result;
    }

    SetImeEnabled(hwnd, false);
    g_InputMode = false;
    g_PendingSubmitOnEnterUp = true;
    return 0;
}

static LRESULT HandleEscapeKey(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    SetImeEnabled(hwnd, false);
    g_InputMode = false;
    g_PendingSubmitOnEnterUp = false;
    ClearPendingPaste();
    CancelTextSubmission();
    g_LastResultText.clear();
    return CallWindowProc(g_OriginalWndProc, hwnd, msg, wParam, lParam);
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
    HIMC context = ImmGetContext(hwnd);
    if (context)
        return context;

    if (!g_CreatedContext)
        g_CreatedContext = ImmCreateContext();
    if (!g_CreatedContext)
        return nullptr;

    ImmAssociateContext(hwnd, g_CreatedContext);

    return ImmGetContext(hwnd);
}

static void SetImeEnabled(HWND hwnd, bool enabled) {
    HIMC context = enabled ? EnsureImeContext(hwnd) : ImmGetContext(hwnd);
    if (!context) {
        g_ImeOpen = false;
        return;
    }

    if (enabled) {
        DWORD conversion = 0;
        DWORD sentence = 0;
        if (ImmGetConversionStatus(context, &conversion, &sentence)) {
            conversion |= IME_CMODE_NATIVE;
            ImmSetConversionStatus(context, conversion, sentence);
        }
    }

    ImmSetOpenStatus(context, enabled ? TRUE : FALSE);
    ImmReleaseContext(hwnd, context);

    if (!enabled)
        ImmAssociateContext(hwnd, nullptr);

    g_ImeOpen = enabled;
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

    if (g_CreatedContext) {
        ImmSetOpenStatus(g_CreatedContext, FALSE);
        ImmAssociateContext(g_hWnd, nullptr);
        ImmDestroyContext(g_CreatedContext);
        g_CreatedContext = nullptr;
    }

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

    if (IsSyntheticAltCodeKeyMessage(msg, wParam))
        return 0;

    switch (msg) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if ((lParam & (1UL << 30)) != 0)
                break;

            if (wParam == VK_RETURN) {
                return HandleEnterKey(hwnd, msg, wParam, lParam);
            }

            if (wParam == VK_ESCAPE)
                return HandleEscapeKey(hwnd, msg, wParam, lParam);

            if (g_InputMode && IsPasteShortcut(wParam)) {
                BeginPendingPaste(wParam);
                return 0;
            }
            break;
        case WM_KEYUP:
        case WM_SYSKEYUP:
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

            if (wParam == VK_RETURN && g_PendingSubmitOnEnterUp) {
                g_PendingSubmitOnEnterUp = false;
                QueueSubmitResultText(hwnd);
                return 0;
            }
            break;
        case WM_APP_SUBMIT_TEXT:
            if (wParam == 2) {
                if (GetForegroundWindow() == hwnd)
                    SendSubmitEnter(hwnd);
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
        case WM_IME_COMPOSITION: {
            if (!g_InputMode) {
                HIMC context = ImmGetContext(hwnd);
                if (context) {
                    ImmNotifyIME(context, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
                    ImmReleaseContext(hwnd, context);
                }
                SetImeEnabled(hwnd, false);
                return 0;
            }

            if (GCS_RESULTSTR & lParam) {
                if (HIMC context = ImmGetContext(hwnd)) {
                    const LONG size = ImmGetCompositionStringW(context, GCS_RESULTSTR, nullptr, 0);
                    if (size > 0) {
                        std::vector<wchar_t> buffer((size / sizeof(wchar_t)) + 1, L'\0');
                        const LONG copied = ImmGetCompositionStringW(context, GCS_RESULTSTR, buffer.data(), size);
                        if (copied >= 0) {
                            g_LastResultText.assign(buffer.data(), copied / sizeof(wchar_t));
                        }
                    } else {
                        g_LastResultText.clear();
                    }

                    ImmReleaseContext(hwnd, context);
                }
            }
            break;
        }
        case WM_IME_STARTCOMPOSITION:
            if (!g_InputMode) {
                EnforceImeState(hwnd);
                return 0;
            }
            break;
        case WM_IME_SETCONTEXT:
        case WM_INPUTLANGCHANGE:
            if (!g_InputMode)
                EnforceImeState(hwnd);
            break;
        case WM_INPUTLANGCHANGEREQUEST:
            if (!g_InputMode)
                return 0;   // reject
            break;
        case WM_SETFOCUS:
            EnforceImeState(hwnd);
            break;
        case WM_KILLFOCUS:
            ClearPendingPaste();
            CancelTextSubmission();
            g_RestoreImeAfterPaste = false;
            SetImeEnabled(hwnd, false);
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

    RestoreWindowProc();
    DetachImeContext();
    CancelTextSubmission();

    g_hWnd = hwnd;
    g_OriginalWndProc = reinterpret_cast<WNDPROC>(GetWindowLongPtr(hwnd, GWLP_WNDPROC));
    if (!g_OriginalWndProc) {
        DetachImeContext();
        g_hWnd = nullptr;
        return;
    }

    SetWindowLongPtr(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ReplaceWindowFunc));
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
            g_PendingSubmitOnEnterUp = false;
            ClearPendingPaste();
            g_RestoreImeAfterPaste = false;
            StopSubmissionWorker();
            g_LastResultText.clear();
            unregister_addon(hinstDLL);
            break;
        default: break;
    }
    return TRUE;
}
