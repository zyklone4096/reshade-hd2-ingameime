#ifndef RESHADE_HD2_INGAMEIME_INPUT_H
#define RESHADE_HD2_INGAMEIME_INPUT_H

#include <cstdint>
#include <string>

// magic number "HD2IME" for marking inputs
constexpr std::uintptr_t kSyntheticInputExtraInfo = 0x484432494D45ull;

void SendText(const std::wstring &text);
void SendText(const std::wstring &text, bool useAltCode);
void SendAltCode(unsigned int code);
void SendAltText(const std::wstring &text);
void SendEnterKey();

std::wstring GetClipboard();

#endif //RESHADE_HD2_INGAMEIME_INPUT_H
