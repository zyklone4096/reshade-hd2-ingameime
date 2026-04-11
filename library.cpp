#include "library.h"
#include "input.h"

#include <windows.h>
#include <imm.h>

#include <reshade.hpp>
#include <string>
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
static std::wstring g_LastResultText;

static void SetImeEnabled(HWND hwnd, bool enabled);
static void SetInputMode(HWND hwnd, bool enabled);

static void SubmitResultText(HWND hwnd) {
    const std::wstring text = g_LastResultText;
    g_LastResultText.clear();

    if (text.empty() || GetForegroundWindow() != hwnd)
        return;

    SendAltText(text);
}

static LRESULT HandleEnterKey(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (!g_InputMode) {
        const LRESULT result = CallWindowProc(g_OriginalWndProc, hwnd, msg, wParam, lParam);
        SetInputMode(hwnd, true);
        return result;
    }

    SetImeEnabled(hwnd, false);
    SubmitResultText(hwnd);
    g_InputMode = false;
    return CallWindowProc(g_OriginalWndProc, hwnd, msg, wParam, lParam);
}

static LRESULT HandleEscapeKey(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    SetImeEnabled(hwnd, false);
    g_InputMode = false;
    g_LastResultText.clear();
    return CallWindowProc(g_OriginalWndProc, hwnd, msg, wParam, lParam);
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

    ImmSetOpenStatus(context, enabled ? TRUE : FALSE);
    ImmReleaseContext(hwnd, context);
    g_ImeOpen = enabled;
}

static void SetInputMode(HWND hwnd, bool enabled) {
    g_InputMode = enabled;
    SetImeEnabled(hwnd, enabled);
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
            if (wParam == VK_RETURN) {
                return HandleEnterKey(hwnd, msg, wParam, lParam);
            }

            if (wParam == VK_ESCAPE)
                return HandleEscapeKey(hwnd, msg, wParam, lParam);
            break;
        case WM_GETDLGCODE: {
            const auto dlg_code = static_cast<LRESULT>(CallWindowProc(g_OriginalWndProc, hwnd, msg, wParam, lParam));
            return dlg_code | DLGC_WANTALLKEYS;
        }
        case WM_IME_COMPOSITION: {
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
        case WM_SETFOCUS:
            if (g_InputMode)
                SetImeEnabled(hwnd, true);
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
            g_LastResultText.clear();
            unregister_addon(hinstDLL);
            break;
        default: break;
    }
    return TRUE;
}
