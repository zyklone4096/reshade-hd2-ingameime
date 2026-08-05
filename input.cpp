#include "input.h"

#include <windows.h>

#include <vector>

namespace {

INPUT MakeUnicodeInput(wchar_t character, DWORD flags) {
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wScan = character;
    input.ki.dwFlags = KEYEVENTF_UNICODE | flags;
    input.ki.dwExtraInfo = static_cast<ULONG_PTR>(kSyntheticInputExtraInfo);
    return input;
}

} // namespace

std::wstring GetClipboard() {
    if (!OpenClipboard(nullptr))
        return {};

    const HANDLE data = GetClipboardData(CF_UNICODETEXT);
    if (!data) {
        CloseClipboard();
        return {};
    }

    const auto text = static_cast<const wchar_t *>(GlobalLock(data));
    if (!text) {
        CloseClipboard();
        return {};
    }

    std::wstring result(text);
    GlobalUnlock(data);
    CloseClipboard();
    return result;
}

void SendClipboardText(const std::wstring &text) {
    std::vector<INPUT> inputs;
    inputs.reserve(text.size() * 2);

    for (const wchar_t character : text) {
        if (character == L'\r' || character == L'\n')
            continue;

        inputs.push_back(MakeUnicodeInput(character, 0));
        inputs.push_back(MakeUnicodeInput(character, KEYEVENTF_KEYUP));
    }

    if (!inputs.empty())
        SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
}
