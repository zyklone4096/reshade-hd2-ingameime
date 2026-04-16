#include "input.h"

#include <windows.h>
#include <thread>
#include <chrono>
#include <vector>

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
    SleepForRange(1, 2);
}

void SendKeyUp(WORD vk) {
    INPUT input = MakeKeyInput(vk, KEYEVENTF_KEYUP);
    SendInput(1, &input, sizeof(INPUT));
    SleepForRange(1, 2);
}

void SendVirtualKey(WORD vk) {
    SendKeyDown(vk);
    SendKeyUp(vk);
}

void SendUnicodeChar(wchar_t character) {
    INPUT inputs[2] = {};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wScan = character;
    inputs[0].ki.dwFlags = KEYEVENTF_UNICODE;

    inputs[1] = inputs[0];
    inputs[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;

    SendInput(2, inputs, sizeof(INPUT));
    SleepForRange(1, 2);
}

bool TryGetGbkAltCode(wchar_t character, unsigned int &code) {
    char gbkBytes[3] = {};
    const int gbkSize = WideCharToMultiByte(kGbkCodePage, 0, &character, 1, gbkBytes, static_cast<int>(sizeof(gbkBytes)), nullptr, nullptr);
    if (gbkSize <= 0)
        return false;

    const unsigned char lead = static_cast<unsigned char>(gbkBytes[0]);
    if (gbkSize == 1 || lead <= 0x7F) {
        code = lead;
        return true;
    }

    const unsigned char trail = static_cast<unsigned char>(gbkBytes[1]);
    code = (static_cast<unsigned int>(lead) << 8) | trail;
    return true;
}

void SendAltCode(unsigned int code) {
    const std::string digits = std::to_string(code);

    std::vector<INPUT> inputs;
    inputs.reserve((digits.size() * 2) + 3);

    inputs.push_back(MakeKeyInput(VK_MENU));
    for (char digit : digits) {
        const WORD numpadVk = static_cast<WORD>(VK_NUMPAD0 + (digit - '0'));
        inputs.push_back(MakeKeyInput(numpadVk));
        inputs.push_back(MakeKeyInput(numpadVk, KEYEVENTF_KEYUP));
    }
    inputs.push_back(MakeKeyInput(VK_MENU, KEYEVENTF_KEYUP));

    SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));

    INPUT altUp = MakeKeyInput(VK_MENU, KEYEVENTF_KEYUP);
    SendInput(1, &altUp, sizeof(INPUT));
    SleepForRange(2, 4);
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
    if (!OpenClipboard(nullptr))
        return L"";

    std::wstring result;
    if (const auto handle = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto text = static_cast<const wchar_t *>(GlobalLock(handle))) {
            result.assign(text);
            GlobalUnlock(handle);
        }
    }

    CloseClipboard();
    return result;
}

void SendText(const std::wstring &text) {
    for (wchar_t character : text) {
        if (character == L'\r' || character == L'\n')
            continue;

        if (character <= 0x7F)
            SendUnicodeChar(character);
        else {
            unsigned int code = 0;
            if (TryGetGbkAltCode(character, code))
                SendAltCode(code);
        }
    }
}
