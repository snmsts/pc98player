// SPDX-License-Identifier: MIT
//  hostfont_gdi.cpp -- GDI で描く（win32/main.cpp と同じ字形）
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string.h>
#include "hostfont.h"
#include "../core/machine.h"

static HDC      s_dc;
static HBITMAP  s_bmp;
static HFONT    s_font16, s_font8;   // font8: 幅 8 ドットに縦長で描く
static uint32_t* s_bits;

static std::wstring W(const std::string& utf8) {
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    std::wstring w(n ? n - 1 : 0, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &w[0], n);
    return w;
}

bool hostfont_open(const std::string& spec, std::string*) {
    s_dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = 16; bi.bmiHeader.biHeight = -16;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    s_bmp = CreateDIBSection(s_dc, &bi, DIB_RGB_COLORS, (void**)&s_bits, nullptr, 0);
    SelectObject(s_dc, s_bmp);
    std::wstring f = spec.empty() ? L"ＭＳ ゴシック" : W(spec);
    s_font16 = CreateFontW(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0, SHIFTJIS_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, f.c_str());
    s_font8 = CreateFontW(-16, 8, 0, 0, FW_NORMAL, 0, 0, 0, SHIFTJIS_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, f.c_str());
    SetTextColor(s_dc, RGB(255, 255, 255));
    SetBkColor(s_dc, RGB(0, 0, 0));
    SetBkMode(s_dc, OPAQUE);
    return true;
}

static void draw(const wchar_t* s, int len, int w, uint8_t* out, int stride_bytes, HFONT font) {
    RECT rc = {0, 0, 16, 16};
    SelectObject(s_dc, font);
    ExtTextOutW(s_dc, 0, 0, ETO_OPAQUE, &rc, s, (UINT)len, nullptr);
    GdiFlush();
    for (int y = 0; y < 16; y++)
        for (int bx = 0; bx < stride_bytes; bx++) {
            uint8_t v = 0;
            for (int k = 0; k < 8; k++) {
                int x = bx * 8 + k;
                if (x < w && (s_bits[y * 16 + x] & 0x00808080)) v |= (uint8_t)(0x80 >> k);
            }
            out[y * stride_bytes + bx] = v;
        }
}
static int jis_to_wide(uint16_t jis, wchar_t* wc) {
    int j1 = jis >> 8, j2 = jis & 0xFF;
    int s1 = ((j1 + 1) >> 1) + (j1 <= 0x5E ? 0x70 : 0xB0);
    int s2 = j2 + ((j1 & 1) ? (j2 >= 0x60 ? 0x20 : 0x1F) : 0x7E);
    char sj[3] = {(char)s1, (char)s2, 0};
    return MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, sj, 2, wc, 4);
}
static void cb_kanji(void*, uint16_t jis, uint8_t out[32]) {
    wchar_t wc[4] = {0};
    int n = jis_to_wide(jis, wc);
    if (n <= 0) { memset(out, 0, 32); return; }
    draw(wc, n, 16, out, 2, s_font16);
}
static void cb_narrow(void*, uint16_t jis, uint8_t out[16]) {
    wchar_t wc[4] = {0};
    int n = jis_to_wide(jis, wc);
    if (n <= 0) { memset(out, 0, 16); return; }
    draw(wc, n, 8, out, 1, s_font8);
}
// ANK は 0x20-0x7E と半角カナだけ。残りは擬似漢字 ROM（core/fontrom.cpp）が作る
static bool s_ank_shift = true;
static void cb_ank(void*, uint8_t c, uint8_t out[16]) {
    wchar_t wc[2] = {0};
    if (c == 0x5C) wc[0] = 0x00A5;
    else if (c >= 0x20 && c < 0x7F) wc[0] = c;
    else if (c >= 0xA1 && c <= 0xDF) { char b = (char)c; MultiByteToWideChar(932, 0, &b, 1, wc, 2); }
    if (!wc[0]) { memset(out, 0, 16); return; }
    draw(wc, 1, 8, out, 1, s_font16);
    if (s_ank_shift) hostfont_shift_ank(out);
}

void hostfont_install(bool ank_shift) {
    s_ank_shift = ank_shift;
    FontSource fs; fs.kanji = cb_kanji; fs.ank = cb_ank; fs.narrow = cb_narrow; fs.user = nullptr;
    video_set_font(fs);
}
