// SPDX-License-Identifier: MIT
// -----------------------------------------------------------------------------
//  PC98PLAYER.EXE  --  PC-98 のゲームを「Windows アプリのように」動かす窓
//
//  ゲームのフォルダにこの EXE と PC98PLAYER.INI を置いて実行するだけ。
//  ・画面      : 640x400 を整数倍（Scale=）で表示。Alt+Enter で全画面
//  ・キー      : 物理位置で PC-98 のキーへ（JIS 配列基準）
//  ・マウス    : ウインドウをクリックすると捕まえる（F12 で放す）
//  ・音        : YM2608（ymfm）＋ビープを waveOut で出す
//  ・フォント  : PC-98 の漢字 ROM の代わりに MS ゴシックをその場で描く
// -----------------------------------------------------------------------------
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <commdlg.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <deque>
#include "../core/player.h"
#include "../core/hostfs.h"
#include "../core/floppy.h"
#include "../core/fdreal.h"
#include "../core/hdimage.h"
#include <algorithm>


static Player*       g_p = nullptr;
static HWND          g_hwnd;
static BITMAPINFO    g_bmi;
static bool          g_full = false;
static WINDOWPLACEMENT g_wp = { sizeof(g_wp) };
static bool          g_captured = false;
static int           g_mouse_dx = 0, g_mouse_dy = 0, g_mouse_btn = 0;
static int           g_mouse_speed = 100;
static bool          g_turbo = false;
static std::wstring  g_title;
static bool          g_paused_by_focus = false;
static bool          g_pause_inactive = false;
static bool          g_middle_release = true;   // 中ボタン（ホイール）クリックでマウスを放す
static bool          g_mouse_lock_disable = false;   // MouseLockDisable=1: マウスを捕まえない（マウスを使わないソフト向け）

static std::wstring W(const std::string& utf8) {
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    std::wstring w(n ? n - 1 : 0, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &w[0], n);
    return w;
}
static std::string U(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n ? n - 1 : 0, '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}

// ---- フォント（GDI で描く）----------------------------------------------------
struct GdiFont {
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    HFONT font16 = nullptr, font8 = nullptr;   // font8: 幅 8 ドットに縦長で描く MS ゴシック
    uint32_t* bits = nullptr;
};
static GdiFont g_font;

static void font_init(const std::wstring& face) {
    g_font.dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = 16; bi.bmiHeader.biHeight = -16;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    g_font.bmp = CreateDIBSection(g_font.dc, &bi, DIB_RGB_COLORS, (void**)&g_font.bits, nullptr, 0);
    SelectObject(g_font.dc, g_font.bmp);
    std::wstring f = face.empty() ? L"ＭＳ ゴシック" : face;
    g_font.font16 = CreateFontW(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0, SHIFTJIS_CHARSET, OUT_DEFAULT_PRECIS,
                                CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, f.c_str());
    // 半角化用: 同じフォントを幅 8 ドットに押し込んで描く（PC-98 の「年月日」やヰヱなど）
    g_font.font8 = CreateFontW(-16, 8, 0, 0, FW_NORMAL, 0, 0, 0, SHIFTJIS_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, f.c_str());
    SetTextColor(g_font.dc, RGB(255, 255, 255));
    SetBkColor(g_font.dc, RGB(0, 0, 0));
    SetBkMode(g_font.dc, OPAQUE);
}
static void font_draw(const wchar_t* s, int len, int w, uint8_t* out, int stride_bytes, HFONT font = nullptr) {
    RECT rc = {0, 0, 16, 16};
    SelectObject(g_font.dc, font ? font : g_font.font16);
    ExtTextOutW(g_font.dc, 0, 0, ETO_OPAQUE, &rc, s, (UINT)len, nullptr);
    GdiFlush();
    for (int y = 0; y < 16; y++) {
        for (int bx = 0; bx < stride_bytes; bx++) {
            uint8_t v = 0;
            for (int k = 0; k < 8; k++) {
                int x = bx * 8 + k;
                if (x < w && (g_font.bits[y * 16 + x] & 0x00808080)) v |= (uint8_t)(0x80 >> k);
            }
            out[y * stride_bytes + bx] = v;
        }
    }
}
static int jis_to_wide(uint16_t jis, wchar_t* wc) {
    int j1 = jis >> 8, j2 = jis & 0xFF;
    int s1 = ((j1 + 1) >> 1) + (j1 <= 0x5E ? 0x70 : 0xB0);
    int s2 = j2 + ((j1 & 1) ? (j2 >= 0x60 ? 0x20 : 0x1F) : 0x7E);
    char sj[3] = {(char)s1, (char)s2, 0};
    return MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, sj, 2, wc, 4);
}
static void cb_narrow(void*, uint16_t jis, uint8_t out[16]) {
    wchar_t wc[4] = {0};
    int n = jis_to_wide(jis, wc);
    if (n <= 0) { memset(out, 0, 16); return; }
    font_draw(wc, n, 8, out, 1, g_font.font8);
}
static void cb_kanji(void*, uint16_t jis, uint8_t out[32]) {
    int j1 = jis >> 8, j2 = jis & 0xFF;
    // JIS → シフト JIS
    int s1 = ((j1 + 1) >> 1) + (j1 <= 0x5E ? 0x70 : 0xB0);
    int s2 = j2 + ((j1 & 1) ? (j2 >= 0x60 ? 0x20 : 0x1F) : 0x7E);
    char sj[3] = {(char)s1, (char)s2, 0};
    wchar_t wc[4] = {0};
    int n = MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, sj, 2, wc, 4);
    if (n <= 0) { memset(out, 0, 32); return; }
    font_draw(wc, n, 16, out, 2);
}
// ANK は 0x20-0x7E と半角カナ（0xA1-0xDF）だけをもらう。
// それ以外（罫線・ブロック・記号・年月日…）は擬似漢字 ROM（core/fontrom.cpp）が作る。
static void cb_ank(void*, uint8_t c, uint8_t out[16]) {
    wchar_t wc[2] = {0};
    if (c == 0x5C) wc[0] = 0x00A5;                       // PC-98 の 5Ch は円記号
    else if (c >= 0x20 && c < 0x7F) wc[0] = c;
    else if (c >= 0xA1 && c <= 0xDF) { char b = (char)c; MultiByteToWideChar(932, 0, &b, 1, wc, 2); }
    if (!wc[0]) { memset(out, 0, 16); return; }
    font_draw(wc, 1, 8, out, 1);
}

// ---- キー変換（スキャンコード → PC-98）-----------------------------------------
static int map_key(UINT scan, bool ext) {
    if (!ext) {
        switch (scan) {
        case 0x01: return 0x00;
        case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A: case 0x0B:
            return (int)scan - 1;
        case 0x0C: return 0x0B;  case 0x0D: return 0x0C;  case 0x7D: return 0x0D;
        case 0x0E: return 0x0E;  case 0x0F: return 0x0F;
        case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17: case 0x18: case 0x19:
            return (int)scan;
        case 0x1A: return 0x1A;  case 0x1B: return 0x1B;  case 0x1C: return 0x1C;
        case 0x1D: return 0x74;
        case 0x1E: case 0x1F: case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26:
            return (int)scan - 1;
        case 0x27: return 0x26;  case 0x28: return 0x27;  case 0x2B: return 0x28;
        case 0x2A: case 0x36: return 0x70;
        case 0x2C: case 0x2D: case 0x2E: case 0x2F: case 0x30: case 0x31: case 0x32:
            return (int)scan - 3;
        case 0x33: return 0x30;  case 0x34: return 0x31;  case 0x35: return 0x32;  case 0x73: return 0x33;
        case 0x37: return 0x45;
        case 0x38: return 0x73;
        case 0x39: return 0x34;
        case 0x3A: return 0x71;
        case 0x3B: case 0x3C: case 0x3D: case 0x3E: case 0x3F: case 0x40: case 0x41: case 0x42: case 0x43: case 0x44:
            return 0x62 + (int)scan - 0x3B;
        case 0x47: return 0x42; case 0x48: return 0x43; case 0x49: return 0x44; case 0x4A: return 0x40;
        case 0x4B: return 0x46; case 0x4C: return 0x47; case 0x4D: return 0x48; case 0x4E: return 0x49;
        case 0x4F: return 0x4A; case 0x50: return 0x4B; case 0x51: return 0x4C; case 0x52: return 0x4E; case 0x53: return 0x50;
        case 0x70: return 0x72;   // カタカナ/ひらがな → カナ
        case 0x79: return 0x35;   // 変換 → XFER
        case 0x7B: return 0x51;   // 無変換 → NFER
        case 0x46: return 0x60;   // ScrollLock → STOP
        }
        return -1;
    }
    switch (scan) {
    case 0x1C: return 0x1C;
    case 0x1D: return 0x74;
    case 0x35: return 0x41;
    case 0x38: return 0x73;
    case 0x47: return 0x3E;
    case 0x48: return 0x3A;
    case 0x49: return 0x37;   // PageUp   → ROLL DOWN
    case 0x4B: return 0x3B;
    case 0x4D: return 0x3C;
    case 0x4F: return 0x3F;   // End      → HELP
    case 0x50: return 0x3D;
    case 0x51: return 0x36;   // PageDown → ROLL UP
    case 0x52: return 0x38;
    case 0x53: return 0x39;
    case 0x37: return 0x61;   // PrintScreen → COPY
    }
    return -1;
}

// ---- 音（waveOut）-------------------------------------------------------------
struct AudioOut {
    HWAVEOUT h = nullptr;
    std::deque<WAVEHDR*> queue;
    int rate = 44100;
    bool ok = false;
    void open(int r) {
        rate = r;
        WAVEFORMATEX wf = {};
        wf.wFormatTag = WAVE_FORMAT_PCM; wf.nChannels = 2; wf.nSamplesPerSec = (DWORD)r;
        wf.wBitsPerSample = 16; wf.nBlockAlign = 4; wf.nAvgBytesPerSec = (DWORD)r * 4;
        ok = waveOutOpen(&h, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR;
    }
    void reap() {
        while (!queue.empty() && (queue.front()->dwFlags & WHDR_DONE)) {
            WAVEHDR* w = queue.front(); queue.pop_front();
            waveOutUnprepareHeader(h, w, sizeof(*w));
            delete[] w->lpData; delete w;
        }
    }
    int queued_frames() { reap(); return (int)queue.size(); }
    void push(const int16_t* s, size_t n_int16) {
        if (!ok || !n_int16) return;
        reap();
        WAVEHDR* w = new WAVEHDR();
        memset(w, 0, sizeof(*w));
        w->dwBufferLength = (DWORD)(n_int16 * 2);
        w->lpData = new char[w->dwBufferLength];
        memcpy(w->lpData, s, w->dwBufferLength);
        waveOutPrepareHeader(h, w, sizeof(*w));
        waveOutWrite(h, w, sizeof(*w));
        queue.push_back(w);
    }
    void close() {
        if (!h) return;
        waveOutReset(h);
        reap();
        waveOutClose(h);
        h = nullptr;
    }
};
static AudioOut g_audio;
static void midi_all_off();
static void audio_flush() { if (g_audio.h) { waveOutReset(g_audio.h); g_audio.reap(); } midi_all_off(); }

// ---- MIDI（MPU-PC98II の出口）--------------------------------------------------
static HMIDIOUT g_midi = nullptr;
static void midi_sink(void*, const uint8_t* msg, int len) {
    if (!g_midi || len <= 0) return;
    if (msg[0] == 0xF0) {
        std::vector<char> buf(msg, msg + len);
        MIDIHDR hd = {};
        hd.lpData = buf.data(); hd.dwBufferLength = (DWORD)len;
        if (midiOutPrepareHeader(g_midi, &hd, sizeof(hd)) != MMSYSERR_NOERROR) return;
        if (midiOutLongMsg(g_midi, &hd, sizeof(hd)) == MMSYSERR_NOERROR) {
            for (int i = 0; i < 200 && !(hd.dwFlags & MHDR_DONE); i++) Sleep(1);
        }
        midiOutUnprepareHeader(g_midi, &hd, sizeof(hd));
        return;
    }
    DWORD w = msg[0] | (len > 1 ? (msg[1] << 8) : 0) | (len > 2 ? (msg[2] << 16) : 0);
    midiOutShortMsg(g_midi, w);
}
static void midi_all_off() {
    if (!g_midi) return;
    for (int ch = 0; ch < 16; ch++) { midiOutShortMsg(g_midi, 0x7BB0 | ch); midiOutShortMsg(g_midi, 0x78B0 | ch); }
}

// ---- マウスの捕獲 --------------------------------------------------------------
static void update_title();
static void set_capture(bool on) {
    if (on && g_mouse_lock_disable) return;   // MouseLockDisable=1: クリックしても F12 でも捕まえない
    if (on == g_captured) return;
    g_captured = on;
    if (on) {
        RECT rc; GetClientRect(g_hwnd, &rc);
        POINT a = {rc.left, rc.top}, b = {rc.right, rc.bottom};
        ClientToScreen(g_hwnd, &a); ClientToScreen(g_hwnd, &b);
        RECT clip = {a.x, a.y, b.x, b.y};
        ClipCursor(&clip);
        while (ShowCursor(FALSE) >= 0) {}
        SetCapture(g_hwnd);
    } else {
        ClipCursor(nullptr);
        while (ShowCursor(TRUE) < 0) {}
        ReleaseCapture();
        g_mouse_btn = 0;
        if (g_p) machine_mouse(g_p->m, 0, 0, 0);
    }
    update_title();
}

static void update_title() {
    std::wstring t = g_title;
    t += L"  [F11: ロード / Shift+F11: セーブ]";
    if (g_mouse_lock_disable) {}
    else if (g_captured) t += L"  [マウス使用中: F12 で解放]";
    else t += L"  [クリックでマウスを使う]";
    if (g_turbo) t += L"  [早送り]";
    SetWindowTextW(g_hwnd, t.c_str());
}

// ---- 窓 ------------------------------------------------------------------------
static void window_size_for_scale(int scale, int* w, int* h) {
    RECT rc = {0, 0, 640 * scale, 400 * scale};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    *w = rc.right - rc.left; *h = rc.bottom - rc.top;
}
static void toggle_fullscreen() {
    DWORD style = (DWORD)GetWindowLongW(g_hwnd, GWL_STYLE);
    if (!g_full) {
        MONITORINFO mi = { sizeof(mi) };
        if (GetWindowPlacement(g_hwnd, &g_wp) && GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            SetWindowLongW(g_hwnd, GWL_STYLE, (LONG)(style & ~WS_OVERLAPPEDWINDOW));
            SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            g_full = true;
        }
    } else {
        SetWindowLongW(g_hwnd, GWL_STYLE, (LONG)(style | WS_OVERLAPPEDWINDOW));
        SetWindowPlacement(g_hwnd, &g_wp);
        SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_full = false;
    }
    if (g_captured) { set_capture(false); set_capture(true); }
}

// ---- ステートセーブ／ロードのスロット選択画面 --------------------------------
//  Shift+F11 = 保存、F11 = 読み込み。開いている間はエミュレーションを止め、
//  ゲーム画面を暗くした上にスロットを 4x2 で並べる。
enum { MENU_NONE = 0, MENU_SAVE = 1, MENU_LOAD = 2 };
static int      g_menu = MENU_NONE;
static int      g_sel = 0;
static SlotInfo g_slots[STATE_SLOTS];
static std::wstring g_menu_note;          // メニュー内の一言（空きスロットを選んだ等）
static std::wstring g_toast;              // 閉じた後にしばらく出す結果
static DWORD    g_toast_until = 0;
static const int MENU_COLS = 4, CELL_W = 150, CELL_H = 104, GRID_X = 20, GRID_Y = 36;
// フロッピーの欄（スロットの下の 1/3）: 640x400 の座標
static const int FD_BOX_Y = 250, FD_BOX_H = 98;
static const int FD_Y = 320, FD_H = 22;                       // ボタンの段
static const int FD_INS_X = 30, FD_INS_W = 156, FD_EJ_X = 194, FD_EJ_W = 110;
static const int FD_PREV_X = 382, FD_PREV_W = 110, FD_NEXT_X = 500, FD_NEXT_W = 110;
enum { HIT_FD_INSERT = -2, HIT_FD_EJECT = -3, HIT_FD_NEXT = -4, HIT_FD_PREV = -5 };
static std::vector<std::wstring> g_disk_list;   // まとめて渡されたフロッピーイメージ（「次のディスク」で順に入れ替える）
static int g_disk_index = 0;
static void fd_insert_listed(int k);

static void menu_open(int kind) {
    if (!g_p) return;
    set_capture(false);
    for (int k = 0; k < 128; k++) if (g_p->m->kb_down[k]) machine_key(g_p->m, (uint8_t)k, false);
    for (int i = 0; i < STATE_SLOTS; i++) state_slot_info(g_p->ps.cfg.root, i, &g_slots[i]);
    g_menu = kind;
    g_menu_note.clear();
    g_toast_until = 0;
    if (kind == MENU_LOAD && !g_slots[g_sel].used) {   // 読み込みは中身のあるスロットへ寄せる
        for (int i = 0; i < STATE_SLOTS; i++) if (g_slots[i].used) { g_sel = i; break; }
    }
    InvalidateRect(g_hwnd, nullptr, FALSE);
}
static void menu_close() {
    g_menu = MENU_NONE;
    InvalidateRect(g_hwnd, nullptr, FALSE);
}
static void show_toast(const std::wstring& t) { g_toast = t; g_toast_until = GetTickCount() + 2500; }

// ---- 仮想フロッピー --------------------------------------------------------------
static std::wstring fd_name() {
    FloppyImage* im = floppy::image();
    if (!im) return L"（入っていません）";
    std::wstring p = W(im->path);
    size_t k = p.find_last_of(L"\\/");
    return k == std::wstring::npos ? p : p.substr(k + 1);
}
static std::wstring fd_info() {
    FloppyImage* im = floppy::image();
    if (!im) return L"";
    static const wchar_t* md[] = {L"2D", L"2DD", L"2HD", L"1.44MB"};
    return W(im->format) + L"・" + (im->media >= 0 && im->media <= 3 ? md[im->media] : L"?") + (im->wprot ? L"・書き込み禁止" : L"");
}
static void fd_insert_dialog() {
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW of = {};
    of.lStructSize = sizeof(of);
    of.hwndOwner = g_hwnd;
    of.lpstrFilter = L"フロッピーイメージ (*.d88;*.d68;*.88d;*.d98;*.fdi;*.nfd;*.hdm;*.xdf;*.dup;*.tfd;*.2hd;*.img;*.scp;*.hfe)\0"
                     L"*.d88;*.d68;*.88d;*.d98;*.fdi;*.nfd;*.hdm;*.xdf;*.dup;*.tfd;*.2hd;*.img;*.scp;*.hfe\0すべてのファイル (*.*)\0*.*\0";
    of.lpstrFile = file; of.nMaxFile = MAX_PATH;
    std::wstring dir = W(g_p->ps.cfg.root);
    of.lpstrInitialDir = dir.c_str();
    of.lpstrTitle = L"フロッピーイメージを選ぶ";
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&of)) return;
    std::string err;
    if (floppy::insert(U(file), &err)) {
        wchar_t buf[160]; swprintf(buf, 160, L"%c: にフロッピーを入れました", (wchar_t)floppy::drive_letter());
        show_toast(buf);
        menu_close();
    } else g_menu_note = L"入れられませんでした: " + W(err);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}
// 実機のドライブを入れる（読み込みに時間がかかるので砂時計を出す）
static void fd_insert_device(const std::string& spec) {
    HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    std::string err;
    bool ok = floppy::insert(spec, &err);
    SetCursor(old);
    if (ok) {
        wchar_t buf[200]; swprintf(buf, 200, L"%c: に実機のドライブをつなぎました（%ls）", (wchar_t)floppy::drive_letter(), W(spec).c_str());
        show_toast(buf);
        menu_close();
    } else g_menu_note = W(err);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}
static void fd_save_d88() {
    FloppyImage* im = floppy::image();
    if (!im) { g_menu_note = L"フロッピーは入っていません"; InvalidateRect(g_hwnd, nullptr, FALSE); return; }
    wchar_t file[MAX_PATH] = L"DISK.D88";
    OPENFILENAMEW of = {};
    of.lStructSize = sizeof(of);
    of.hwndOwner = g_hwnd;
    of.lpstrFilter = L"D88 イメージ (*.d88)\0*.d88\0";
    of.lpstrFile = file; of.nMaxFile = MAX_PATH;
    std::wstring dir = W(g_p->ps.cfg.root);
    of.lpstrInitialDir = dir.c_str();
    of.lpstrDefExt = L"d88";
    of.lpstrTitle = L"今のディスクを D88 で保存";
    of.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&of)) return;
    HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    std::string err;
    bool ok = im->save_d88(U(file), &err);
    SetCursor(old);
    if (ok) { show_toast(L"D88 で保存しました"); menu_close(); }
    else g_menu_note = L"保存できませんでした: " + W(err);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}
// 「入れる」のメニュー: イメージファイル / 実機のドライブ / 読み直し / D88 で保存
static void fd_menu(bool at_mouse) {
    HMENU mnu = CreatePopupMenu();
    AppendMenuW(mnu, MF_STRING, 1, L"イメージファイルを選ぶ…");
    for (size_t i = 0; i < g_disk_list.size() && i < 40; i++) {
        std::wstring n = g_disk_list[i].substr(g_disk_list[i].find_last_of(L"\\/") + 1);
        AppendMenuW(mnu, MF_STRING | ((int)i == g_disk_index && floppy::image() ? MF_CHECKED : 0), 200 + (UINT)i, (std::to_wstring(i + 1) + L" 枚目: " + n).c_str());
    }
    std::vector<fdreal::Device> devs = fdreal::list_devices();
    AppendMenuW(mnu, MF_SEPARATOR, 0, nullptr);
    bool has_gw = false;
    for (size_t i = 0; i < devs.size() && i < 40; i++) {
        AppendMenuW(mnu, MF_STRING, 100 + (UINT)i, W(devs[i].label).c_str());
        if (devs[i].spec.compare(0, 3, "GW:") == 0) has_gw = true;
    }
    if (!has_gw) AppendMenuW(mnu, MF_STRING, 2, L"Greaseweazle を探してつなぐ");
    if (devs.empty()) AppendMenuW(mnu, MF_STRING | MF_GRAYED, 0, L"（USB フロッピードライブは見つかりません）");
    AppendMenuW(mnu, MF_SEPARATOR, 0, nullptr);
    FloppyImage* im = floppy::image();
    AppendMenuW(mnu, MF_STRING | ((im && im->src) ? 0 : MF_GRAYED), 3, L"読み直す（実機のディスクを入れ替えた）");
    AppendMenuW(mnu, MF_STRING | (im ? 0 : MF_GRAYED), 4, L"今のディスクを D88 で保存…");
    POINT pt;
    if (at_mouse) GetCursorPos(&pt);
    else { RECT rc; GetClientRect(g_hwnd, &rc); pt.x = rc.right / 2; pt.y = rc.bottom / 2; ClientToScreen(g_hwnd, &pt); }
    int id = (int)TrackPopupMenu(mnu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, nullptr);
    DestroyMenu(mnu);
    if (id == 1) fd_insert_dialog();
    else if (id == 2) fd_insert_device("GW");
    else if (id == 3 && im && im->src) {
        HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
        floppy::media_changed();   // 入れ替えとして扱う（DOS のキャッシュも捨てる）
        SetCursor(old);
        show_toast(L"ディスクを読み直しました"); menu_close();
    }
    else if (id == 4) fd_save_d88();
    else if (id >= 100 && id < 100 + (int)devs.size()) fd_insert_device(devs[id - 100].spec);
    else if (id >= 200 && id < 200 + (int)g_disk_list.size()) fd_insert_listed(id - 200);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}
// まとめて渡されたイメージの k 枚目を入れる
static void fd_insert_listed(int k) {
    if (k < 0 || k >= (int)g_disk_list.size()) return;
    std::string err;
    if (floppy::insert(U(g_disk_list[k]), &err)) {
        g_disk_index = k;
        std::wstring n = g_disk_list[k].substr(g_disk_list[k].find_last_of(L"\\/") + 1);
        wchar_t buf[200]; swprintf(buf, 200, L"%c: ← %ls（%d/%d）", (wchar_t)floppy::drive_letter(), n.c_str(), k + 1, (int)g_disk_list.size());
        show_toast(buf);
        menu_close();
    } else g_menu_note = L"入れられませんでした: " + W(err);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}
static void fd_next_disk() { if (g_disk_list.size() > 1) fd_insert_listed((g_disk_index + 1) % (int)g_disk_list.size()); }
static void fd_prev_disk() { if (g_disk_list.size() > 1) fd_insert_listed((g_disk_index + (int)g_disk_list.size() - 1) % (int)g_disk_list.size()); }
static void fd_eject() {
    if (!floppy::image()) { g_menu_note = L"フロッピーは入っていません"; InvalidateRect(g_hwnd, nullptr, FALSE); return; }
    floppy::eject();
    wchar_t buf[80]; swprintf(buf, 80, L"%c: のフロッピーを取り出しました", (wchar_t)floppy::drive_letter());
    show_toast(buf);
    menu_close();
}

static void audio_flush();
static void menu_decide() {
    std::string err;
    wchar_t buf[128];
    if (g_menu == MENU_SAVE) {
        if (g_p->save_state(g_sel, &err)) { swprintf(buf, 128, L"スロット %d に保存しました", g_sel + 1); show_toast(buf); menu_close(); }
        else g_menu_note = L"保存できませんでした: " + W(err);
    } else if (g_menu == MENU_LOAD) {
        if (!g_slots[g_sel].used) { g_menu_note = L"このスロットは空です"; InvalidateRect(g_hwnd, nullptr, FALSE); return; }
        if (g_p->load_state(g_sel, &err)) {
            audio_flush();
            swprintf(buf, 128, L"スロット %d から再開しました", g_sel + 1); show_toast(buf); menu_close();
        } else g_menu_note = L"読み込めませんでした: " + W(err);
    }
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// 画面上の表示領域（640x400 を縦横比を保って置いた矩形）
static void view_rect(int cw, int ch, int* dx, int* dy, int* dw, int* dh) {
    *dw = cw; *dh = cw * 400 / 640;
    if (*dh > ch) { *dh = ch; *dw = ch * 640 / 400; }
    *dx = (cw - *dw) / 2; *dy = (ch - *dh) / 2;
}
// クライアント座標 → スロット番号（無ければ -1）
static int menu_hit(int x, int y) {
    RECT rc; GetClientRect(g_hwnd, &rc);
    int dx, dy, dw, dh; view_rect(rc.right, rc.bottom, &dx, &dy, &dw, &dh);
    if (dw <= 0) return -1;
    double lx = (x - dx) * 640.0 / dw, ly = (y - dy) * 400.0 / dh;
    for (int i = 0; i < STATE_SLOTS; i++) {
        int cx = GRID_X + (i % MENU_COLS) * CELL_W, cy = GRID_Y + (i / MENU_COLS) * CELL_H;
        if (lx >= cx && lx < cx + CELL_W - 6 && ly >= cy && ly < cy + CELL_H - 8) return i;
    }
    if (ly >= FD_Y && ly < FD_Y + FD_H) {
        if (lx >= FD_INS_X && lx < FD_INS_X + FD_INS_W) return HIT_FD_INSERT;
        if (lx >= FD_EJ_X && lx < FD_EJ_X + FD_EJ_W) return HIT_FD_EJECT;
        if (g_disk_list.size() > 1 && lx >= FD_NEXT_X && lx < FD_NEXT_X + FD_NEXT_W) return HIT_FD_NEXT;
        if (g_disk_list.size() > 1 && lx >= FD_PREV_X && lx < FD_PREV_X + FD_PREV_W) return HIT_FD_PREV;
    }
    return -1;
}

static void draw_text(HDC dc, int x, int y, int size, COLORREF col, const std::wstring& t, bool center, int width) {
    HFONT f = CreateFontW(-size, 0, 0, 0, FW_BOLD, 0, 0, 0, SHIFTJIS_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          ANTIALIASED_QUALITY, FF_MODERN, L"ＭＳ ゴシック");
    HGDIOBJ old = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, col);
    if (center) { RECT r = {x, y, x + width, y + size * 2}; DrawTextW(dc, t.c_str(), (int)t.size(), &r, DT_CENTER | DT_TOP | DT_SINGLELINE | DT_NOPREFIX); }
    else TextOutW(dc, x, y, t.c_str(), (int)t.size());
    SelectObject(dc, old);
    DeleteObject(f);
}

static void draw_menu(HDC dc, int dx, int dy, int dw, int dh) {
    double k = dw / 640.0;
    auto X = [&](double v) { return dx + (int)(v * k); };
    auto Y = [&](double v) { return dy + (int)(v * k); };
    auto S = [&](double v) { return (int)(v * k + 0.5); };
    (void)dh;
    bool save = g_menu == MENU_SAVE;
    draw_text(dc, X(0), Y(8), S(18), save ? RGB(255, 210, 120) : RGB(140, 210, 255),
              save ? L"ステートセーブ ― 保存するスロットを選んでください" : L"ステートロード ― 読み込むスロットを選んでください",
              true, S(640));
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = STATE_THUMB_W; bi.bmiHeader.biHeight = -STATE_THUMB_H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    std::vector<uint32_t> px(STATE_THUMB_W * STATE_THUMB_H);
    for (int i = 0; i < STATE_SLOTS; i++) {
        int cx = GRID_X + (i % MENU_COLS) * CELL_W, cy = GRID_Y + (i / MENU_COLS) * CELL_H;
        bool sel = i == g_sel;
        // 枠
        HBRUSH frame = CreateSolidBrush(sel ? (save ? RGB(255, 200, 60) : RGB(80, 190, 255)) : RGB(90, 90, 110));
        RECT fr = {X(cx), Y(cy), X(cx + CELL_W - 6), Y(cy + CELL_H - 8)};
        FillRect(dc, &fr, frame);
        DeleteObject(frame);
        HBRUSH bg = CreateSolidBrush(sel ? RGB(40, 40, 60) : RGB(20, 20, 28));
        int bw = sel ? 3 : 1;
        RECT in = {fr.left + S(bw), fr.top + S(bw), fr.right - S(bw), fr.bottom - S(bw)};
        FillRect(dc, &in, bg);
        DeleteObject(bg);
        // 縮小画像
        int tw = 96, th = 60, tx = cx + (CELL_W - 6 - tw) / 2, ty = cy + 6;
        const SlotInfo& si = g_slots[i];
        if (si.used && si.thumb.size() == px.size() * 3) {
            for (size_t j = 0; j < px.size(); j++)
                px[j] = ((uint32_t)si.thumb[j * 3] << 16) | ((uint32_t)si.thumb[j * 3 + 1] << 8) | si.thumb[j * 3 + 2];
            SetStretchBltMode(dc, HALFTONE); SetBrushOrgEx(dc, 0, 0, nullptr);
            StretchDIBits(dc, X(tx), Y(ty), S(tw), S(th), 0, 0, STATE_THUMB_W, STATE_THUMB_H, px.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
        } else {
            HBRUSH e = CreateSolidBrush(RGB(34, 34, 44));
            RECT er = {X(tx), Y(ty), X(tx) + S(tw), Y(ty) + S(th)};
            FillRect(dc, &er, e); DeleteObject(e);
            draw_text(dc, X(tx), Y(ty + 23), S(13), RGB(110, 110, 130), L"― 空き ―", true, S(tw));
        }
        wchar_t lab[32]; swprintf(lab, 32, L"スロット %d", i + 1);
        draw_text(dc, X(cx + 8), Y(cy + 69), S(13), sel ? RGB(255, 255, 255) : RGB(200, 200, 210), lab, false, 0);
        draw_text(dc, X(cx + 8), Y(cy + 84), S(11), sel ? RGB(230, 230, 230) : RGB(150, 150, 165),
                  si.used ? W(si.when) : L"", false, 0);
    }
    // フロッピーの欄（画面の下 1/3）
    {
        HBRUSH pb = CreateSolidBrush(RGB(70, 80, 110));
        RECT pr = {X(GRID_X), Y(FD_BOX_Y), X(640 - GRID_X), Y(FD_BOX_Y + FD_BOX_H)};
        FillRect(dc, &pr, pb); DeleteObject(pb);
        HBRUSH ib = CreateSolidBrush(RGB(24, 28, 42));
        RECT ir = {pr.left + S(1), pr.top + S(1), pr.right - S(1), pr.bottom - S(1)};
        FillRect(dc, &ir, ib); DeleteObject(ib);
        wchar_t lab[40]; swprintf(lab, 40, L"フロッピー（%c:）", (wchar_t)(floppy::drive_letter() ? floppy::drive_letter() : L'-'));
        draw_text(dc, X(GRID_X + 10), Y(FD_BOX_Y + 8), S(14), RGB(255, 230, 150), lab, false, 0);
        if (g_disk_list.size() > 1) {
            wchar_t cnt[48]; swprintf(cnt, 48, L"%d / %d 枚目", g_disk_index + 1, (int)g_disk_list.size());
            draw_text(dc, X(420), Y(FD_BOX_Y + 8), S(14), RGB(255, 230, 150), cnt, true, S(190));
        }
        // 名前（長ければ後ろを省く）と形式
        std::wstring nm = fd_name();
        double wsum = 0; size_t cut = nm.size();
        for (size_t i = 0; i < nm.size(); i++) { wsum += nm[i] < 0x100 ? 7.5 : 15; if (wsum > 560) { cut = i; break; } }
        if (cut < nm.size()) nm = nm.substr(0, cut) + L"…";
        draw_text(dc, X(GRID_X + 10), Y(FD_BOX_Y + 28), S(15), floppy::image() ? RGB(240, 240, 250) : RGB(130, 130, 145), nm, false, 0);
        draw_text(dc, X(GRID_X + 10), Y(FD_BOX_Y + 48), S(12), RGB(160, 165, 185), fd_info(), false, 0);
        auto button = [&](int bx, int bw, const wchar_t* t) {
            HBRUSH b = CreateSolidBrush(RGB(60, 70, 100));
            RECT r = {X(bx), Y(FD_Y), X(bx + bw), Y(FD_Y + FD_H)};
            FillRect(dc, &r, b); DeleteObject(b);
            draw_text(dc, X(bx), Y(FD_Y + 4), S(13), RGB(255, 255, 255), t, true, S(bw));
        };
        button(FD_INS_X, FD_INS_W, L"F: 入れる／実機…");
        button(FD_EJ_X, FD_EJ_W, L"E: 取り出す");
        if (g_disk_list.size() > 1) {
            button(FD_PREV_X, FD_PREV_W, L"P: 前のディスク");
            button(FD_NEXT_X, FD_NEXT_W, L"N: 次のディスク");
        }
    }
    if (!g_menu_note.empty())
        draw_text(dc, X(0), Y(356), S(15), RGB(255, 120, 120), g_menu_note, true, S(640));
    draw_text(dc, X(0), Y(380), S(12), RGB(170, 170, 185),
              L"カーソルキー / マウス: 選ぶ　　Enter / 左クリック: 決定　　Esc / 右クリック: やめる", true, S(640));
}

// フロッピーからのインストールの画面（install_ui.inc）
static bool iu_active();
static bool iu_message(HWND h, UINT msg, WPARAM wp, LPARAM lp);
static void draw_install(HDC dc, int dx, int dy, int dw, int dh);

static void present(HDC dc) {
    RECT rc; GetClientRect(g_hwnd, &rc);
    int cw = rc.right, ch = rc.bottom;
    if (cw > 0 && ch > 0 && iu_active()) {   // フロッピーからのインストールの画面
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bm = CreateCompatibleBitmap(dc, cw, ch);
        HGDIOBJ ob = SelectObject(mem, bm);
        RECT all = {0, 0, cw, ch};
        FillRect(mem, &all, (HBRUSH)GetStockObject(BLACK_BRUSH));
        int dx, dy, dw, dh;
        view_rect(cw, ch, &dx, &dy, &dw, &dh);
        HBRUSH bg = CreateSolidBrush(RGB(30, 34, 48));
        RECT vr = {dx, dy, dx + dw, dy + dh};
        FillRect(mem, &vr, bg); DeleteObject(bg);
        draw_install(mem, dx, dy, dw, dh);
        BitBlt(dc, 0, 0, cw, ch, mem, 0, 0, SRCCOPY);
        SelectObject(mem, ob); DeleteObject(bm); DeleteDC(mem);
        return;
    }
    if (cw <= 0 || ch <= 0 || !g_p) return;
    int dx, dy, dw, dh;
    view_rect(cw, ch, &dx, &dy, &dw, &dh);
    bool overlay = g_menu != MENU_NONE || GetTickCount() < g_toast_until;
    HDC mem = dc;
    HBITMAP bm = nullptr; HGDIOBJ oldbm = nullptr;
    if (overlay) {   // ちらつかないよう裏で組み立てる
        mem = CreateCompatibleDC(dc);
        bm = CreateCompatibleBitmap(dc, cw, ch);
        oldbm = SelectObject(mem, bm);
    }
    HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
    if (dy > 0) { RECT r1 = {0, 0, cw, dy}; FillRect(mem, &r1, black); RECT r2 = {0, dy + dh, cw, ch}; FillRect(mem, &r2, black); }
    if (dx > 0) { RECT r1 = {0, 0, dx, ch}; FillRect(mem, &r1, black); RECT r2 = {dx + dw, 0, cw, ch}; FillRect(mem, &r2, black); }
    SetStretchBltMode(mem, g_p->ps.smooth ? HALFTONE : COLORONCOLOR);
    if (g_p->ps.smooth) SetBrushOrgEx(mem, 0, 0, nullptr);
    if (g_menu != MENU_NONE) {
        // ゲーム画面を暗くする
        static std::vector<uint32_t> dim(640 * 400);
        for (int i = 0; i < 640 * 400; i++) {
            uint32_t c = g_p->fb[i];
            dim[i] = (((c >> 16) & 255) * 30 / 100 << 16) | (((c >> 8) & 255) * 30 / 100 << 8) | ((c & 255) * 30 / 100);
        }
        StretchDIBits(mem, dx, dy, dw, dh, 0, 0, 640, 400, dim.data(), &g_bmi, DIB_RGB_COLORS, SRCCOPY);
        draw_menu(mem, dx, dy, dw, dh);
    } else {
        StretchDIBits(mem, dx, dy, dw, dh, 0, 0, 640, 400, g_p->fb, &g_bmi, DIB_RGB_COLORS, SRCCOPY);
        if (GetTickCount() < g_toast_until) {
            double k = dw / 640.0;
            double est = 24;   // 文字の幅の見積もり（全角 15、半角 7.5）で箱の幅を決める
            for (wchar_t c : g_toast) est += c < 0x100 ? 7.5 : 15;
            int tw = (int)(std::min(624.0, std::max(260.0, est)) * k), th = (int)(26 * k);
            RECT r = {dx + (int)(8 * k), dy + (int)(8 * k), dx + (int)(8 * k) + tw, dy + (int)(8 * k) + th};
            HBRUSH b = CreateSolidBrush(RGB(20, 20, 30)); FillRect(mem, &r, b); DeleteObject(b);
            draw_text(mem, r.left, r.top + (int)(5 * k), (int)(15 * k), RGB(255, 255, 255), g_toast, true, tw);
        }
    }
    if (overlay) {
        BitBlt(dc, 0, 0, cw, ch, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldbm); DeleteObject(bm); DeleteDC(mem);
    }
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (iu_message(h, msg, wp, lp)) return 0;
    switch (msg) {
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); present(dc); EndPaint(h, &ps); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) {
            set_capture(false);
            // 離した瞬間に押しっぱなしのキーが残らないよう全部離す
            if (g_p) for (int k = 0; k < 128; k++) if (g_p->m->kb_down[k]) machine_key(g_p->m, (uint8_t)k, false);
            if (g_pause_inactive) g_paused_by_focus = true;
        } else g_paused_by_focus = false;
        return 0;
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP: {
        bool down = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
        UINT scan = (lp >> 16) & 0xFF;
        bool ext = (lp >> 24) & 1;
        if (down && wp == VK_RETURN && (GetKeyState(VK_MENU) & 0x8000)) { if (!(lp & (1 << 30))) toggle_fullscreen(); return 0; }
        if (down && wp == VK_F4 && (GetKeyState(VK_MENU) & 0x8000)) { PostMessageW(h, WM_CLOSE, 0, 0); return 0; }
        bool first = down && !(lp & (1 << 30));
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (g_menu != MENU_NONE) {
            // スロット選択中はゲームにキーを渡さない
            if (!down) return 0;
            switch (wp) {
            case VK_LEFT:  g_sel = (g_sel + STATE_SLOTS - 1) % STATE_SLOTS; break;
            case VK_RIGHT: g_sel = (g_sel + 1) % STATE_SLOTS; break;
            case VK_UP:    g_sel = (g_sel + STATE_SLOTS - MENU_COLS) % STATE_SLOTS; break;
            case VK_DOWN:  g_sel = (g_sel + MENU_COLS) % STATE_SLOTS; break;
            case VK_RETURN: case VK_SPACE: if (first) menu_decide(); return 0;
            case VK_ESCAPE: if (first) menu_close(); return 0;
            case VK_F11: if (first) menu_close(); return 0;
            case 'F': if (first) fd_menu(false); return 0;
            case 'E': if (first) fd_eject(); return 0;
            case 'N': if (first) fd_next_disk(); return 0;
            case 'P': if (first) fd_prev_disk(); return 0;
            default: return 0;
            }
            g_menu_note.clear();
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (wp == VK_F11) { if (first) menu_open(shift ? MENU_SAVE : MENU_LOAD); return 0; }
        if (wp == VK_F12) {
            if (shift || g_turbo) { g_turbo = down; update_title(); return 0; }   // Shift+F12（押している間）= 早送り
            if (first) set_capture(!g_captured);
            return 0;
        }
        if (wp == VK_PAUSE) { if (g_p) machine_key(g_p->m, 0x60, down); return 0; }
        int sc = map_key(scan, ext);
        if (sc >= 0 && g_p) {
            bool repeat = down && (lp & (1 << 30));
            if (!repeat || g_p->ps.cfg.key_repeat) machine_key(g_p->m, (uint8_t)sc, down);
        }
        return 0; }
    case WM_SYSCHAR: case WM_CHAR: return 0;
    case WM_MOUSEMOVE:
        if (g_menu != MENU_NONE) {
            int hit = menu_hit((short)LOWORD(lp), (short)HIWORD(lp));
            if (hit >= 0 && hit != g_sel) { g_sel = hit; g_menu_note.clear(); InvalidateRect(h, nullptr, FALSE); }
        }
        break;
    case WM_LBUTTONDOWN:
        if (g_menu != MENU_NONE) {
            int hit = menu_hit((short)LOWORD(lp), (short)HIWORD(lp));
            if (hit >= 0) { g_sel = hit; menu_decide(); }
            else if (hit == HIT_FD_INSERT) fd_menu(true);
            else if (hit == HIT_FD_EJECT) fd_eject();
            else if (hit == HIT_FD_NEXT) fd_next_disk();
            else if (hit == HIT_FD_PREV) fd_prev_disk();
            return 0;
        }
        if (!g_captured && g_p) { set_capture(true); return 0; }
        g_mouse_btn |= 1; return 0;
    case WM_LBUTTONUP: g_mouse_btn &= ~1; return 0;
    case WM_RBUTTONDOWN: if (g_menu != MENU_NONE) { menu_close(); return 0; } if (g_captured) g_mouse_btn |= 2; return 0;
    case WM_RBUTTONUP: g_mouse_btn &= ~2; return 0;
    case WM_MBUTTONDOWN: if (g_middle_release) set_capture(false); return 0;
    case WM_INPUT: {
        if (!g_captured) break;
        UINT sz = 0;
        GetRawInputData((HRAWINPUT)lp, RID_INPUT, nullptr, &sz, sizeof(RAWINPUTHEADER));
        std::vector<uint8_t> buf(sz);
        if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, buf.data(), &sz, sizeof(RAWINPUTHEADER)) == sz) {
            RAWINPUT* ri = (RAWINPUT*)buf.data();
            if (ri->header.dwType == RIM_TYPEMOUSE && !(ri->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
                g_mouse_dx += ri->data.mouse.lLastX;
                g_mouse_dy += ri->data.mouse.lLastY;
            }
        }
        break; }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && g_captured) { SetCursor(nullptr); return TRUE; }
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ---- 起動時の設定 ---------------------------------------------------------------
static std::wstring exe_dir() {
    wchar_t path[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
    std::wstring p = path;
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? L"." : p.substr(0, s);
}
static std::wstring upper(std::wstring s) { for (auto& c : s) c = (wchar_t)towupper(c); return s; }

// INI が無いとき: 起動候補を探してひな形を書く
static std::string guess_start(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    std::vector<std::wstring> bats, exes;
    HANDLE hf = FindFirstFileW((dir + L"\\*.*").c_str(), &fd);
    if (hf != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring n = upper(fd.cFileName);
            size_t d = n.rfind(L'.');
            if (d == std::wstring::npos) continue;
            std::wstring ext = n.substr(d), base = n.substr(0, d);
            if (base == L"PC98PLAYER" || base.find(L"INST") != std::wstring::npos || base == L"AUTOEXEC" || base == L"SETUP" || base == L"CONFIG") continue;
            if (ext == L".BAT") bats.push_back(n);
            else if (ext == L".EXE" || ext == L".COM") exes.push_back(n);
        } while (FindNextFileW(hf, &fd));
        FindClose(hf);
    }
    if (!bats.empty()) return U(bats[0]);
    if (!exes.empty()) return U(exes[0]);
    return "";
}
static void write_template_ini(const std::wstring& path, const std::string& start, char drive = 'A') {
    std::string t =
        "; PC98PLAYER.INI  -- このフォルダのゲームを PC98PLAYER.EXE で起動するための設定\r\n"
        "[PC98PLAYER]\r\n"
        "; 最初に実行するプログラムまたはバッチファイル（引数は Args=）\r\n"
        "Start=" + start + "\r\n"
        "Args=\r\n"
        "; ウインドウの表示倍率（1 で 640x400）\r\n"
        "Scale=2\r\n"
        "; 起動時に全画面（Alt+Enter でも切替）\r\n"
        "FullScreen=0\r\n"
        "; 拡大時になめらかにする（0=ドットのまま）\r\n"
        "Smooth=0\r\n"
        "; 仮想 CPU の速さ（MHz 相当）\r\n"
        "CpuMHz=16\r\n"
        "; ゲームのフォルダを何ドライブに見せるか\r\n"
        "Drive=" + std::string(1, drive) + "\r\n"
        "; 音源ボード（86 / 26 / 0=なし）と割込み（3/10/12/13）\r\n"
        "SoundBoard=86\r\n"
        "SoundIRQ=12\r\n"
        "; MIDI（MPU-PC98II, E0D0h）を載せる。1 にすると Windows の MIDI 出力へ送る（MidiDevice=-1 は既定の出力）\r\n"
        "MIDI=0\r\n"
        "MidiDevice=-1\r\n"
        "; MIDI の演奏速度（%）。MIDI だけ遅いときに 120〜150 などへ上げる（MPU のテンポで演奏するドライバに効く）\r\n"
        "MidiSpeedFix=100\r\n"
        "; 起動時に入れるフロッピーイメージ（D88 / FDI / NFD / ベタ / SCP / HFE）と、そのドライブ名。F11 の画面でも入れ替えられる\r\n"
        "; 実機のドライブも使える: FDD:A（USB フロッピー）, GW または GW:COM5（Greaseweazle）\r\n"
        "FloppyImage=\r\n"
        "FloppyDrive=B\r\n"
        "; Greaseweazle のドライブ（A / B。Shugart 接続は 0〜2）と、1 トラックを何回転読むか\r\n"
        "GWDrive=A\r\n"
        "GWRevs=3\r\n"
        "; 起動時のカレントドライブ（空なら Start= のドライブ）と、ゲームのドライブの空き容量として見せる大きさ（MB）\r\n"
        "CurrentDrive=\r\n"
        "FreeSpaceMB=96\r\n"
        "; EMS（EMM386 相当, ページフレーム D000h）と XMS（HIMEM.SYS 相当）。0 で無し\r\n"
        "EMS=1\r\n"
        "EMSKB=4096\r\n"
        "XMS=1\r\n"
        "XMSKB=8192\r\n"
        "; 先頭 MCB のセグメント（16 進、既定 0200）。動かないソフトで 0100〜0600 などを試す\r\n"
        "FirstMCB=0200\r\n"
        "; 2000 年問題対策: 年だけ置き換える（例 1998）/ 起動日を指定の日付にする（例 1999/12/31）。空なら今日\r\n"
        "FakeYear=\r\n"
        "FakeDate=\r\n"
        "; 音量（%、0〜1000。100 を超えると大きな山は自動で丸めて音割れを抑える）\r\n"
        "Volume=100\r\n"
        "FMVolume=100\r\n"
        "SSGVolume=100\r\n"
        "BeepVolume=50\r\n"
        "; マウスの速さ（%）\r\n"
        "MouseSpeed=100\r\n"
        "; ウインドウのタイトル（空ならフォルダ名）\r\n"
        "Title=\r\n"
        "; 漢字の描画に使うフォント\r\n"
        "Font=ＭＳ ゴシック\r\n"
        "; ゲームが終わったら窓を閉じる\r\n"
        "ExitOnEnd=1\r\n"
        "; 非アクティブ時に一時停止\r\n"
        "PauseInactive=0\r\n"
        "; マウスの中ボタン（ホイール）クリックでマウスを放す（0 で無効。F12 は常に有効）\r\n"
        "MiddleRelease=1\r\n"
        "; 1 にするとマウスを一切捕まえない（窓に閉じ込めず、ゲームにも渡さない。マウスを使わないソフト向け）\r\n"
        "MouseLockDisable=0\r\n";
    // INI は Shift_JIS で書く（メモ帳で開きやすいように）
    std::wstring wt = W(t);
    int n = WideCharToMultiByte(932, 0, wt.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string sj(n ? n - 1 : 0, '\0');
    if (n) WideCharToMultiByte(932, 0, wt.c_str(), -1, &sj[0], n, nullptr, nullptr);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) { DWORD wr; WriteFile(h, sj.data(), (DWORD)sj.size(), &wr, nullptr); CloseHandle(h); }
}

// INI は Shift_JIS でも UTF-8 でもよい。読み込み前に UTF-8 へそろえる
static bool load_ini_any(const std::wstring& path, Ini& ini) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD sz = GetFileSize(h, nullptr), rd = 0;
    std::string data(sz, '\0');
    ReadFile(h, &data[0], sz, &rd, nullptr);
    CloseHandle(h);
    bool utf8 = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data.c_str(), (int)data.size(), nullptr, 0) > 0;
    std::string conv = data;
    if (!utf8) {
        int n = MultiByteToWideChar(932, 0, data.c_str(), (int)data.size(), nullptr, 0);
        std::wstring w(n, L'\0');
        MultiByteToWideChar(932, 0, data.c_str(), (int)data.size(), &w[0], n);
        conv = U(w);
    }
    wchar_t tmp[MAX_PATH], tmpf[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    GetTempFileNameW(tmp, L"p98", 0, tmpf);
    HANDLE o = CreateFileW(tmpf, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    DWORD wr; WriteFile(o, conv.data(), (DWORD)conv.size(), &wr, nullptr); CloseHandle(o);
    bool ok = ini.load(U(tmpf));
    DeleteFileW(tmpf);
    return ok;
}

// ---- 最初に実行するファイルを選ぶ（INI を作るとき） ------------------------------------
//  ゲームのフォルダを開いたファイル選択ダイアログ（*.* が対象。BAT/EXE/COM どれでも）。
//  選んだファイルの、ゲームのフォルダからの相対パス（直下ならファイル名だけ）を返す。やめたら空
static std::string choose_start(const std::wstring& dir, const std::string& guess, const std::wstring& title) {
    std::wstring ud = upper(dir);
    if (!ud.empty() && ud.back() != L'\\') ud += L'\\';
    for (;;) {
        wchar_t file[MAX_PATH] = L"";
        if (!guess.empty()) lstrcpynW(file, W(guess).c_str(), MAX_PATH);
        OPENFILENAMEW of = {};
        of.lStructSize = sizeof(of);
        of.lpstrFilter = L"すべてのファイル (*.*)\0*.*\0起動できるファイル (*.bat;*.exe;*.com)\0*.bat;*.exe;*.com\0";
        of.nFilterIndex = 1;
        of.lpstrFile = file; of.nMaxFile = MAX_PATH;
        of.lpstrInitialDir = dir.c_str();
        of.lpstrTitle = title.c_str();
        of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
        if (!GetOpenFileNameW(&of)) return "";
        std::wstring f = file, uf = upper(f);
        if (uf.compare(0, ud.size(), ud) == 0) return U(f.substr(ud.size()));
        MessageBoxW(nullptr, (L"ゲームのフォルダの中のファイルを選んでください:\n" + dir).c_str(), L"PC98PLAYER", MB_ICONWARNING);
    }
}

// ---- ハードディスクイメージの展開 --------------------------------------------------
//  PC98PLAYER.EXE の第 1 引数に HDI / NHD / THD / HDD を渡すと、カレントフォルダへ中身を展開する。
//  戻り値: 1 = 展開して起動する（*game_dir に展開先）, 0 = やめた／起動しない, -1 = 失敗
// 展開・インストール先: カレントフォルダ。ただし Windows のフォルダやドライブの直下なら、
// イメージの横に同じ名前のフォルダを作ってそこにする
static std::wstring safe_dest(const std::wstring& image, std::wstring dest) {
    std::wstring name = image.substr(image.find_last_of(L"\\/") + 1);
    wchar_t windir[MAX_PATH] = L"";
    GetWindowsDirectoryW(windir, MAX_PATH);
    std::wstring ud = upper(dest), uw = upper(windir);
    if (dest.size() <= 3 || (!uw.empty() && ud.compare(0, uw.size(), uw) == 0)) {
        std::wstring base = name.substr(0, name.find_last_of(L'.'));
        dest = image.substr(0, image.find_last_of(L"\\/")) + L"\\" + base;
    }
    return dest;
}

#include "install_ui.inc"

static int extract_hd_image(const std::wstring& image, std::wstring dest, std::wstring* game_dir) {
    std::wstring name = image.substr(image.find_last_of(L"\\/") + 1);
    dest = safe_dest(image, dest);
    hdimage::Info info; std::string err;
    if (!hdimage::probe(U(image), &info, &err)) {
        MessageBoxW(nullptr, (L"ハードディスクイメージを読めません:\n" + image + L"\n\n" + W(err)).c_str(), L"PC98PLAYER", MB_ICONERROR);
        return -1;
    }
    wchar_t head[512];
    swprintf(head, 512, L"「%ls」（%ls・MS-DOS の区画 %d 個）の中身を、次のフォルダへ展開します。\n\n%ls\n\n"
                        L"同じ名前のファイルが既にあるときは上書きしません。よろしいですか？",
             name.c_str(), W(info.format).c_str(), (int)info.parts.size(), dest.c_str());
    if (MessageBoxW(nullptr, head, L"インストールアシスタント - ハードディスクイメージの展開", MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
    CreateDirectoryW(dest.c_str(), nullptr);
    HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    hdimage::Result res;
    bool ok = hdimage::extract(U(image), U(dest), &res, &err);
    SetCursor(old);
    if (!ok) {
        MessageBoxW(nullptr, (L"展開できませんでした: " + W(err)).c_str(), L"PC98PLAYER", MB_ICONERROR);
        return -1;
    }
    wchar_t sum[512];
    swprintf(sum, 512, L"展開しました: ファイル %d 個・フォルダ %d 個（%.1f MB）", res.files, res.dirs, res.bytes / 1048576.0);
    // 飛ばしたファイル・読めなかったもの・区画が複数あるときだけ、詳しい結果を先に見せる
    if (res.skipped || res.errors || info.parts.size() > 1) {
        std::wstring msg = sum;
        if (res.skipped) msg += L"\n既にあったので飛ばしたファイル: " + std::to_wstring(res.skipped) + L" 個";
        if (res.errors) msg += L"\n読めなかったもの: " + std::to_wstring(res.errors) + L" 個";
        msg += L"\n";
        for (size_t i = 0; i < res.notes.size() && i < 12; i++) msg += L"\n・" + W(res.notes[i]);
        MessageBoxW(nullptr, msg.c_str(), L"PC98PLAYER", res.errors ? MB_ICONWARNING : MB_ICONINFORMATION);
    }
    // INI が無ければ、最初に実行するファイルを選んでもらって作る
    std::wstring ini = dest + L"\\PC98PLAYER.INI";
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        DWORD aa = GetFileAttributesW((dest + L"\\AUTOEXEC.BAT").c_str());
        std::string guess = (aa != INVALID_FILE_ATTRIBUTES && !(aa & FILE_ATTRIBUTE_DIRECTORY)) ? "AUTOEXEC.BAT" : guess_start(dest);
        std::string st = choose_start(dest, guess, std::wstring(sum) + L" ― 最初に実行するファイルを選んでください");
        if (st.empty()) {
            write_template_ini(ini, "");
            MessageBoxW(nullptr, (L"PC98PLAYER.INI を作りました。INI の Start= に最初に実行するファイルを書いてから起動してください。\n\n" + ini).c_str(),
                        L"PC98PLAYER", MB_ICONINFORMATION);
            return 0;
        }
        write_template_ini(ini, st);
    }
    *game_dir = dest;
    return 1;
}

static void create_main_window(HINSTANCE inst, int scale) {
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"PC98PLAYER";
    RegisterClassExW(&wc);
    int ww, wh;
    window_size_for_scale(scale, &ww, &wh);
    g_hwnd = CreateWindowExW(0, L"PC98PLAYER", g_title.c_str(), WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, ww, wh, nullptr, nullptr, inst, nullptr);
    memset(&g_bmi, 0, sizeof(g_bmi));
    g_bmi.bmiHeader.biSize = sizeof(g_bmi.bmiHeader);
    g_bmi.bmiHeader.biWidth = 640; g_bmi.bmiHeader.biHeight = -400;
    g_bmi.bmiHeader.biPlanes = 1; g_bmi.bmiHeader.biBitCount = 32;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int show) {
    SetProcessDPIAware();
    timeBeginPeriod(1);
    std::wstring dir = exe_dir();
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    auto full_path = [](const std::wstring& p) {
        wchar_t b[1024]; DWORD n = GetFullPathNameW(p.c_str(), 1024, b, nullptr);
        std::wstring r = (n && n < 1024) ? std::wstring(b) : p;
        while (r.size() > 3 && (r.back() == L'\\' || r.back() == L'/')) r.pop_back();
        return r;
    };
    auto file_exists = [](const std::wstring& p) { DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); };
    std::wstring ini_path = dir + L"\\PC98PLAYER.INI";
    bool hd_done = false;
    // フロッピーイメージ（1 枚以上）: インストールの画面
    std::vector<std::wstring> fd_args;
    // 実機のフロッピードライブ（エクスプローラーからドライブをドロップすると "A:\" が来る）も同じ画面で
    auto drive_arg = [](const std::wstring& a) -> std::wstring {
        std::wstring u = upper(a);
        if (u.size() >= 2 && u.size() <= 3 && u[1] == L':' && u[0] >= L'A' && u[0] <= L'Z' && (u.size() == 2 || u[2] == L'\\')) {
            wchar_t root[4] = {u[0], L':', L'\\', 0};
            if (GetDriveTypeW(root) == DRIVE_REMOVABLE) return std::wstring(L"FDD:") + u[0];
        }
        if (u == L"GW" || u.compare(0, 3, L"GW:") == 0 || u.compare(0, 4, L"FDD:") == 0) return a;
        return L"";
    };
    bool dev_first = argc >= 2 && !drive_arg(argv[1]).empty();
    if (dev_first) fd_args.push_back(drive_arg(argv[1]));
    else for (int i = 1; i < argc; i++) {
        std::wstring a = full_path(argv[i]);
        if (argv[i][0] != L'-' && file_exists(a) && is_floppy_image_name(a)) fd_args.push_back(a);
    }
    if (dev_first) g_install_mode = true;
    else if (argc >= 2 && !fd_args.empty() && is_floppy_image_name(argv[1])) {
        std::sort(fd_args.begin(), fd_args.end(), [](const std::wstring& x, const std::wstring& y) { return lstrcmpiW(x.c_str(), y.c_str()) < 0; });
        g_install_mode = true;
    }
    if (g_install_mode) {
    } else if (argc >= 2 && argv[1][0] != L'-') {
        std::wstring a = full_path(argv[1]);
        DWORD attr = GetFileAttributesW(a.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY) && hdimage::is_hd_image(U(a))) {
            // ハードディスクイメージ: カレントフォルダへ中身を展開して、そこをゲームのフォルダにする
            int r = extract_hd_image(a, full_path(L"."), &dir);
            if (r < 0) return 1;       // 失敗
            if (r == 0) return 0;      // やめた／起動しない
            ini_path = dir + L"\\PC98PLAYER.INI";
            hd_done = true;
        }
    }
    if (hd_done || g_install_mode) {
    } else if (argc >= 2 && argv[1][0] != L'-') {
        // 引数: ゲームのフォルダ、または INI ファイル
        std::wstring a = full_path(argv[1]);
        DWORD attr = GetFileAttributesW(a.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) { dir = a; ini_path = dir + L"\\PC98PLAYER.INI"; }
        else if (attr != INVALID_FILE_ATTRIBUTES) {
            ini_path = a;
            size_t s = a.find_last_of(L"\\/");
            if (s != std::wstring::npos) dir = a.substr(0, s);
        }
    } else if (argc < 2) {
        // 引数なし: カレントフォルダ（ショートカットの「作業フォルダー」）に INI があればそれを使う。
        // これで EXE を 1 か所に置いたまま、ゲームごとのショートカットで起動できる。
        std::wstring cwd = full_path(L".");
        if (lstrcmpiW(cwd.c_str(), dir.c_str()) != 0) {
            // 作業フォルダーを優先する: INI があればそれ、無くても起動できそうなファイルがあれば
            // そこをゲームのフォルダにする（EXE の横の INI は、作業フォルダーが空のときだけ使う）
            if (file_exists(cwd + L"\\PC98PLAYER.INI") || !guess_start(cwd).empty()) { dir = cwd; ini_path = dir + L"\\PC98PLAYER.INI"; }
        }
    }
    // --fontsheet: 擬似漢字 ROM の一覧を PC98FONT.BMP に書き出して終わる（字形の確認用）
    if (argc >= 2 && lstrcmpiW(argv[1], L"--fontsheet") == 0) {
        font_init(L"");
        FontSource fs0; fs0.kanji = cb_kanji; fs0.ank = cb_ank; fs0.narrow = cb_narrow; fs0.user = nullptr;
        video_set_font(fs0);
        std::vector<uint32_t> px; int w = 0, h = 0;
        fontrom_make_sheet(px, &w, &h);
        std::wstring out = exe_dir() + L"\\PC98FONT.BMP";
        BITMAPFILEHEADER bf = {}; BITMAPINFOHEADER bi = {};
        bi.biSize = sizeof(bi); bi.biWidth = w; bi.biHeight = -h; bi.biPlanes = 1; bi.biBitCount = 32;
        bf.bfType = 0x4D42; bf.bfOffBits = sizeof(bf) + sizeof(bi); bf.bfSize = bf.bfOffBits + (DWORD)(px.size() * 4);
        HANDLE fh = CreateFileW(out.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        DWORD wr;
        WriteFile(fh, &bf, sizeof(bf), &wr, nullptr); WriteFile(fh, &bi, sizeof(bi), &wr, nullptr);
        WriteFile(fh, px.data(), (DWORD)(px.size() * 4), &wr, nullptr); CloseHandle(fh);
        MessageBoxW(nullptr, (L"書き出しました: " + out).c_str(), L"PC98PLAYER", MB_ICONINFORMATION);
        return 0;
    }
    Ini ini;
    PlayerSettings ps;
    if (g_install_mode) {
        // フロッピーからのインストール: 画面でインストーラとドライブを選んでから動かす（INI は作らない）
        dir = safe_dest(floppy::is_device_spec(U(fd_args[0])) ? exe_dir() + L"\\PC98GAME" : fd_args[0], full_path(L"."));
        CreateDirectoryW(dir.c_str(), nullptr);
        if (file_exists(dir + L"\\PC98PLAYER.INI")) load_ini_any(dir + L"\\PC98PLAYER.INI", ini);   // 音などの設定だけ借りる
        player_settings_from_ini(ini, U(dir), &ps);
        g_title = L"インストールアシスタント - PC98PLAYER";
        create_main_window(inst, ps.scale);
        ShowWindow(g_hwnd, show);
        UpdateWindow(g_hwnd);
        if (!install_ui_run(fd_args, dir)) return 0;
        ps.cfg.drive = g_iu.hd;
        ps.cfg.floppy_drive = g_iu.fd;
        ps.cfg.floppy_image = U(g_iu.disks[g_iu.disk]);
        ps.cfg.start = std::string(1, g_iu.fd) + ":\\" + g_iu.chosen;
        ps.cfg.args = U(g_iu.args);
        ps.cfg.current_drive = g_iu.cur_fd ? g_iu.fd : g_iu.hd;
        ps.exit_on_end = false;
        ps.title = "インストールアシスタント";
        g_disk_list = g_iu.disks;
        g_disk_index = g_iu.disk;
        g_mouse_speed = ini.geti("PC98PLAYER.MOUSESPEED", 100);
        g_middle_release = ini.geti("PC98PLAYER.MIDDLERELEASE", 1) != 0;
        g_mouse_lock_disable = ini.geti("PC98PLAYER.MOUSELOCKDISABLE", 0) != 0;
    } else {
    if (GetFileAttributesW(ini_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        // INI が無い: 最初に実行するファイルを選んでもらう（やめたら従来どおり推測）
        std::string st = choose_start(dir, guess_start(dir), L"PC98PLAYER.INI を作ります ― 最初に実行するファイルを選んでください");
        if (!st.empty()) write_template_ini(ini_path, st);
    }
    if (GetFileAttributesW(ini_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::string st = guess_start(dir);
        write_template_ini(ini_path, st);
        std::wstring msg = L"PC98PLAYER.INI が無かったので、ひな形を作りました。\n\n";
        msg += st.empty() ? L"起動するファイルが見つかりません。INI の Start= に書いてから、もう一度起動してください。"
                          : (L"Start=" + W(st) + L" で起動します。違う場合は INI を書き換えてください。");
        MessageBoxW(nullptr, msg.c_str(), L"PC98PLAYER", MB_ICONINFORMATION);
        if (st.empty()) return 1;
    }
    load_ini_any(ini_path, ini);
    // Root=: ゲームのフォルダを INI とは別の場所にする（INI からの相対パス可）
    {
        std::string rt = ini.get("PC98PLAYER.ROOT", "");
        if (!rt.empty()) {
            std::wstring r = W(rt);
            bool abs = r.size() >= 2 && (r[1] == L':' || (r[0] == L'\\' && r[1] == L'\\'));
            std::wstring ini_dir = ini_path.substr(0, ini_path.find_last_of(L"\\/"));
            std::wstring cand = full_path(abs ? r : ini_dir + L"\\" + r);
            DWORD at = GetFileAttributesW(cand.c_str());
            if (at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY)) dir = cand;
            else MessageBoxW(nullptr, (L"Root= のフォルダが見つかりません:\n" + cand + L"\n\nINI のあるフォルダで起動します。").c_str(), L"PC98PLAYER", MB_ICONWARNING);
        }
    }
    player_settings_from_ini(ini, U(dir), &ps);
    if (ps.cfg.start.empty()) {
        // Start= が空: ゲームのフォルダから起動するファイルを推測して、それで起動する
        std::string st = guess_start(dir);
        if (!st.empty()) ps.cfg.start = st;
    }
    if (ps.cfg.start.empty()) {
        std::wstring msg = L"PC98PLAYER.INI の Start= が空です。最初に実行するファイル名を書いてください。\n\n読んだ INI: " + ini_path +
                           L"\nゲームのフォルダ: " + dir;
        MessageBoxW(nullptr, msg.c_str(), L"PC98PLAYER", MB_ICONERROR);
        return 1;
    }
    if (ps.cfg.trace && !_wgetenv(L"PC98PLAYER_LOG")) {
        // Trace=1: ゲームのフォルダの PC98PLAYER.LOG へ記録する（plog は Shift_JIS ではなく UTF-8 で書く）
        std::string lp = hostfs::join(ps.cfg.root, "PC98PLAYER.LOG");
        std::wstring e = L"PC98PLAYER_LOG=" + W(lp);
        _wputenv(e.c_str());
        std::string ac(MAX_PATH * 3, '\0');
        int n = WideCharToMultiByte(CP_ACP, 0, W(lp).c_str(), -1, &ac[0], (int)ac.size(), nullptr, nullptr);
        if (n > 0) { ac.resize(n - 1); _putenv(("PC98PLAYER_LOG=" + ac).c_str()); }
    }
    g_mouse_speed = ini.geti("PC98PLAYER.MOUSESPEED", 100);
    g_pause_inactive = ini.geti("PC98PLAYER.PAUSEINACTIVE", 0) != 0;
    g_middle_release = ini.geti("PC98PLAYER.MIDDLERELEASE", 1) != 0;
    g_mouse_lock_disable = ini.geti("PC98PLAYER.MOUSELOCKDISABLE", 0) != 0;
    }

    font_init(W(ps.font_name));
    FontSource fs; fs.kanji = cb_kanji; fs.ank = cb_ank; fs.narrow = cb_narrow; fs.user = nullptr;
    video_set_font(fs);

    g_title = ps.title.empty() ? W(ps.cfg.root.substr(ps.cfg.root.find_last_of("\\/") == std::string::npos ? 0 : ps.cfg.root.find_last_of("\\/") + 1)) : W(ps.title);
    g_title += L" - PC98PLAYER";

    if (!g_hwnd) create_main_window(inst, ps.scale);
    else SetWindowTextW(g_hwnd, g_title.c_str());

    RAWINPUTDEVICE rid = {0x01, 0x02, 0, g_hwnd};
    RegisterRawInputDevices(&rid, 1, sizeof(rid));

    g_p = new Player();
    std::string err;
    if (!g_p->init(ps, &err)) {
        MessageBoxW(nullptr, W(err).c_str(), L"PC98PLAYER", MB_ICONERROR);
        return 1;
    }
    g_audio.open(ps.sample_rate);
    if (ps.cfg.midi) {
        UINT dev = ps.midi_device < 0 ? MIDI_MAPPER : (UINT)ps.midi_device;
        if (midiOutOpen(&g_midi, dev, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) g_midi = nullptr;
        g_p->midi_sink = midi_sink;
    }
    ShowWindow(g_hwnd, show);
    if (ps.fullscreen) toggle_fullscreen();
    update_title();
    if (!g_p->floppy_error.empty()) show_toast(W(g_p->floppy_error));

    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    const double frame_sec = (double)FRAME_TICKS / MASTER_CLOCK;
    QueryPerformanceCounter(&now);
    double next = (double)now.QuadPart / freq.QuadPart;
    bool running = true;
    bool ended_notice = false;
    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running) break;
        QueryPerformanceCounter(&now);
        double t = (double)now.QuadPart / freq.QuadPart;
        if (g_paused_by_focus) { Sleep(20); next = t; continue; }
        if (g_menu != MENU_NONE) {
            // 止めている間は溜まった音を捨て、入力を待つだけ
            if (g_audio.queued_frames()) audio_flush();
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
            next = t;
            continue;
        }
        if (!g_turbo && t < next) {
            double wait = next - t;
            DWORD ms = (DWORD)(wait * 1000.0);
            if (ms > 0) MsgWaitForMultipleObjects(0, nullptr, FALSE, ms, QS_ALLINPUT);
            continue;
        }
        // マウスを送る
        if (g_captured) {
            int dx = g_mouse_dx * g_mouse_speed / 100, dy = g_mouse_dy * g_mouse_speed / 100;
            g_mouse_dx = 0; g_mouse_dy = 0;
            machine_mouse(g_p->m, dx, dy, g_mouse_btn);
        }
        int frames = g_turbo ? 8 : 1;
        for (int i = 0; i < frames; i++) {
            g_p->run_frame(i == frames - 1);
            if (!g_turbo) {
                int q = g_audio.queued_frames();
                if (q < 12) {
                    if (q == 0) {   // 途切れたら少し先行させる
                        std::vector<int16_t> sil(g_p->audio.size() * 2, 0);
                        g_audio.push(sil.data(), sil.size());
                    }
                    g_audio.push(g_p->audio.data(), g_p->audio.size());
                }
            }
        }
        next += frame_sec;
        if (t - next > 0.25) next = t;
        HDC dc = GetDC(g_hwnd);
        present(dc);
        ReleaseDC(g_hwnd, dc);

        if (g_p->m->quit && !ended_notice) {
            ended_notice = true;
            if (g_p->m->quit == 2) {
                MessageBoxW(g_hwnd, (L"エミュレーションを続けられなくなりました。\n" + W(g_p->m->status)).c_str(), L"PC98PLAYER", MB_ICONWARNING);
                running = false;
            } else if (g_install_mode) {
                if (install_finished()) running = false;
            } else if (g_p->ps.exit_on_end) running = false;
        }
    }
    set_capture(false);
    g_audio.close();
    if (g_midi) { midi_all_off(); midiOutReset(g_midi); midiOutClose(g_midi); g_midi = nullptr; }
    g_p->shutdown();
    timeEndPeriod(1);
    return 0;
}
