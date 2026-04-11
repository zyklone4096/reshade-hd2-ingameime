#ifndef RESHADE_HD2_INGAMEIME_INPUT_H
#define RESHADE_HD2_INGAMEIME_INPUT_H

#include <string>

void SendText(const std::wstring &text);
void SendAltCode(unsigned int code);
void SendAltText(const std::wstring &text);

#endif //RESHADE_HD2_INGAMEIME_INPUT_H
