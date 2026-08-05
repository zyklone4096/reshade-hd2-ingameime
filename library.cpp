#include "library.h"
#include "input.h"

#include <windows.h>

#include <reshade.hpp>

extern "C" __declspec(dllexport) const char *NAME = "Helldivers 2 Ingame IME";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Chinese clipboard paste support";

namespace {

HWND g_hWnd = nullptr;
WNDPROC g_OriginalWndProc = nullptr;
bool g_InputMode = false;
bool g_PendingInputModeOnEnterUp = false;
bool g_PendingInputModeOffOnEnterUp = false;

bool g_PendingPaste = false;
WPARAM g_PendingPasteKey = 0;
WPARAM g_PendingPasteModifierKey = 0;
bool g_PendingPasteKeyReleased = false;
bool g_PendingPasteModifierReleased = false;

HKL g_GameplayKeyboardLayout = nullptr;
HKL g_TextKeyboardLayout = nullptr;
bool g_ChangingKeyboardLayout = false;
bool g_GameplayLayoutActive = false;

HKL GetGameplayKeyboardLayout() {
    if (!g_GameplayKeyboardLayout)
        g_GameplayKeyboardLayout = LoadKeyboardLayoutW(L"00000409", KLF_NOTELLSHELL);

    return g_GameplayKeyboardLayout;
}

void ActivateKeyboardLayoutForGameplay() {
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

void RestoreTextKeyboardLayout() {
    g_GameplayLayoutActive = false;
    if (!g_TextKeyboardLayout || GetKeyboardLayout(0) == g_TextKeyboardLayout)
        return;

    g_ChangingKeyboardLayout = true;
    ActivateKeyboardLayout(g_TextKeyboardLayout, 0);
    g_ChangingKeyboardLayout = false;
}

void RestoreLayoutAfterGameplay(HWND targetHwnd) {
    RestoreTextKeyboardLayout();

    if (!g_TextKeyboardLayout)
        return;

    if (!targetHwnd)
        targetHwnd = GetForegroundWindow();

    if (targetHwnd && targetHwnd != g_hWnd) {
        PostMessageW(targetHwnd, WM_INPUTLANGCHANGEREQUEST, 0,
                     reinterpret_cast<LPARAM>(g_TextKeyboardLayout));
    }
}

void ClearPendingPaste() {
    g_PendingPaste = false;
    g_PendingPasteKey = 0;
    g_PendingPasteModifierKey = 0;
    g_PendingPasteKeyReleased = false;
    g_PendingPasteModifierReleased = false;
}

void BeginPendingPaste(WPARAM key) {
    g_PendingPaste = true;
    g_PendingPasteKey = key;
    g_PendingPasteModifierKey = key == 'V' ? VK_CONTROL : VK_SHIFT;
    g_PendingPasteKeyReleased = false;
    g_PendingPasteModifierReleased = false;
}

bool TrySendPendingPaste(HWND hwnd) {
    if (!g_PendingPaste || !g_PendingPasteKeyReleased || !g_PendingPasteModifierReleased)
        return false;

    ClearPendingPaste();
    if (GetForegroundWindow() == hwnd)
        SendClipboardText(GetClipboard());
    return true;
}

bool IsPasteShortcut(WPARAM key) {
    const bool ctrlDown = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shiftDown = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    return (key == 'V' && ctrlDown) || (key == VK_INSERT && shiftDown);
}

void EnterInputMode() {
    g_InputMode = true;
    g_PendingInputModeOffOnEnterUp = false;
    RestoreTextKeyboardLayout();
}

void LeaveInputMode() {
    g_InputMode = false;
    g_PendingInputModeOnEnterUp = false;
    g_PendingInputModeOffOnEnterUp = false;
    ClearPendingPaste();
    ActivateKeyboardLayoutForGameplay();
}

void RestoreWindowProc() {
    if (g_hWnd && g_OriginalWndProc) {
        SetWindowLongPtr(g_hWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_OriginalWndProc));
        g_OriginalWndProc = nullptr;
    }
}

} // namespace

LRESULT CALLBACK ReplaceWindowFunc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (!(g_hWnd && g_OriginalWndProc))
        return DefWindowProc(hwnd, msg, wParam, lParam);

    switch (msg) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (!g_InputMode) {
                if (wParam == VK_RETURN && (lParam & (1UL << 30)) == 0)
                    g_PendingInputModeOnEnterUp = true;
                ActivateKeyboardLayoutForGameplay();
            } else {
                if (wParam == VK_RETURN && (lParam & (1UL << 30)) == 0)
                    g_PendingInputModeOffOnEnterUp = true;

                if (IsPasteShortcut(wParam)) {
                    BeginPendingPaste(wParam);
                    return 0;
                }

                if (g_PendingPaste &&
                    (wParam == g_PendingPasteKey || wParam == g_PendingPasteModifierKey))
                    return 0;
            }
            break;

        case WM_KEYUP:
        case WM_SYSKEYUP:
            if (g_PendingPaste) {
                if (wParam == g_PendingPasteKey) {
                    g_PendingPasteKeyReleased = true;
                    TrySendPendingPaste(hwnd);
                    return 0;
                }

                if (wParam == g_PendingPasteModifierKey) {
                    g_PendingPasteModifierReleased = true;
                    if (TrySendPendingPaste(hwnd))
                        return 0;
                }
            }

            if (wParam == VK_RETURN && g_PendingInputModeOnEnterUp) {
                g_PendingInputModeOnEnterUp = false;
                const LRESULT result = CallWindowProc(g_OriginalWndProc, hwnd, msg, wParam, lParam);
                EnterInputMode();
                return result;
            }

            if (wParam == VK_RETURN && g_PendingInputModeOffOnEnterUp) {
                g_PendingInputModeOffOnEnterUp = false;
                const LRESULT result = CallWindowProc(g_OriginalWndProc, hwnd, msg, wParam, lParam);
                LeaveInputMode();
                return result;
            }

            if (wParam == VK_ESCAPE && g_InputMode) {
                const LRESULT result = CallWindowProc(g_OriginalWndProc, hwnd, msg, wParam, lParam);
                LeaveInputMode();
                return result;
            }
            break;

        case WM_INPUTLANGCHANGE:
            if (g_ChangingKeyboardLayout)
                return 0;
            if (!g_InputMode) {
                ActivateKeyboardLayoutForGameplay();
                return 0;
            }
            break;

        case WM_INPUTLANGCHANGEREQUEST:
            // Reject Win+Space and other layout changes without switching layouts
            // synchronously from inside the system's change-request message.
            if (!g_InputMode)
                return 0;
            break;

        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE) {
                g_InputMode = false;
                g_PendingInputModeOnEnterUp = false;
                g_PendingInputModeOffOnEnterUp = false;
                ClearPendingPaste();
                RestoreLayoutAfterGameplay(reinterpret_cast<HWND>(lParam));
            } else if (!g_InputMode) {
                ActivateKeyboardLayoutForGameplay();
            }
            break;

        case WM_ACTIVATEAPP:
            if (!wParam) {
                g_InputMode = false;
                g_PendingInputModeOnEnterUp = false;
                g_PendingInputModeOffOnEnterUp = false;
                ClearPendingPaste();
                RestoreLayoutAfterGameplay(GetForegroundWindow());
            } else if (!g_InputMode) {
                ActivateKeyboardLayoutForGameplay();
            }
            break;

        case WM_SETFOCUS:
            if (g_InputMode)
                RestoreTextKeyboardLayout();
            else
                ActivateKeyboardLayoutForGameplay();
            break;

        case WM_KILLFOCUS:
            g_InputMode = false;
            g_PendingInputModeOnEnterUp = false;
            g_PendingInputModeOffOnEnterUp = false;
            ClearPendingPaste();
            RestoreLayoutAfterGameplay(reinterpret_cast<HWND>(wParam));
            break;

        default:
            break;
    }

    return CallWindowProc(g_OriginalWndProc, hwnd, msg, wParam, lParam);
}

void OnReshadePresent(reshade::api::effect_runtime *runtime) {
    const auto hwnd = static_cast<HWND>(runtime->get_hwnd());
    if (!hwnd)
        return;

    if (g_hWnd == hwnd && g_OriginalWndProc)
        return;

    RestoreWindowProc();
    g_hWnd = hwnd;
    g_OriginalWndProc = reinterpret_cast<WNDPROC>(GetWindowLongPtr(hwnd, GWLP_WNDPROC));
    if (!g_OriginalWndProc) {
        g_hWnd = nullptr;
        return;
    }

    SetWindowLongPtr(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ReplaceWindowFunc));
    ActivateKeyboardLayoutForGameplay();
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    using namespace reshade;
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH:
            if (!register_addon(hinstDLL))
                return FALSE;
            register_event<addon_event::reshade_present>(&OnReshadePresent);
            break;

        case DLL_PROCESS_DETACH:
            unregister_event<addon_event::reshade_present>(&OnReshadePresent);
            RestoreWindowProc();
            RestoreLayoutAfterGameplay(GetForegroundWindow());
            g_hWnd = nullptr;
            g_InputMode = false;
            g_GameplayKeyboardLayout = nullptr;
            g_TextKeyboardLayout = nullptr;
            g_ChangingKeyboardLayout = false;
            g_GameplayLayoutActive = false;
            unregister_addon(hinstDLL);
            break;

        default:
            break;
    }
    return TRUE;
}
