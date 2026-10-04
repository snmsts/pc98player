// SPDX-License-Identifier: MIT
//  hostfont.h -- 漢字 ROM の代わりにホストのフォントで字を描く（FontSource を用意する）
//
//  Windows は GDI（win32/main.cpp と同じ MS ゴシック）、それ以外は FreeType。
#pragma once
#include <stdint.h>
#include <string>

// spec: 書体名（GDI）またはフォントファイルのパス（FreeType）。空なら既定
bool hostfont_open(const std::string& spec, std::string* err);
void hostfont_install(bool ank_shift);   // video_set_font() に渡す。ank_shift は INI の FontShift=

// 実機の ROM の半角（8x16）は左端の 1 列を空けてある。ホストのフォントは左端から描くことがあるので、
// 右端が空いていれば 1 ドット右へ寄せる（win32/main.cpp の cb_ank と同じ。MIMPI V4 の曲名の崩れ対策）
inline void hostfont_shift_ank(uint8_t out[16]) {
    uint8_t l = 0, r = 0;
    for (int y = 0; y < 16; y++) { l |= out[y] & 0x80; r |= out[y] & 0x01; }
    if (l && !r) for (int y = 0; y < 16; y++) out[y] = (uint8_t)(out[y] >> 1);
}
