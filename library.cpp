#include "library.h"
#include "input.h"

#include <windows.h>
#include <imm.h>

#include <reshade.hpp>
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
static std::wstring g_LastResultText;
static std::wstring g_PendingSubmitText;
static bool g_PendingSendEnter = false;
constexpr UINT WM_APP_SUBMIT_TEXT = WM_APP + 1;

static void SetImeEnabled(HWND hwnd, bool enabled);
static void SetInputMode(HWND hwnd, bool enabled);

static void QueueTextSubmission(HWND hwnd, std::wstring text, bool sendEnter) {
    g_PendingSubmitText = std::move(text);
    g_PendingSendEnter = sendEnter;

    if (g_PendingSubmitText.empty()) {
        g_PendingSendEnter = false;
        return;
    }

    PostMessage(hwnd, WM_APP_SUBMIT_TEXT, 0, 0);
}

static void QueueSubmitResultText(HWND hwnd) {
    QueueTextSubmission(hwnd, std::move(g_LastResultText), true);
    g_LastResultText.clear();
}

static void QueuePasteText(HWND hwnd) {
    QueueTextSubmission(hwnd, GetClipboard(), false);
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
    g_LastResultText.clear();
    g_PendingSubmitText.clear();
    g_PendingSendEnter = false;
    return CallWindowProc(g_OriginalWndProc, hwnd, msg, wParam, lParam);
}

static bool IsPasteShortcut(WPARAM key) {
    const bool ctrlDown = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shiftDown = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    return (key == 'V' && ctrlDown) || (key == VK_INSERT && shiftDown);
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
                QueuePasteText(hwnd);
                return 0;
            }
            break;
        case WM_KEYUP:
        case WM_SYSKEYUP:
            if (wParam == VK_RETURN && g_PendingSubmitOnEnterUp) {
                g_PendingSubmitOnEnterUp = false;
                QueueSubmitResultText(hwnd);
                return 0;
            }
            break;
        case WM_APP_SUBMIT_TEXT:
            if (!g_PendingSubmitText.empty() && GetForegroundWindow() == hwnd) {
                if (g_PendingSendEnter) {
                    SendAltText(g_PendingSubmitText);
                    SendSubmitEnter(hwnd);
                } else {
                    SendText(g_PendingSubmitText);
                }
            }
            g_PendingSubmitText.clear();
            g_PendingSendEnter = false;
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
            g_LastResultText.clear();
            g_PendingSubmitText.clear();
            g_PendingSendEnter = false;
            unregister_addon(hinstDLL);
            break;
        default: break;
    }
    return TRUE;
}
