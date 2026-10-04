// SPDX-License-Identifier: MIT
//  hostfont_ft.cpp -- FreeType で描く（macOS / Linux。Windows でも PC98_SDL_FREETYPE=ON で使える）
//
//  Font= にフォントファイルのパスを書けばそれを、無ければ OS ごとの既定を順に探す。
#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "hostfont.h"
#include "../core/machine.h"
#include "../core/hostfs.h"

static FT_Library s_lib;
static FT_Face    s_face;
static int        s_base = 14;   // 16 ドットの枠の中のベースライン

static bool file_exists(const std::string& p) {
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

static std::vector<std::string> candidates(const std::string& spec) {
    std::vector<std::string> v;
    if (!spec.empty()) v.push_back(spec);
#ifdef _WIN32
    const char* wd = getenv("WINDIR");
    std::string fonts = std::string(wd ? wd : "C:\\Windows") + "\\Fonts\\";
    v.push_back(fonts + "msgothic.ttc");
#elif defined(__APPLE__)
    v.push_back("/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc");
    v.push_back("/System/Library/Fonts/Hiragino Sans W3.ttc");
    v.push_back("/Library/Fonts/Osaka.ttf");
    v.push_back("/System/Library/Fonts/Supplemental/Arial Unicode.ttf");
#else
    v.push_back("/usr/share/fonts/opentype/ipafont-gothic/ipag.ttf");
    v.push_back("/usr/share/fonts/truetype/fonts-japanese-gothic.ttf");
    v.push_back("/usr/share/fonts/ipa-gothic/ipag.ttf");
    v.push_back("/usr/share/fonts/truetype/vlgothic/VL-Gothic-Regular.ttf");
    v.push_back("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc");
    v.push_back("/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc");
    v.push_back("/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc");
#endif
    return v;
}

bool hostfont_open(const std::string& spec, std::string* err) {
    if (FT_Init_FreeType(&s_lib)) { if (err) *err = "FreeType を初期化できません"; return false; }
    // 書体名（"ＭＳ ゴシック" など）はパスではないので既定の候補にまかせる
    bool is_path = spec.find('/') != std::string::npos || spec.find('\\') != std::string::npos;
    for (auto& p : candidates(is_path ? spec : "")) {
        if (!file_exists(p)) continue;
        if (FT_New_Face(s_lib, p.c_str(), 0, &s_face) == 0) break;
        s_face = nullptr;
    }
    if (!s_face) { if (err) *err = "日本語のフォントが見つかりません。INI の Font= にフォントファイルのパスを書いてください"; return false; }
    if (s_face->units_per_EM && s_face->ascender > 0) {
        int a = s_face->ascender, d = -s_face->descender;
        s_base = (16 * a + (a + d) / 2) / (a + d);
        if (s_base < 12) s_base = 12;
        if (s_base > 15) s_base = 15;
    }
    return true;
}

// UTF-8 の先頭 1 字
static uint32_t first_cp(const std::string& s) {
    const unsigned char* p = (const unsigned char*)s.c_str();
    if (!p[0]) return 0;
    if (p[0] < 0x80) return p[0];
    if ((p[0] & 0xE0) == 0xC0) return ((p[0] & 0x1Fu) << 6) | (p[1] & 0x3Fu);
    if ((p[0] & 0xF0) == 0xE0) return ((p[0] & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
    return ((p[0] & 0x07u) << 18) | ((p[1] & 0x3Fu) << 12) | ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu);
}
static uint32_t jis_to_cp(uint16_t jis) {
    int j1 = jis >> 8, j2 = jis & 0xFF;
    int s1 = ((j1 + 1) >> 1) + (j1 <= 0x5E ? 0x70 : 0xB0);
    int s2 = j2 + ((j1 & 1) ? (j2 >= 0x60 ? 0x20 : 0x1F) : 0x7E);
    char sj[3] = {(char)s1, (char)s2, 0};
    std::string u = hostfs::from_sjis(sj);
    if (u == sj) return 0;   // 変換できなかった
    return first_cp(u);
}

// cp を幅 w の枠に描いて 1bpp で返す。xsize: 横のピクセルサイズ（8 で縦長に押し込む）
static void draw(uint32_t cp, int w, int xsize, uint8_t* out, int stride_bytes) {
    memset(out, 0, (size_t)(16 * stride_bytes));
    if (!s_face || !cp) return;
    FT_UInt gi = FT_Get_Char_Index(s_face, cp);
    if (!gi) return;   // フォントに無い字は空白（.notdef の豆腐を出さない）
    FT_Set_Pixel_Sizes(s_face, (FT_UInt)xsize, 16);
    if (FT_Load_Glyph(s_face, gi, FT_LOAD_RENDER | FT_LOAD_TARGET_MONO | FT_LOAD_MONOCHROME)) return;
    FT_GlyphSlot g = s_face->glyph;
    int adv = (int)(g->advance.x >> 6);
    int ox = g->bitmap_left + (adv > 0 && adv < w ? (w - adv) / 2 : 0);
    int oy = s_base - g->bitmap_top;
    const FT_Bitmap& b = g->bitmap;
    for (unsigned y = 0; y < b.rows; y++) {
        int py = oy + (int)y;
        if (py < 0 || py >= 16) continue;
        const unsigned char* row = b.buffer + (int)y * b.pitch;
        for (unsigned x = 0; x < b.width; x++) {
            bool on = b.pixel_mode == FT_PIXEL_MODE_MONO ? (row[x >> 3] & (0x80 >> (x & 7))) != 0 : row[x] >= 128;
            int px = ox + (int)x;
            if (!on || px < 0 || px >= w) continue;
            out[py * stride_bytes + (px >> 3)] |= (uint8_t)(0x80 >> (px & 7));
        }
    }
}

static void cb_kanji(void*, uint16_t jis, uint8_t out[32]) { draw(jis_to_cp(jis), 16, 16, out, 2); }
static void cb_narrow(void*, uint16_t jis, uint8_t out[16]) { draw(jis_to_cp(jis), 8, 8, out, 1); }
static bool s_ank_shift = true;
static void cb_ank(void*, uint8_t c, uint8_t out[16]) {
    uint32_t cp = 0;
    if (c == 0x5C) cp = 0x00A5;                          // PC-98 の 5Ch は円記号
    else if (c >= 0x20 && c < 0x7F) cp = c;
    else if (c >= 0xA1 && c <= 0xDF) cp = 0xFF61 + (c - 0xA1);   // 半角カナ
    draw(cp, 8, 16, out, 1);
    if (s_ank_shift) hostfont_shift_ank(out);
}

void hostfont_install(bool ank_shift) {
    s_ank_shift = ank_shift;
    FontSource fs; fs.kanji = cb_kanji; fs.ank = cb_ank; fs.narrow = cb_narrow; fs.user = nullptr;
    video_set_font(fs);
}
