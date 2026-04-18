#include "input.h"

#include <windows.h>
#include <thread>
#include <chrono>

constexpr UINT kGbkCodePage = 936;

INPUT MakeKeyInput(WORD vk, DWORD flags = 0) {
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    input.ki.dwFlags = flags;
    return input;
}

int GetRandomDelay(int minMs, int maxMs) {
    if (maxMs <= minMs)
        return minMs;

    const auto tick = static_cast<unsigned int>(GetTickCount64());
    return minMs + static_cast<int>(tick % static_cast<unsigned int>(maxMs - minMs + 1));
}

void SleepForRange(int minMs, int maxMs) {
    std::this_thread::sleep_for(std::chrono::milliseconds(GetRandomDelay(minMs, maxMs)));
}

void SendKeyDown(WORD vk) {
    INPUT input = MakeKeyInput(vk);
    SendInput(1, &input, sizeof(INPUT));
    SleepForRange(3, 5);
}

void SendKeyUp(WORD vk) {
    INPUT input = MakeKeyInput(vk, KEYEVENTF_KEYUP);
    SendInput(1, &input, sizeof(INPUT));
    SleepForRange(3, 5);
}

void SendVirtualKey(WORD vk) {
    SendKeyDown(vk);
    SendKeyUp(vk);
}

void SendEnterKey() {
    SendVirtualKey(VK_RETURN);
}

void SendUnicodeChar(wchar_t character) {
    INPUT inputs[2] = {};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wScan = character;
    inputs[0].ki.dwFlags = KEYEVENTF_UNICODE;

    inputs[1] = inputs[0];
    inputs[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;

    SendInput(2, inputs, sizeof(INPUT));
    SleepForRange(5, 8);
}

bool TryGetGbkAltCode(wchar_t character, unsigned int &code) {
    char gbkBytes[3] = {};
    const int gbkSize = WideCharToMultiByte(kGbkCodePage, 0, &character, 1, gbkBytes, static_cast<int>(sizeof(gbkBytes)), nullptr, nullptr);
    if (gbkSize <= 0)
        return false;

    const auto lead = static_cast<unsigned char>(gbkBytes[0]);
    if (gbkSize == 1 || lead <= 0x7F) {
        code = lead;
        return true;
    }

    const auto trail = static_cast<unsigned char>(gbkBytes[1]);
    code = (static_cast<unsigned int>(lead) << 8) | trail;
    return true;
}

void SendAltCode(unsigned int code) {
    INPUT inputs[4] = {};
    int inputCount = 0;

    const bool ctrlDown = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shiftDown = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool altDown = (GetKeyState(VK_MENU) & 0x8000) != 0;
    const bool winDown = (GetKeyState(VK_LWIN) & 0x8000) != 0 || (GetKeyState(VK_RWIN) & 0x8000) != 0;

    if (ctrlDown) {
        inputs[inputCount].type = INPUT_KEYBOARD;
        inputs[inputCount].ki.wVk = VK_CONTROL;
        inputs[inputCount].ki.dwFlags = KEYEVENTF_KEYUP;
        inputCount++;
    }
    if (shiftDown) {
        inputs[inputCount].type = INPUT_KEYBOARD;
        inputs[inputCount].ki.wVk = VK_SHIFT;
        inputs[inputCount].ki.dwFlags = KEYEVENTF_KEYUP;
        inputCount++;
    }
    if (altDown) {
        inputs[inputCount].type = INPUT_KEYBOARD;
        inputs[inputCount].ki.wVk = VK_MENU;
        inputs[inputCount].ki.dwFlags = KEYEVENTF_KEYUP;
        inputCount++;
    }
    if (winDown) {
        inputs[inputCount].type = INPUT_KEYBOARD;
        inputs[inputCount].ki.wVk = VK_LWIN;
        inputs[inputCount].ki.dwFlags = KEYEVENTF_KEYUP;
        inputCount++;
    }

    if (inputCount > 0) {
        SendInput(inputCount, inputs, sizeof(INPUT));
        SleepForRange(20, 30);
    }

    const bool numLockWasOn = (GetKeyState(VK_NUMLOCK) & 0x0001) != 0;
    if (!numLockWasOn) {
        INPUT numLockInput = MakeKeyInput(VK_NUMLOCK);
        SendInput(1, &numLockInput, sizeof(INPUT));
        INPUT numLockUp = MakeKeyInput(VK_NUMLOCK, KEYEVENTF_KEYUP);
        SendInput(1, &numLockUp, sizeof(INPUT));
        SleepForRange(10, 15);
    }

    const std::string digits = std::to_string(code);
    SendKeyDown(VK_MENU);
    for (const char digit : digits) {
        const WORD numpadVk = static_cast<WORD>(VK_NUMPAD0 + (digit - '0'));
        SendVirtualKey(numpadVk);
    }
    SendKeyUp(VK_MENU);

    if (!numLockWasOn) {
        SleepForRange(5, 10);
        INPUT numLockInput = MakeKeyInput(VK_NUMLOCK);
        SendInput(1, &numLockInput, sizeof(INPUT));
        INPUT numLockUp = MakeKeyInput(VK_NUMLOCK, KEYEVENTF_KEYUP);
        SendInput(1, &numLockUp, sizeof(INPUT));
    }

    SleepForRange(8, 12);
}

void SendAltText(const std::wstring &text) {
    for (wchar_t character : text) {
        if (character == L'\r' || character == L'\n')
            continue;

        unsigned int code = 0;
        if (TryGetGbkAltCode(character, code))
            SendAltCode(code);
    }
}

std::wstring GetClipboard() {
    if (!OpenClipboard(nullptr)) return {};
    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    if (!hData) { CloseClipboard(); return {}; }
    std::wstring result(static_cast<wchar_t*>(GlobalLock(hData)));
    GlobalUnlock(hData);
    CloseClipboard();
    return result;
}

void SendText(const std::wstring &text, bool useAltCode) {
    for (wchar_t character : text) {
        if (character == L'\r' || character == L'\n')
            continue;

        if (useAltCode && character > 0x7F) {
            unsigned int code = 0;
            if (TryGetGbkAltCode(character, code))
                SendAltCode(code);
        } else {
            SendUnicodeChar(character);
        }
    }
}

void SendText(const std::wstring &text) {
    SendText(text, true);
}
