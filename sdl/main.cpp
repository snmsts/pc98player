// SPDX-License-Identifier: MIT
// -----------------------------------------------------------------------------
//  sdl/main.cpp  --  PC98PLAYER の SDL3 版（Windows / macOS / Linux）
//
//  win32/main.cpp と同じ使い方・同じ INI を SDL3 で組み直したもの。
//  ・画面      : 640x400 を縦横比を保って拡大（Scale= は最初の窓の大きさ）。Alt+Enter で全画面
//  ・キー      : 物理位置（SDL のスキャンコード）で PC-98 のキーへ（JIS 配列基準）
//  ・マウス    : ウインドウをクリックすると捕まえる（F12 / 中ボタンで放す）
//  ・音        : SDL のオーディオストリーム。先に溜める量は AudioFrames=（既定 4 フレーム ≒ 70ms）
//  ・フォント  : Windows は GDI、それ以外は FreeType（hostfont_*.cpp）
//  ・メニュー  : F11 / Shift+F11 のスロット画面。文字は擬似漢字 ROM でそのまま描く
// -----------------------------------------------------------------------------
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "../core/player.h"
#include "../core/hostfs.h"
#include "../core/floppy.h"
#include "../core/fdreal.h"
#include "hostfont.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#endif

namespace fs = std::filesystem;

static Player*       g_p = nullptr;
static SDL_Window*   g_win = nullptr;
static SDL_Renderer* g_ren = nullptr;
static SDL_Texture*  g_tex = nullptr;       // ゲーム画面
static SDL_Texture*  g_ovl = nullptr;       // メニュー・お知らせ（透過）
static std::vector<uint32_t> g_ov(640 * 400);
static bool          g_captured = false;
static float         g_mouse_ax = 0, g_mouse_ay = 0;   // 捕まえている間の移動量（端数を持ち越す）
static int           g_mouse_btn = 0;
static int           g_mouse_speed = 100;
static bool          g_turbo = false;
static std::string   g_title;
static bool          g_paused_by_focus = false;
static bool          g_pause_inactive = false;
static bool          g_middle_release = true;
static bool          g_mouse_lock_disable = false;
static int           g_audio_frames = 4;
static Uint32        g_ev_dialog = 0;       // ファイル選択の結果（別スレッドから届く）

static fs::path P(const std::string& u8) { return fs::u8path(u8); }
static std::string U8(const fs::path& p) { auto s = p.u8string(); return std::string(s.begin(), s.end()); }

static void message(SDL_MessageBoxFlags kind, const std::string& text) {
    SDL_ShowSimpleMessageBox(kind, "PC98PLAYER", text.c_str(), g_win);
}

// ---- キー変換（SDL のスキャンコード = USB の物理位置 → PC-98）------------------------
static int map_key(SDL_Scancode sc) {
    static const char* rows[3] = {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
    static const int   base[3] = {0x10, 0x1D, 0x29};
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) {
        char c = (char)('A' + (sc - SDL_SCANCODE_A));
        for (int r = 0; r < 3; r++) if (const char* p = strchr(rows[r], c)) return base[r] + (int)(p - rows[r]);
    }
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return 0x01 + (sc - SDL_SCANCODE_1);
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F10) return 0x62 + (sc - SDL_SCANCODE_F1);
    switch (sc) {
    case SDL_SCANCODE_ESCAPE:         return 0x00;
    case SDL_SCANCODE_0:              return 0x0A;
    case SDL_SCANCODE_MINUS:          return 0x0B;
    case SDL_SCANCODE_EQUALS:         return 0x0C;   // ^
    case SDL_SCANCODE_INTERNATIONAL3: return 0x0D;   // ￥
    case SDL_SCANCODE_BACKSPACE:      return 0x0E;
    case SDL_SCANCODE_TAB:            return 0x0F;
    case SDL_SCANCODE_LEFTBRACKET:    return 0x1A;   // @
    case SDL_SCANCODE_RIGHTBRACKET:   return 0x1B;   // [
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return 0x1C;
    case SDL_SCANCODE_SEMICOLON:      return 0x26;
    case SDL_SCANCODE_APOSTROPHE:     return 0x27;   // :
    case SDL_SCANCODE_BACKSLASH: case SDL_SCANCODE_NONUSHASH: return 0x28;   // ]
    case SDL_SCANCODE_COMMA:          return 0x30;
    case SDL_SCANCODE_PERIOD:         return 0x31;
    case SDL_SCANCODE_SLASH:          return 0x32;
    case SDL_SCANCODE_INTERNATIONAL1: return 0x33;   // _
    case SDL_SCANCODE_SPACE:          return 0x34;
    case SDL_SCANCODE_INTERNATIONAL4: return 0x35;   // 変換 → XFER
    case SDL_SCANCODE_PAGEDOWN:       return 0x36;   // ROLL UP
    case SDL_SCANCODE_PAGEUP:         return 0x37;   // ROLL DOWN
    case SDL_SCANCODE_INSERT:         return 0x38;
    case SDL_SCANCODE_DELETE:         return 0x39;
    case SDL_SCANCODE_UP:             return 0x3A;
    case SDL_SCANCODE_LEFT:           return 0x3B;
    case SDL_SCANCODE_RIGHT:          return 0x3C;
    case SDL_SCANCODE_DOWN:           return 0x3D;
    case SDL_SCANCODE_HOME:           return 0x3E;   // HOME/CLR
    case SDL_SCANCODE_END:            return 0x3F;   // HELP
    case SDL_SCANCODE_KP_MINUS:       return 0x40;
    case SDL_SCANCODE_KP_DIVIDE:      return 0x41;
    case SDL_SCANCODE_KP_7:           return 0x42;
    case SDL_SCANCODE_KP_8:           return 0x43;
    case SDL_SCANCODE_KP_9:           return 0x44;
    case SDL_SCANCODE_KP_MULTIPLY:    return 0x45;
    case SDL_SCANCODE_KP_4:           return 0x46;
    case SDL_SCANCODE_KP_5:           return 0x47;
    case SDL_SCANCODE_KP_6:           return 0x48;
    case SDL_SCANCODE_KP_PLUS:        return 0x49;
    case SDL_SCANCODE_KP_1:           return 0x4A;
    case SDL_SCANCODE_KP_2:           return 0x4B;
    case SDL_SCANCODE_KP_3:           return 0x4C;
    case SDL_SCANCODE_KP_EQUALS:      return 0x4D;
    case SDL_SCANCODE_KP_0:           return 0x4E;
    case SDL_SCANCODE_KP_COMMA:       return 0x4F;
    case SDL_SCANCODE_KP_PERIOD:      return 0x50;
    case SDL_SCANCODE_INTERNATIONAL5: return 0x51;   // 無変換 → NFER
    case SDL_SCANCODE_SCROLLLOCK: case SDL_SCANCODE_PAUSE: return 0x60;   // STOP
    case SDL_SCANCODE_PRINTSCREEN:    return 0x61;   // COPY
    case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return 0x70;
    case SDL_SCANCODE_CAPSLOCK:       return 0x71;
    case SDL_SCANCODE_INTERNATIONAL2: case SDL_SCANCODE_LANG1: return 0x72;   // カナ
    case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return 0x73;             // GRPH
    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return 0x74;
    default: return -1;
    }
}

// ---- MIDI（MPU-PC98II の出口。今は Windows だけ）------------------------------------
#ifdef _WIN32
static HMIDIOUT g_midi = nullptr;
static void midi_sink(void*, const uint8_t* msg, int len) {
    if (!g_midi || len <= 0) return;
    if (msg[0] == 0xF0) {
        std::vector<char> buf(msg, msg + len);
        MIDIHDR hd = {};
        hd.lpData = buf.data(); hd.dwBufferLength = (DWORD)len;
        if (midiOutPrepareHeader(g_midi, &hd, sizeof(hd)) != MMSYSERR_NOERROR) return;
        if (midiOutLongMsg(g_midi, &hd, sizeof(hd)) == MMSYSERR_NOERROR)
            for (int i = 0; i < 200 && !(hd.dwFlags & MHDR_DONE); i++) Sleep(1);
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
static bool midi_open(int device) {
    UINT dev = device < 0 ? MIDI_MAPPER : (UINT)device;
    if (midiOutOpen(&g_midi, dev, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) { g_midi = nullptr; return false; }
    g_p->midi_sink = midi_sink;
    return true;
}
static void midi_close() { if (g_midi) { midi_all_off(); midiOutReset(g_midi); midiOutClose(g_midi); g_midi = nullptr; } }
#else
static void midi_all_off() {}
static bool midi_open(int) { return false; }
static void midi_close() {}
#endif

// ---- 音 ------------------------------------------------------------------------
static SDL_AudioStream* g_audio = nullptr;
static void audio_open(int rate) {
    SDL_AudioSpec spec;
    spec.format = SDL_AUDIO_S16; spec.channels = 2; spec.freq = rate;
    g_audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (g_audio) SDL_ResumeAudioStreamDevice(g_audio);
}
static void audio_flush() { if (g_audio) SDL_ClearAudioStream(g_audio); midi_all_off(); }
// 1 フレームぶんを送る。溜まりすぎていたら捨てる（遅れを増やさない）、空になっていたら 1 フレーム先行させる
static void audio_push_frame() {
    if (!g_audio || g_p->audio.empty()) return;
    int bytes = (int)(g_p->audio.size() * sizeof(int16_t));
    int q = SDL_GetAudioStreamQueued(g_audio) / bytes;
    if (q >= g_audio_frames) return;
    if (q == 0) {
        std::vector<int16_t> sil(g_p->audio.size(), 0);
        SDL_PutAudioStreamData(g_audio, sil.data(), bytes);
    }
    SDL_PutAudioStreamData(g_audio, g_p->audio.data(), bytes);
}

// ---- 重ねて描くもの（640x400、擬似漢字 ROM の字で）----------------------------------
static inline uint32_t RGBA(int r, int g, int b, int a = 255) { return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b; }

static void ov_fill(int x, int y, int w, int h, uint32_t c) {
    for (int yy = std::max(y, 0); yy < std::min(y + h, 400); yy++)
        for (int xx = std::max(x, 0); xx < std::min(x + w, 640); xx++) g_ov[yy * 640 + xx] = c;
}
static void ov_glyph(int x, int y, const uint8_t* g, int wbytes, uint32_t c) {
    for (int yy = 0; yy < 16; yy++)
        for (int xx = 0; xx < wbytes * 8; xx++) {
            if (!(g[yy * wbytes + (xx >> 3)] & (0x80 >> (xx & 7)))) continue;
            int px = x + xx, py = y + yy;
            if (px >= 0 && px < 640 && py >= 0 && py < 400) g_ov[py * 640 + px] = c;
        }
}
static bool sjis_lead(uint8_t b) { return (b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC); }
static int ov_text_width(const std::string& utf8) {
    std::string sj = hostfs::to_sjis(utf8);
    int w = 0;
    for (size_t i = 0; i < sj.size(); i++) {
        if (sjis_lead((uint8_t)sj[i]) && i + 1 < sj.size()) { w += 16; i++; } else w += 8;
    }
    return w;
}
// center_w > 0 なら x から center_w の幅の中央に置く。影をつけて読みやすくする
static void ov_text(int x, int y, uint32_t c, const std::string& utf8, int center_w = 0) {
    if (center_w > 0) x += (center_w - ov_text_width(utf8)) / 2;
    std::string sj = hostfs::to_sjis(utf8);
    for (int pass = 0; pass < 2; pass++) {
        int cx = pass ? x : x + 1, cy = pass ? y : y + 1;
        uint32_t col = pass ? c : RGBA(0, 0, 0);
        for (size_t i = 0; i < sj.size(); i++) {
            uint8_t b = (uint8_t)sj[i];
            if (sjis_lead(b) && i + 1 < sj.size()) {
                uint16_t jis = sjis_to_jis((uint16_t)((b << 8) | (uint8_t)sj[i + 1]));
                ov_glyph(cx, cy, font_get_kanji(jis), 2, col);
                cx += 16; i++;
            } else {
                ov_glyph(cx, cy, font_get_ank(b), 1, col);
                cx += 8;
            }
        }
    }
}

// ---- マウスの捕獲・タイトル ----------------------------------------------------------
static void update_title() {
    std::string t = g_title + "  [F11: ロード / Shift+F11: セーブ]";
    if (g_mouse_lock_disable) {}
    else if (g_captured) t += "  [マウス使用中: F12 で解放]";
    else t += "  [クリックでマウスを使う]";
    if (g_turbo) t += "  [早送り]";
    SDL_SetWindowTitle(g_win, t.c_str());
}
static void set_capture(bool on) {
    if (on && g_mouse_lock_disable) return;
    if (on == g_captured) return;
    g_captured = on;
    SDL_SetWindowRelativeMouseMode(g_win, on);
    g_mouse_ax = g_mouse_ay = 0;
    if (!on) { g_mouse_btn = 0; if (g_p) machine_mouse(g_p->m, 0, 0, 0); }
    update_title();
}
static void release_keys() {
    if (g_p) for (int k = 0; k < 128; k++) if (g_p->m->kb_down[k]) machine_key(g_p->m, (uint8_t)k, false);
}
static void toggle_fullscreen() {
    bool full = (SDL_GetWindowFlags(g_win) & SDL_WINDOW_FULLSCREEN) != 0;
    SDL_SetWindowFullscreen(g_win, !full);
    if (g_captured) { set_capture(false); set_capture(true); }
}

// ---- ステートセーブ／ロードのスロット画面 -------------------------------------------
enum { MENU_NONE = 0, MENU_SAVE = 1, MENU_LOAD = 2 };
static int         g_menu = MENU_NONE;
static int         g_sel = 0;
static SlotInfo    g_slots[STATE_SLOTS];
static std::string g_menu_note;
static std::string g_toast;
static Uint64      g_toast_until = 0;
static const int MENU_COLS = 4, CELL_W = 150, CELL_H = 132, GRID_X = 20, GRID_Y = 62;
static const int FD_Y = 328, FD_H = 22, FD_INS_X = 392, FD_INS_W = 128, FD_EJ_X = 526, FD_EJ_W = 94;
enum { HIT_FD_INSERT = -2, HIT_FD_EJECT = -3 };

// フロッピーの小メニュー（win32 版のポップアップメニューの代わり）
struct FdItem { std::string label; int id; bool enabled; std::string spec; };
static std::vector<FdItem> g_fd_items;
static int  g_fd_sel = -1;           // -1 = 閉じている
static const int FDM_X = 300, FDM_W = 320, FDM_IH = 18;
enum { FD_PICK = 1, FD_GW = 2, FD_REREAD = 3, FD_SAVE = 4, FD_DEV = 100 };
enum { DLG_INSERT = 1, DLG_SAVE_D88 = 2 };

static void show_toast(const std::string& t) { g_toast = t; g_toast_until = SDL_GetTicks() + 2500; }

static void menu_open(int kind) {
    if (!g_p) return;
    set_capture(false);
    release_keys();
    for (int i = 0; i < STATE_SLOTS; i++) state_slot_info(g_p->ps.cfg.root, i, &g_slots[i]);
    g_menu = kind;
    g_menu_note.clear();
    g_toast_until = 0;
    g_fd_sel = -1;
    if (kind == MENU_LOAD && !g_slots[g_sel].used)
        for (int i = 0; i < STATE_SLOTS; i++) if (g_slots[i].used) { g_sel = i; break; }
}
static void menu_close() { g_menu = MENU_NONE; g_fd_sel = -1; }

static std::string fd_name() {
    FloppyImage* im = floppy::image();
    if (!im) return "（空）";
    std::string p = im->path;
    size_t k = p.find_last_of("\\/");
    return (k == std::string::npos ? p : p.substr(k + 1)) + "  [" + im->format + (im->wprot ? "・書込禁止" : "") + "]";
}
static std::string drive_str() { char d = floppy::drive_letter(); return std::string(1, d ? d : '-'); }

static void SDLCALL dialog_done(void* user, const char* const* list, int) {
    if (!list || !list[0]) return;   // 取り消し・失敗
    SDL_Event e;
    SDL_zero(e);
    e.type = g_ev_dialog;
    e.user.code = (Sint32)(intptr_t)user;
    e.user.data1 = new std::string(list[0]);
    SDL_PushEvent(&e);
}
static const SDL_DialogFileFilter k_fd_filters[] = {
    {"フロッピーイメージ", "d88;d68;88d;d98;fdi;nfd;hdm;xdf;dup;tfd;2hd;img;scp;hfe"},
    {"すべてのファイル", "*"},
};
static const SDL_DialogFileFilter k_d88_filter[] = { {"D88 イメージ", "d88"} };

static void fd_insert(const std::string& spec) {
    std::string err;
    if (floppy::insert(spec, &err)) {
        show_toast(std::string(floppy::is_device_spec(spec) ? "実機のドライブをつなぎました: " : "") + drive_str() + ": にフロッピーを入れました");
        menu_close();
    } else g_menu_note = "入れられませんでした: " + err;
}
static void fd_save_d88(const std::string& path) {
    FloppyImage* im = floppy::image();
    if (!im) { g_menu_note = "フロッピーは入っていません"; return; }
    std::string err;
    if (im->save_d88(path, &err)) { show_toast("D88 で保存しました"); menu_close(); }
    else g_menu_note = "保存できませんでした: " + err;
}
static void fd_menu_open() {
    g_fd_items.clear();
    g_fd_items.push_back({"イメージファイルを選ぶ…", FD_PICK, true, ""});
    std::vector<fdreal::Device> devs = fdreal::list_devices();
    bool has_gw = false;
    for (size_t i = 0; i < devs.size() && i < 8; i++) {
        g_fd_items.push_back({devs[i].label, FD_DEV, true, devs[i].spec});
        if (devs[i].spec.compare(0, 3, "GW:") == 0) has_gw = true;
    }
    if (!has_gw) g_fd_items.push_back({"Greaseweazle を探してつなぐ", FD_GW, true, "GW"});
    FloppyImage* im = floppy::image();
    g_fd_items.push_back({"読み直す（実機のディスクを入れ替えた）", FD_REREAD, im && im->src, ""});
    g_fd_items.push_back({"今のディスクを D88 で保存…", FD_SAVE, im != nullptr, ""});
    g_fd_sel = 0;
}
static void fd_menu_decide(int i) {
    if (i < 0 || i >= (int)g_fd_items.size() || !g_fd_items[i].enabled) return;
    FdItem it = g_fd_items[i];
    g_fd_sel = -1;
    std::string root = g_p->ps.cfg.root;
    switch (it.id) {
    case FD_PICK:
        SDL_ShowOpenFileDialog(dialog_done, (void*)(intptr_t)DLG_INSERT, g_win, k_fd_filters, 2, root.c_str(), false);
        break;
    case FD_SAVE: {
        std::string def = hostfs::join(root, "DISK.D88");
        SDL_ShowSaveFileDialog(dialog_done, (void*)(intptr_t)DLG_SAVE_D88, g_win, k_d88_filter, 1, def.c_str());
        break; }
    case FD_REREAD:
        floppy::media_changed();
        show_toast("ディスクを読み直しました"); menu_close();
        break;
    case FD_GW: case FD_DEV:
        fd_insert(it.spec);
        break;
    }
}
static void fd_eject() {
    if (!floppy::image()) { g_menu_note = "フロッピーは入っていません"; return; }
    floppy::eject();
    show_toast(drive_str() + ": のフロッピーを取り出しました");
    menu_close();
}

static void menu_decide() {
    std::string err;
    char buf[128];
    if (g_menu == MENU_SAVE) {
        if (g_p->save_state(g_sel, &err)) { snprintf(buf, sizeof(buf), "スロット %d に保存しました", g_sel + 1); show_toast(buf); menu_close(); }
        else g_menu_note = "保存できませんでした: " + err;
    } else if (g_menu == MENU_LOAD) {
        if (!g_slots[g_sel].used) { g_menu_note = "このスロットは空です"; return; }
        if (g_p->load_state(g_sel, &err)) {
            audio_flush();
            snprintf(buf, sizeof(buf), "スロット %d から再開しました", g_sel + 1); show_toast(buf); menu_close();
        } else g_menu_note = "読み込めませんでした: " + err;
    }
}

// 640x400 の座標 → スロット番号（無ければ -1）
static int menu_hit(float lx, float ly) {
    for (int i = 0; i < STATE_SLOTS; i++) {
        int cx = GRID_X + (i % MENU_COLS) * CELL_W, cy = GRID_Y + (i / MENU_COLS) * CELL_H;
        if (lx >= cx && lx < cx + CELL_W - 6 && ly >= cy && ly < cy + CELL_H - 8) return i;
    }
    if (ly >= FD_Y && ly < FD_Y + FD_H) {
        if (lx >= FD_INS_X && lx < FD_INS_X + FD_INS_W) return HIT_FD_INSERT;
        if (lx >= FD_EJ_X && lx < FD_EJ_X + FD_EJ_W) return HIT_FD_EJECT;
    }
    return -1;
}
static int fd_menu_top() { return FD_Y - 6 - (int)g_fd_items.size() * FDM_IH - 4; }
static int fd_menu_hit(float lx, float ly) {
    int top = fd_menu_top() + 2;
    if (lx < FDM_X || lx >= FDM_X + FDM_W || ly < top) return -1;
    int i = (int)(ly - top) / FDM_IH;
    return i < (int)g_fd_items.size() ? i : -1;
}

static void draw_menu() {
    bool save = g_menu == MENU_SAVE;
    ov_fill(0, 0, 640, 400, RGBA(0, 0, 0, 185));
    ov_text(0, 18, save ? RGBA(255, 210, 120) : RGBA(140, 210, 255),
            save ? "ステートセーブ ― 保存するスロットを選んでください" : "ステートロード ― 読み込むスロットを選んでください", 640);
    for (int i = 0; i < STATE_SLOTS; i++) {
        int cx = GRID_X + (i % MENU_COLS) * CELL_W, cy = GRID_Y + (i / MENU_COLS) * CELL_H;
        int fw = CELL_W - 6, fh = CELL_H - 8;
        bool sel = i == g_sel;
        int bw = sel ? 3 : 1;
        ov_fill(cx, cy, fw, fh, sel ? (save ? RGBA(255, 200, 60) : RGBA(80, 190, 255)) : RGBA(90, 90, 110));
        ov_fill(cx + bw, cy + bw, fw - 2 * bw, fh - 2 * bw, sel ? RGBA(40, 40, 60) : RGBA(20, 20, 28));
        int tx = cx + 7, ty = cy + 7, tw = 130, th = 81;
        const SlotInfo& si = g_slots[i];
        if (si.used && si.thumb.size() == (size_t)STATE_THUMB_W * STATE_THUMB_H * 3) {
            for (int y = 0; y < th; y++)
                for (int x = 0; x < tw; x++) {
                    const uint8_t* s = &si.thumb[((y * STATE_THUMB_H / th) * STATE_THUMB_W + x * STATE_THUMB_W / tw) * 3];
                    g_ov[(ty + y) * 640 + tx + x] = RGBA(s[0], s[1], s[2]);
                }
        } else {
            ov_fill(tx, ty, tw, th, RGBA(34, 34, 44));
            ov_text(tx, ty + 32, RGBA(110, 110, 130), "― 空き ―", tw);
        }
        char lab[32]; snprintf(lab, sizeof(lab), "スロット %d", i + 1);
        ov_text(cx + 7, cy + 90, sel ? RGBA(255, 255, 255) : RGBA(200, 200, 210), lab);
        if (si.used) ov_text(cx + 7, cy + 106, sel ? RGBA(230, 230, 230) : RGBA(150, 150, 165), si.when);
    }
    // フロッピーの欄
    ov_text(GRID_X, FD_Y + 3, RGBA(255, 230, 150), "FD " + drive_str() + ":");
    ov_text(GRID_X + 56, FD_Y + 3, floppy::image() ? RGBA(235, 235, 245) : RGBA(130, 130, 145), fd_name());
    ov_fill(FD_INS_X, FD_Y, FD_INS_W, FD_H, RGBA(60, 70, 100));
    ov_text(FD_INS_X, FD_Y + 3, RGBA(255, 255, 255), "F:入れる/実機", FD_INS_W);
    ov_fill(FD_EJ_X, FD_Y, FD_EJ_W, FD_H, RGBA(60, 70, 100));
    ov_text(FD_EJ_X, FD_Y + 3, RGBA(255, 255, 255), "E:取り出す", FD_EJ_W);
    if (!g_menu_note.empty()) ov_text(0, 356, RGBA(255, 120, 120), g_menu_note, 640);
    ov_text(0, 380, RGBA(170, 170, 185), "カーソル/マウス:選ぶ  Enter/左クリック:決定  Esc/右クリック:やめる", 640);
    // フロッピーの小メニュー
    if (g_fd_sel >= 0) {
        int top = fd_menu_top();
        int h = (int)g_fd_items.size() * FDM_IH + 4;
        ov_fill(FDM_X - 1, top - 1, FDM_W + 2, h + 2, RGBA(150, 160, 190));
        ov_fill(FDM_X, top, FDM_W, h, RGBA(28, 30, 42));
        for (size_t i = 0; i < g_fd_items.size(); i++) {
            int y = top + 2 + (int)i * FDM_IH;
            if ((int)i == g_fd_sel) ov_fill(FDM_X + 2, y, FDM_W - 4, FDM_IH, RGBA(60, 90, 150));
            ov_text(FDM_X + 8, y + 1, g_fd_items[i].enabled ? RGBA(240, 240, 250) : RGBA(110, 110, 125), g_fd_items[i].label);
        }
    }
}
static void draw_toast() {
    int tw = std::min(624, std::max(260, ov_text_width(g_toast) + 16));
    ov_fill(8, 8, tw, 24, RGBA(20, 20, 30, 230));
    ov_text(8, 12, RGBA(255, 255, 255), g_toast, tw);
}

static void present() {
    SDL_UpdateTexture(g_tex, nullptr, g_p->fb, 640 * 4);
    SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 255);
    SDL_RenderClear(g_ren);
    SDL_RenderTexture(g_ren, g_tex, nullptr, nullptr);
    bool toast = SDL_GetTicks() < g_toast_until;
    if (g_menu != MENU_NONE || toast) {
        std::fill(g_ov.begin(), g_ov.end(), 0);
        if (g_menu != MENU_NONE) draw_menu(); else draw_toast();
        SDL_UpdateTexture(g_ovl, nullptr, g_ov.data(), 640 * 4);
        SDL_RenderTexture(g_ren, g_ovl, nullptr, nullptr);
    }
    SDL_RenderPresent(g_ren);
}

// ---- 入力 ------------------------------------------------------------------------
static void on_menu_key(SDL_Scancode sc, bool first) {
    if (g_fd_sel >= 0) {   // フロッピーの小メニュー
        int n = (int)g_fd_items.size();
        switch (sc) {
        case SDL_SCANCODE_UP:   g_fd_sel = (g_fd_sel + n - 1) % n; break;
        case SDL_SCANCODE_DOWN: g_fd_sel = (g_fd_sel + 1) % n; break;
        case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_SPACE: if (first) fd_menu_decide(g_fd_sel); break;
        case SDL_SCANCODE_ESCAPE: case SDL_SCANCODE_F: if (first) g_fd_sel = -1; break;
        default: break;
        }
        return;
    }
    switch (sc) {
    case SDL_SCANCODE_LEFT:  g_sel = (g_sel + STATE_SLOTS - 1) % STATE_SLOTS; break;
    case SDL_SCANCODE_RIGHT: g_sel = (g_sel + 1) % STATE_SLOTS; break;
    case SDL_SCANCODE_UP:    g_sel = (g_sel + STATE_SLOTS - MENU_COLS) % STATE_SLOTS; break;
    case SDL_SCANCODE_DOWN:  g_sel = (g_sel + MENU_COLS) % STATE_SLOTS; break;
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_SPACE: if (first) menu_decide(); return;
    case SDL_SCANCODE_ESCAPE: case SDL_SCANCODE_F11: if (first) menu_close(); return;
    case SDL_SCANCODE_F: if (first) fd_menu_open(); return;
    case SDL_SCANCODE_E: if (first) fd_eject(); return;
    default: return;
    }
    g_menu_note.clear();
}

static void on_event(SDL_Event& e, bool& running) {
    if (e.type == g_ev_dialog) {
        std::string* path = (std::string*)e.user.data1;
        if (e.user.code == DLG_INSERT) fd_insert(*path);
        else if (e.user.code == DLG_SAVE_D88) fd_save_d88(*path);
        delete path;
        return;
    }
    switch (e.type) {
    case SDL_EVENT_QUIT: running = false; break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        set_capture(false);
        release_keys();   // 離した瞬間に押しっぱなしのキーが残らないように
        if (g_pause_inactive) g_paused_by_focus = true;
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED: g_paused_by_focus = false; break;
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: {
        bool down = e.key.down, first = down && !e.key.repeat;
        SDL_Scancode sc = e.key.scancode;
        bool alt = (e.key.mod & SDL_KMOD_ALT) != 0, shift = (e.key.mod & SDL_KMOD_SHIFT) != 0;
        if (down && (sc == SDL_SCANCODE_RETURN) && (alt || (e.key.mod & SDL_KMOD_GUI))) { if (first) toggle_fullscreen(); break; }
        if (g_menu != MENU_NONE) { if (down) on_menu_key(sc, first); break; }
        if (sc == SDL_SCANCODE_F11) { if (first) menu_open(shift ? MENU_SAVE : MENU_LOAD); break; }
        if (sc == SDL_SCANCODE_F12) {
            if (shift || g_turbo) { g_turbo = down; update_title(); break; }   // Shift+F12（押している間）= 早送り
            if (first) set_capture(!g_captured);
            break;
        }
        int k = map_key(sc);
        if (k >= 0 && g_p && (!e.key.repeat || g_p->ps.cfg.key_repeat)) machine_key(g_p->m, (uint8_t)k, down);
        break; }
    case SDL_EVENT_MOUSE_MOTION:
        if (g_captured) {
            g_mouse_ax += e.motion.xrel * g_mouse_speed / 100.0f;
            g_mouse_ay += e.motion.yrel * g_mouse_speed / 100.0f;
        } else if (g_menu != MENU_NONE) {
            SDL_ConvertEventToRenderCoordinates(g_ren, &e);
            if (g_fd_sel >= 0) { int h = fd_menu_hit(e.motion.x, e.motion.y); if (h >= 0) g_fd_sel = h; }
            else { int h = menu_hit(e.motion.x, e.motion.y); if (h >= 0 && h != g_sel) { g_sel = h; g_menu_note.clear(); } }
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: {
        bool down = e.button.down;
        int bit = e.button.button == SDL_BUTTON_LEFT ? 1 : e.button.button == SDL_BUTTON_RIGHT ? 2 : 0;
        if (g_menu != MENU_NONE) {
            if (!down) break;
            SDL_ConvertEventToRenderCoordinates(g_ren, &e);
            if (g_fd_sel >= 0) {
                int h = fd_menu_hit(e.button.x, e.button.y);
                if (bit == 1 && h >= 0) fd_menu_decide(h); else g_fd_sel = -1;
                break;
            }
            if (bit == 2) { menu_close(); break; }
            if (bit != 1) break;
            int h = menu_hit(e.button.x, e.button.y);
            if (h >= 0) { g_sel = h; menu_decide(); }
            else if (h == HIT_FD_INSERT) fd_menu_open();
            else if (h == HIT_FD_EJECT) fd_eject();
            break;
        }
        if (e.button.button == SDL_BUTTON_MIDDLE) { if (down && g_middle_release) set_capture(false); break; }
        if (!g_captured) { if (down && bit == 1 && g_p) set_capture(true); break; }
        if (down) g_mouse_btn |= bit; else g_mouse_btn &= ~bit;
        break; }
    default: break;
    }
}

// ---- 起動時の設定 ---------------------------------------------------------------
static std::string upper(std::string s) { for (auto& c : s) c = (char)toupper((unsigned char)c); return s; }

// INI が無いとき: 起動候補を探す（*.BAT を優先、次に *.EXE / *.COM）
static std::string guess_start(const std::string& dir) {
    std::vector<std::string> bats, exes;
    std::error_code ec;
    for (auto& de : fs::directory_iterator(P(dir), ec)) {
        if (!de.is_regular_file(ec)) continue;
        std::string n = upper(U8(de.path().filename()));
        size_t d = n.rfind('.');
        if (d == std::string::npos) continue;
        std::string ext = n.substr(d), base = n.substr(0, d);
        if (base == "PC98PLAYER" || base.find("INST") != std::string::npos || base == "AUTOEXEC" || base == "SETUP" || base == "CONFIG") continue;
        if (ext == ".BAT") bats.push_back(n);
        else if (ext == ".EXE" || ext == ".COM") exes.push_back(n);
    }
    std::sort(bats.begin(), bats.end());
    std::sort(exes.begin(), exes.end());
    if (!bats.empty()) return bats[0];
    if (!exes.empty()) return exes[0];
    return "";
}
static void write_template_ini(const std::string& path, const std::string& start) {
    const char* nl = "\r\n";
    std::ostringstream t;
    t << "; PC98PLAYER.INI  -- このフォルダのゲームを PC98PLAYER で起動するための設定" << nl
      << "[PC98PLAYER]" << nl
      << "; 最初に実行するプログラムまたはバッチファイル（引数は Args=）" << nl
      << "Start=" << start << nl << "Args=" << nl
      << "; ウインドウの表示倍率（1 で 640x400）" << nl << "Scale=2" << nl
      << "; 起動時に全画面（Alt+Enter でも切替）" << nl << "FullScreen=0" << nl
      << "; 拡大時になめらかにする（0=ドットのまま）" << nl << "Smooth=0" << nl
      << "; 仮想 CPU の速さ（MHz 相当）" << nl << "CpuMHz=16" << nl
      << "; ゲームのフォルダを何ドライブに見せるか" << nl << "Drive=A" << nl
      << "; 音源ボード（86 / 26 / 0=なし）と割込み（3/10/12/13）" << nl << "SoundBoard=86" << nl << "SoundIRQ=12" << nl
      << "; MIDI（MPU-PC98II, E0D0h）を載せる。SDL 版では今は Windows だけ音が出る" << nl << "MIDI=0" << nl << "MidiDevice=-1" << nl
      << "MidiSpeedFix=100" << nl
      << "; 起動時に入れるフロッピーイメージ（D88 / FDI / NFD / ベタ / SCP / HFE）と、そのドライブ名。F11 の画面でも入れ替えられる" << nl
      << "FloppyImage=" << nl << "FloppyDrive=B" << nl
      << "GWDrive=A" << nl << "GWRevs=3" << nl
      << "; 起動時のカレントドライブ（空なら Start= のドライブ）と、ゲームのドライブの空き容量として見せる大きさ（MB）" << nl
      << "CurrentDrive=" << nl << "FreeSpaceMB=96" << nl
      << "; EMS（EMM386 相当, ページフレーム D000h）と XMS（HIMEM.SYS 相当）。0 で無し" << nl
      << "EMS=1" << nl << "EMSKB=4096" << nl << "XMS=1" << nl << "XMSKB=8192" << nl
      << "; 先頭 MCB のセグメント（16 進、既定 0200）。動かないソフトで 0100〜0600 などを試す" << nl << "FirstMCB=0200" << nl
      << "; 2000 年問題対策: 年だけ置き換える（例 1998）/ 起動日を指定の日付にする（例 1999/12/31）。空なら今日" << nl
      << "FakeYear=" << nl << "FakeDate=" << nl
      << "; 音量（%、0〜1000）" << nl << "Volume=100" << nl << "FMVolume=100" << nl << "SSGVolume=100" << nl << "BeepVolume=50" << nl
      << "; 音を先に溜めるフレーム数（1 フレーム ≒ 18ms。小さいほど遅れが少ない。音が途切れるなら増やす）" << nl << "AudioFrames=4" << nl
      << "; マウスの速さ（%）" << nl << "MouseSpeed=100" << nl
      << "; ウインドウのタイトル（空ならフォルダ名）" << nl << "Title=" << nl
      << "; 漢字の描画に使うフォント。Windows は書体名、それ以外はフォントファイルのパス（空なら既定）" << nl << "Font=" << nl
      << "; ゲームが終わったら窓を閉じる" << nl << "ExitOnEnd=1" << nl
      << "; 非アクティブ時に一時停止" << nl << "PauseInactive=0" << nl
      << "; マウスの中ボタン（ホイール）クリックでマウスを放す（0 で無効。F12 は常に有効）" << nl << "MiddleRelease=1" << nl
      << "; 1 にするとマウスを一切捕まえない（マウスを使わないソフト向け）" << nl << "MouseLockDisable=0" << nl;
    std::ofstream o(P(path), std::ios::binary);
    o << t.str();
}

static bool is_utf8(const std::string& s) {
    const unsigned char* p = (const unsigned char*)s.data();
    size_t n = s.size(), i = 0;
    while (i < n) {
        int k = p[i] < 0x80 ? 0 : (p[i] & 0xE0) == 0xC0 ? 1 : (p[i] & 0xF0) == 0xE0 ? 2 : (p[i] & 0xF8) == 0xF0 ? 3 : -1;
        if (k < 0 || i + k >= n) return false;
        for (int j = 1; j <= k; j++) if ((p[i + j] & 0xC0) != 0x80) return false;
        i += (size_t)k + 1;
    }
    return true;
}
// INI は Shift_JIS でも UTF-8 でもよい。UTF-8 にそろえてから読む
static bool load_ini_any(const std::string& path, Ini& ini) {
    std::ifstream f(P(path), std::ios::binary);
    if (!f) return false;
    std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (is_utf8(data)) return ini.load(path);
    std::error_code ec;
    fs::path tmp = fs::temp_directory_path(ec) / ("pc98player_" + std::to_string(SDL_GetTicksNS()) + ".ini");
    { std::ofstream o(tmp, std::ios::binary); o << hostfs::from_sjis(data); }
    bool ok = ini.load(U8(tmp));
    fs::remove(tmp, ec);
    return ok;
}

static void set_log_env(const std::string& path) {
#ifdef _WIN32
    // machine.cpp は getenv + fopen で開くので ANSI コードページのパスにする
    std::wstring w = P(path).wstring();
    int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string ac(n > 0 ? n - 1 : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, &ac[0], n, nullptr, nullptr);
    _putenv(("PC98PLAYER_LOG=" + ac).c_str());
#else
    setenv("PC98PLAYER_LOG", path.c_str(), 1);
#endif
}

int main(int argc, char** argv) {
    SDL_SetHint(SDL_HINT_APP_NAME, "PC98PLAYER");
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "512");
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    std::error_code ec;
    auto norm = [&](const fs::path& p) { fs::path r = fs::weakly_canonical(fs::absolute(p, ec), ec); return r.empty() ? p : r; };
    const char* bp = SDL_GetBasePath();
    fs::path exe_dir = norm(P(bp ? bp : "."));
    fs::path dir = exe_dir;
    fs::path ini_path = dir / "PC98PLAYER.INI";
    if (argc >= 2 && argv[1][0] != '-') {
        // 引数: ゲームのフォルダ、または INI ファイル
        fs::path a = norm(P(argv[1]));
        if (fs::is_directory(a, ec)) { dir = a; ini_path = dir / "PC98PLAYER.INI"; }
        else if (fs::exists(a, ec)) { ini_path = a; dir = a.parent_path(); }
    } else if (argc < 2) {
        // 引数なし: カレントフォルダに INI か起動できそうなファイルがあればそこをゲームのフォルダにする
        fs::path cwd = norm(fs::current_path(ec));
        if (cwd != exe_dir && (fs::exists(cwd / "PC98PLAYER.INI", ec) || !guess_start(U8(cwd)).empty())) {
            dir = cwd; ini_path = dir / "PC98PLAYER.INI";
        }
    }
    if (!fs::exists(ini_path, ec)) {
        std::string st = guess_start(U8(dir));
        write_template_ini(U8(ini_path), st);
        message(SDL_MESSAGEBOX_INFORMATION, "PC98PLAYER.INI が無かったので、ひな形を作りました。\n\n" +
                (st.empty() ? std::string("起動するファイルが見つかりません。INI の Start= に書いてから、もう一度起動してください。")
                            : "Start=" + st + " で起動します。違う場合は INI を書き換えてください。"));
        if (st.empty()) return 1;
    }
    Ini ini;
    load_ini_any(U8(ini_path), ini);
    // Root=: ゲームのフォルダを INI とは別の場所にする（INI からの相対パス可）
    {
        std::string rt = ini.get("PC98PLAYER.ROOT", "");
        if (!rt.empty()) {
            fs::path r = P(rt);
            fs::path cand = norm(r.is_absolute() ? r : ini_path.parent_path() / r);
            if (fs::is_directory(cand, ec)) dir = cand;
            else message(SDL_MESSAGEBOX_WARNING, "Root= のフォルダが見つかりません:\n" + U8(cand) + "\n\nINI のあるフォルダで起動します。");
        }
    }
    PlayerSettings ps;
    player_settings_from_ini(ini, U8(dir), &ps);
    if (ps.cfg.start.empty()) ps.cfg.start = guess_start(U8(dir));
    if (ps.cfg.start.empty()) {
        message(SDL_MESSAGEBOX_ERROR, "PC98PLAYER.INI の Start= が空です。最初に実行するファイル名を書いてください。\n\n読んだ INI: " +
                U8(ini_path) + "\nゲームのフォルダ: " + U8(dir));
        return 1;
    }
    if (ps.cfg.trace && !getenv("PC98PLAYER_LOG")) set_log_env(hostfs::join(ps.cfg.root, "PC98PLAYER.LOG"));
    g_mouse_speed = ini.geti("PC98PLAYER.MOUSESPEED", 100);
    g_pause_inactive = ini.geti("PC98PLAYER.PAUSEINACTIVE", 0) != 0;
    g_middle_release = ini.geti("PC98PLAYER.MIDDLERELEASE", 1) != 0;
    g_mouse_lock_disable = ini.geti("PC98PLAYER.MOUSELOCKDISABLE", 0) != 0;
    g_audio_frames = std::max(1, std::min(30, ini.geti("PC98PLAYER.AUDIOFRAMES", 4)));

    std::string ferr;
    bool font_ok = hostfont_open(ps.font_name, &ferr);
    hostfont_install();

    std::string root = ps.cfg.root;
    size_t sl = root.find_last_of("\\/");
    g_title = (ps.title.empty() ? (sl == std::string::npos ? root : root.substr(sl + 1)) : ps.title) + " - PC98PLAYER";

    // 窓: Scale= の大きさ（画面に入らなければ縮める）
    int scale = ps.scale;
    SDL_Rect ub;
    if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &ub))
        while (scale > 1 && (640 * scale > ub.w || 400 * scale > ub.h - 40)) scale--;
    g_win = SDL_CreateWindow(g_title.c_str(), 640 * scale, 400 * scale, SDL_WINDOW_RESIZABLE);
    g_ren = g_win ? SDL_CreateRenderer(g_win, nullptr) : nullptr;
    if (!g_ren) { message(SDL_MESSAGEBOX_ERROR, std::string("窓を作れません: ") + SDL_GetError()); return 1; }
    SDL_SetRenderLogicalPresentation(g_ren, 640, 400, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, 640, 400);
    g_ovl = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 640, 400);
    SDL_SetTextureScaleMode(g_tex, ps.smooth ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
    SDL_SetTextureScaleMode(g_ovl, SDL_SCALEMODE_NEAREST);
    SDL_SetTextureBlendMode(g_ovl, SDL_BLENDMODE_BLEND);
    g_ev_dialog = SDL_RegisterEvents(1);

    g_p = new Player();
    std::string err;
    if (!g_p->init(ps, &err)) { message(SDL_MESSAGEBOX_ERROR, err); return 1; }
    audio_open(ps.sample_rate);
    if (ps.cfg.midi && !midi_open(ps.midi_device)) show_toast("MIDI の出力を開けませんでした");
    if (ps.fullscreen) toggle_fullscreen();
    update_title();
    if (!font_ok) show_toast(ferr);
    if (!g_p->floppy_error.empty()) show_toast(g_p->floppy_error);

    const double frame_sec = (double)FRAME_TICKS / MASTER_CLOCK;
    const double freq = (double)SDL_GetPerformanceFrequency();
    auto now = [&]() { return (double)SDL_GetPerformanceCounter() / freq; };
    double next = now();
    bool running = true, ended_notice = false;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) on_event(e, running);
        if (!running) break;
        double t = now();
        if (g_paused_by_focus) { SDL_WaitEventTimeout(nullptr, 20); next = t; continue; }
        if (g_menu != MENU_NONE) {
            // 止めている間は溜まった音を捨て、入力を待つだけ
            if (g_audio && SDL_GetAudioStreamQueued(g_audio) > 0) audio_flush();
            present();
            SDL_WaitEventTimeout(nullptr, 50);
            next = t;
            continue;
        }
        if (!g_turbo && t < next) {
            double wait = next - t;
            if (wait > 0.003) SDL_WaitEventTimeout(nullptr, (Sint32)((wait - 0.002) * 1000.0));
            else SDL_DelayPrecise((Uint64)(wait * 1e9));
            continue;
        }
        if (g_captured) {
            int dx = (int)g_mouse_ax, dy = (int)g_mouse_ay;
            g_mouse_ax -= dx; g_mouse_ay -= dy;
            machine_mouse(g_p->m, dx, dy, g_mouse_btn);
        }
        int frames = g_turbo ? 8 : 1;
        for (int i = 0; i < frames; i++) {
            g_p->run_frame(i == frames - 1);
            if (!g_turbo) audio_push_frame();
        }
        next += frame_sec;
        if (t - next > 0.25) next = t;
        present();

        if (g_p->m->quit && !ended_notice) {
            ended_notice = true;
            if (g_p->m->quit == 2) {
                message(SDL_MESSAGEBOX_WARNING, "エミュレーションを続けられなくなりました。\n" + g_p->m->status);
                running = false;
            } else if (g_p->ps.exit_on_end) running = false;
        }
    }
    set_capture(false);
    if (g_audio) SDL_DestroyAudioStream(g_audio);
    midi_close();
    g_p->shutdown();
    SDL_Quit();
    return 0;
}
