// SPDX-License-Identifier: MIT
//  hostfont.h -- 漢字 ROM の代わりにホストのフォントで字を描く（FontSource を用意する）
//
//  Windows は GDI（win32/main.cpp と同じ MS ゴシック）、それ以外は FreeType。
#pragma once
#include <string>

// spec: 書体名（GDI）またはフォントファイルのパス（FreeType）。空なら既定
bool hostfont_open(const std::string& spec, std::string* err);
void hostfont_install();   // video_set_font() に渡す
