#ifndef RESHADE_HD2_INGAMEIME_INPUT_H
#define RESHADE_HD2_INGAMEIME_INPUT_H

#include <string>

void SendText(const std::wstring &text);
void SendText(const std::wstring &text, bool useAltCode);
void SendAltCode(unsigned int code);
void SendAltText(const std::wstring &text);
void SendEnterKey();

std::wstring GetClipboard();

#endif //RESHADE_HD2_INGAMEIME_INPUT_H
