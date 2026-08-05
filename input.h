#ifndef RESHADE_HD2_INGAMEIME_INPUT_H
#define RESHADE_HD2_INGAMEIME_INPUT_H

#include <cstdint>
#include <string>

constexpr std::uintptr_t kSyntheticInputExtraInfo = 0x484432494D45ull;

std::wstring GetClipboard();
void SendClipboardText(const std::wstring &text);

#endif // RESHADE_HD2_INGAMEIME_INPUT_H
