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
#include "../core/hdimage.h"
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
static bool          g_memedit_paused = false;     // メモリエディタの「操作中はゲームを止める」で止めている
static bool          g_pause_inactive = false;
static bool          g_middle_release = true;
static bool          g_mouse_lock_disable = false;
static bool          g_mouse_shared = false;   // MouseLockDisable=2: 捕まえずに、カーソルが画面の上にある間だけゲームへ送る
static bool          g_shared_in = false;      // 共有モード: カーソルが画面の上にあるか
static int           g_audio_frames = 4;
static Uint32        g_ev_dialog = 0;       // ファイル選択の結果（別スレッドから届く）

static fs::path P(const std::string& u8) { return fs::u8path(u8); }
static std::string U8(const fs::path& p) { auto s = p.u8string(); return std::string(s.begin(), s.end()); }

static void message(SDL_MessageBoxFlags kind, const std::string& text) {
    SDL_ShowSimpleMessageBox(kind, "PC98PLAYER", text.c_str(), g_win);
}
// はい／いいえ。default_yes=false なら Enter は「いいえ」
static bool ask_yes_no(const std::string& title, const std::string& text, bool default_yes = true) {
    const SDL_MessageBoxButtonData bt[2] = {
        {default_yes ? (Uint32)SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT : 0u, 1, "はい"},
        {(Uint32)SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT | (default_yes ? 0u : (Uint32)SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT), 0, "いいえ"},
    };
    SDL_MessageBoxData d = {SDL_MESSAGEBOX_INFORMATION | SDL_MESSAGEBOX_BUTTONS_LEFT_TO_RIGHT, g_win, title.c_str(), text.c_str(), 2, bt, nullptr};
    int id = 0;
    return SDL_ShowMessageBox(&d, &id) && id == 1;
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
    else if (g_mouse_shared) t += "  [マウス: 画面の上で動かす]";
    else if (g_captured) t += "  [マウス使用中: F12 で解放]";
    else t += "  [クリックでマウスを使う]";
    if (g_turbo) t += "  [早送り]";
    if (g_memedit_paused) t += "  [一時停止中: エディタ操作中]";
    SDL_SetWindowTitle(g_win, t.c_str());
}
static void set_capture(bool on) {
    if (on && (g_mouse_lock_disable || g_mouse_shared)) return;   // MouseLockDisable=1/2: クリックしても F12 でも捕まえない
    if (on == g_captured) return;
    g_captured = on;
    SDL_SetWindowRelativeMouseMode(g_win, on);
    g_mouse_ax = g_mouse_ay = 0;
    if (!on) { g_mouse_btn = 0; if (g_p) machine_mouse(g_p->m, 0, 0, 0); }
    update_title();
}
// 共有モード: カーソルが画面の外へ出た（押していたボタンも離す）
static void shared_out() {
    if (g_shared_in) { g_mouse_btn = 0; if (g_p) machine_mouse(g_p->m, 0, 0, 0); }
    g_shared_in = false;
    g_mouse_ax = g_mouse_ay = 0;
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
static const int MENU_COLS = 4, CELL_W = 150, CELL_H = 132, GRID_X = 20, GRID_Y = 42;
// 下の段: 1 行目 = フロッピーの名前（右端に前／次のディスク）、2 行目 = ボタン（左がフロッピー、右が「その他」）
static const int FD_Y1 = 306, FD_Y = 330, FD_H = 20;
static const int FD_INS_X = 20, FD_INS_W = 112, FD_EJ_X = 138, FD_EJ_W = 88, FD_UNIT_X = 232, FD_UNIT_W = 88;
static const int OT_MEM_X = 326, OT_MEM_W = 104, OT_CODE_X = 436, OT_CODE_W = 104, OT_REBOOT_X = 546, OT_REBOOT_W = 74;
static const int FD_PREV_X = 512, FD_PREV_W = 52, FD_NEXT_X = 568, FD_NEXT_W = 52;
enum { HIT_FD_INSERT = -2, HIT_FD_EJECT = -3, HIT_FD_NEXT = -4, HIT_FD_PREV = -5, HIT_REBOOT = -6, HIT_MEMEDIT = -7, HIT_FD_UNIT = -8, HIT_CODEEDIT = -9 };

// フロッピーの小メニュー（win32 版のポップアップメニューの代わり）
struct FdItem { std::string label; int id; bool enabled; std::string spec; };
static std::vector<FdItem> g_fd_items;
static int  g_fd_sel = -1;           // -1 = 閉じている
static const int FDM_X = 20, FDM_W = 400, FDM_IH = 18;
enum { FD_PICK = 1, FD_GW = 2, FD_REREAD = 3, FD_SAVE = 4, FD_FOLDER = 5, FD_WPROT = 6, FD_DEV = 100, FD_LIST = 200 };
enum { DLG_INSERT = 1, DLG_SAVE_D88 = 2, DLG_FOLDER = 3, DLG_FD_FOLDER = 4 };

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

// ---- 仮想フロッピー --------------------------------------------------------------
static std::vector<std::string> g_disk_list;   // 「前／次のディスク」で順に入れ替える一覧
static int g_disk_index = 0;
// ブートモード（フロッピーから起動するゲーム）や FloppyDrive2= のときはドライブが 2 台。
// F11 の画面の操作は g_fd_unit のドライブに対して行う
static int g_fd_unit = 0;
static bool fd_boot_mode() { return g_p && g_p->ps.cfg.boot_fd; }
static bool fd_two() { return fd_boot_mode() || floppy::drive_letter_unit(1) != 0; }
static int fd_unit() { return fd_two() ? g_fd_unit : 0; }
static bool fd_folder_ok() { return !fd_boot_mode() && fd_unit() == 0; }   // フォルダを入れられるのは 1 台目（DOS）だけ
static FloppyImage* fd_img() { return floppy::image_unit(fd_unit()); }
static std::string fd_cur_path() { return floppy::current_path_unit(fd_unit()); }
static std::string base_name(const std::string& p) { size_t k = p.find_last_of("\\/"); return k == std::string::npos ? p : p.substr(k + 1); }
static std::string fd_label_unit(int u) {   // 「B:」または「ドライブ1」
    if (fd_boot_mode()) return "ドライブ" + std::to_string(u + 1);
    char l = floppy::drive_letter_unit(u);
    return std::string(1, l ? l : '-') + ":";
}
static std::string fd_label() { return fd_label_unit(fd_unit()); }
static bool same_path(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) if (toupper((unsigned char)a[i]) != toupper((unsigned char)b[i])) return false;
    return true;
}
// 今のドライブに入っているディスクが、一覧の何枚目か（無ければ -1）
static int fd_list_index() {
    std::string cur = fd_cur_path();
    for (size_t i = 0; i < g_disk_list.size(); i++) if (same_path(g_disk_list[i], cur)) return (int)i;
    return -1;
}
static std::string fd_name() {
    FloppyImage* im = fd_img();
    if (!im && fd_folder_ok() && !floppy::folder().empty()) return "[" + base_name(floppy::folder()) + "]  [フォルダ]";
    if (!im) return "（空）";
    return base_name(im->path) + "  [" + im->format + (im->wprot ? "・書込禁止" : "") + "]";
}

static void SDLCALL dialog_done(void* user, const char* const* list, int) {
    int code = (int)(intptr_t)user;
    bool picked = list && list[0];
    if (!picked && code != DLG_FOLDER) return;   // 取り消し・失敗（フォルダ選びだけは取り消しも知らせる）
    SDL_Event e;
    SDL_zero(e);
    e.type = g_ev_dialog;
    e.user.code = code;
    e.user.data1 = picked ? new std::string(list[0]) : nullptr;
    SDL_PushEvent(&e);
}
static const SDL_DialogFileFilter k_fd_filters[] = {
    {"フロッピーイメージ", "d88;d68;88d;d98;fdi;nfd;hdm;xdf;dup;tfd;2hd;img;scp;hfe"},
    {"すべてのファイル", "*"},
};
static const SDL_DialogFileFilter k_d88_filter[] = { {"D88 イメージ", "d88"} };

static void fd_insert(const std::string& spec) {
    std::string err;
    if (floppy::insert_unit(fd_unit(), spec, &err)) {
        show_toast(fd_label() + (floppy::is_device_spec(spec) ? " に実機のドライブをつなぎました（" + spec + "）" : " にフロッピーを入れました"));
        menu_close();
    } else if (g_menu != MENU_NONE) g_menu_note = "入れられませんでした: " + err;
    else show_toast("入れられませんでした: " + err);
}
static void fd_insert_folder(const std::string& dir) {
    if (!fd_folder_ok()) { g_menu_note = "フォルダを入れられるのは 1 台目のドライブ（MS-DOS のゲーム）だけです"; return; }
    std::string err;
    if (floppy::insert(dir, &err)) { show_toast(fd_label() + " にフォルダを入れました"); menu_close(); }
    else g_menu_note = "入れられませんでした: " + err;
}
static bool is_floppy_image(const std::string& path) {
    size_t d = path.rfind('.');
    if (d == std::string::npos) return false;
    std::string ext = path.substr(d + 1);
    for (auto& c : ext) c = (char)tolower((unsigned char)c);
    std::string list = std::string(";") + k_fd_filters[0].pattern + ";";
    return list.find(";" + ext + ";") != std::string::npos;
}
static void fd_save_d88(const std::string& path) {
    FloppyImage* im = fd_img();
    if (!im) { g_menu_note = "フロッピーは入っていません"; return; }
    std::string err;
    if (im->save_d88(path, &err)) { show_toast("D88 で保存しました"); menu_close(); }
    else g_menu_note = "保存できませんでした: " + err;
}
static void fd_insert_listed(int k) {
    if (k < 0 || k >= (int)g_disk_list.size()) return;
    std::string err;
    if (floppy::insert_unit(fd_unit(), g_disk_list[k], &err)) {
        g_disk_index = k;
        char buf[64]; snprintf(buf, sizeof(buf), "（%d/%d）", k + 1, (int)g_disk_list.size());
        show_toast(fd_label() + " ← " + base_name(g_disk_list[k]) + buf);
        menu_close();
    } else g_menu_note = "入れられませんでした: " + err;
}
static int fd_base_index() { int i = fd_list_index(); return i >= 0 ? i : g_disk_index; }
static void fd_next_disk() { if (g_disk_list.size() > 1) fd_insert_listed((fd_base_index() + 1) % (int)g_disk_list.size()); }
static void fd_prev_disk() { if (g_disk_list.size() > 1) fd_insert_listed((fd_base_index() + (int)g_disk_list.size() - 1) % (int)g_disk_list.size()); }
static void fd_menu_open() {
    g_fd_items.clear();
    g_fd_items.push_back({"イメージファイルを選ぶ…", FD_PICK, true, ""});
    if (fd_folder_ok()) g_fd_items.push_back({"フォルダを入れる（キーディスク向け）…", FD_FOLDER, true, ""});
    int cur = fd_list_index();
    for (size_t i = 0; i < g_disk_list.size() && i < 8; i++)
        g_fd_items.push_back({std::string((int)i == cur ? "＊" : "　") + std::to_string(i + 1) + " 枚目: " + base_name(g_disk_list[i]), FD_LIST + (int)i, true, ""});
    std::vector<fdreal::Device> devs = fdreal::list_devices();
    bool has_gw = false;
    for (size_t i = 0; i < devs.size() && i < 8; i++) {
        g_fd_items.push_back({devs[i].label, FD_DEV, true, devs[i].spec});
        if (devs[i].spec.compare(0, 3, "GW:") == 0) has_gw = true;
    }
    if (!has_gw) g_fd_items.push_back({"Greaseweazle を探してつなぐ", FD_GW, true, "GW"});
    FloppyImage* im = fd_img();
    g_fd_items.push_back({"読み直す（実機のディスクを入れ替えた）", FD_REREAD, im && im->src, ""});
    g_fd_items.push_back({std::string(im && im->wprot ? "＊" : "　") + "書き込み禁止（ライトプロテクト）", FD_WPROT, im != nullptr, ""});
    g_fd_items.push_back({"今のディスクを D88 で保存…", FD_SAVE, im != nullptr, ""});
    g_fd_sel = 0;
}
static void fd_menu_decide(int i) {
    if (i < 0 || i >= (int)g_fd_items.size() || !g_fd_items[i].enabled) return;
    FdItem it = g_fd_items[i];
    g_fd_sel = -1;
    std::string root = g_p->ps.cfg.root;
    if (it.id >= FD_LIST) { fd_insert_listed(it.id - FD_LIST); return; }
    switch (it.id) {
    case FD_PICK:
        SDL_ShowOpenFileDialog(dialog_done, (void*)(intptr_t)DLG_INSERT, g_win, k_fd_filters, 2, root.c_str(), false);
        break;
    case FD_FOLDER:
        SDL_ShowOpenFolderDialog(dialog_done, (void*)(intptr_t)DLG_FD_FOLDER, g_win, root.c_str(), false);
        break;
    case FD_SAVE: {
        std::string def = hostfs::join(root, "DISK.D88");
        SDL_ShowSaveFileDialog(dialog_done, (void*)(intptr_t)DLG_SAVE_D88, g_win, k_d88_filter, 1, def.c_str());
        break; }
    case FD_REREAD:
        floppy::media_changed();
        show_toast("ディスクを読み直しました"); menu_close();
        break;
    case FD_WPROT:   // 書き込み禁止のつまみ（実機のドライブで書けないものは解除できない）
        if (FloppyImage* im = fd_img()) {
            if (im->wprot && im->src && !im->src->writable()) { g_menu_note = "このドライブには書き込めません"; break; }
            im->wprot = !im->wprot;
            show_toast(fd_label() + (im->wprot ? " を書き込み禁止にしました" : " の書き込み禁止を解除しました"));
            menu_close();
        }
        break;
    case FD_GW: case FD_DEV:
        fd_insert(it.spec);
        break;
    }
}
static void fd_eject() {
    if (!fd_img() && (!fd_folder_ok() || floppy::folder().empty())) { g_menu_note = "フロッピーは入っていません"; return; }
    floppy::eject_unit(fd_unit());
    show_toast(fd_label() + " のフロッピーを取り出しました");
    menu_close();
}
// 操作するドライブを 1 ⇔ 2 で切り替える
static void fd_toggle_unit() {
    if (!fd_two()) return;
    g_fd_unit ^= 1;
    g_menu_note.clear();
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

#include "memedit.inc"
#include "codeedit.inc"
#include "gamepad.inc"

// 「その他」: プログラム再起動（PC-98 の電源を入れ直す。INI も読み直す）
static bool reload_settings(PlayerSettings* out, std::string* err);
static void apply_live_settings(const PlayerSettings& old_ps);
static void menu_reboot() {
    if (!g_p) return;
    if (!ask_yes_no("プログラム再起動", "PC-98 を起動し直して、最初からやり直しますか？\n\n"
                    "ゲームでセーブしていない進行は失われます（ステートセーブは残ります）。", false)) return;
    audio_flush();
    g_turbo = false;
    std::string err, rerr;
    PlayerSettings old_ps = g_p->ps, nps;
    pad::release_all(g_p->m);
    bool reloaded = reload_settings(&nps, &rerr);   // INI を読み直す（読めなければ今の設定のまま）
    bool ok = reloaded ? g_p->reboot(nps, &err) : g_p->reboot(&err);
    pad::forget();
    if (!ok) { g_menu_note = "起動し直せませんでした: " + err; return; }
    if (reloaded) apply_live_settings(old_ps);
    memedit::on_reboot();
    codeedit::on_reboot();
    menu_close();
    if (!g_p->floppy_error.empty()) show_toast(g_p->floppy_error);
    else if (!rerr.empty()) show_toast(rerr);
    else show_toast(reloaded ? "INI を読み直して、プログラムを起動し直しました" : "プログラムを起動し直しました");
    update_title();
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
        if (fd_two() && lx >= FD_UNIT_X && lx < FD_UNIT_X + FD_UNIT_W) return HIT_FD_UNIT;
        if (lx >= OT_MEM_X && lx < OT_MEM_X + OT_MEM_W) return HIT_MEMEDIT;
        if (lx >= OT_CODE_X && lx < OT_CODE_X + OT_CODE_W) return HIT_CODEEDIT;
        if (lx >= OT_REBOOT_X && lx < OT_REBOOT_X + OT_REBOOT_W) return HIT_REBOOT;
    }
    if (g_disk_list.size() > 1 && ly >= FD_Y1 && ly < FD_Y1 + FD_H) {
        if (lx >= FD_PREV_X && lx < FD_PREV_X + FD_PREV_W) return HIT_FD_PREV;
        if (lx >= FD_NEXT_X && lx < FD_NEXT_X + FD_NEXT_W) return HIT_FD_NEXT;
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
// 幅 w に収まるように後ろを省く
static std::string ov_fit(const std::string& utf8, int w) {
    if (ov_text_width(utf8) <= w) return utf8;
    std::string s = utf8;
    while (!s.empty() && ov_text_width(s + "…") > w) {
        size_t k = s.size() - 1;
        while (k > 0 && ((unsigned char)s[k] & 0xC0) == 0x80) k--;
        s.resize(k);
    }
    return s + "…";
}

static void draw_menu() {
    bool save = g_menu == MENU_SAVE;
    ov_fill(0, 0, 640, 400, RGBA(0, 0, 0, 185));
    ov_text(0, 14, save ? RGBA(255, 210, 120) : RGBA(140, 210, 255),
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
    // フロッピーの欄: 1 行目 = 今のドライブとディスク（2 台あるときはもう一方も）、右端に前／次
    {
        bool pn = g_disk_list.size() > 1;
        std::string head = (fd_two() ? fd_label() : "FD " + fd_label()) + " ";
        ov_text(GRID_X, FD_Y1 + 2, RGBA(255, 230, 150), head);
        int nx = GRID_X + ov_text_width(head) + 4;
        std::string nm = fd_name();
        if (fd_two()) {
            std::string o = floppy::current_path_unit(g_fd_unit ^ 1);
            nm += "　／ " + fd_label_unit(g_fd_unit ^ 1) + " " + (o.empty() ? "（空）" : base_name(o));
        }
        if (pn && fd_list_index() >= 0) nm += "　" + std::to_string(fd_list_index() + 1) + "/" + std::to_string(g_disk_list.size()) + " 枚目";
        ov_text(nx, FD_Y1 + 2, !fd_cur_path().empty() ? RGBA(235, 235, 245) : RGBA(130, 130, 145), ov_fit(nm, (pn ? FD_PREV_X - 8 : 620) - nx));
        auto button = [&](int x, int y, int w, const char* t) {
            ov_fill(x, y, w, FD_H, RGBA(60, 70, 100));
            ov_text(x, y + 2, RGBA(255, 255, 255), t, w);
        };
        if (pn) { button(FD_PREV_X, FD_Y1, FD_PREV_W, "P:前"); button(FD_NEXT_X, FD_Y1, FD_NEXT_W, "N:次"); }
        button(FD_INS_X, FD_Y, FD_INS_W, "F:入れる/実機");
        button(FD_EJ_X, FD_Y, FD_EJ_W, "E:取り出す");
        if (fd_two()) button(FD_UNIT_X, FD_Y, FD_UNIT_W, "D:ドライブ");
        button(OT_MEM_X, FD_Y, OT_MEM_W, "M:メモリ編集");
        button(OT_CODE_X, FD_Y, OT_CODE_W, "C:コード編集");
        button(OT_REBOOT_X, FD_Y, OT_REBOOT_W, "R:再起動");
    }
    if (!g_menu_note.empty()) ov_text(0, 356, RGBA(255, 120, 120), ov_fit(g_menu_note, 636), 640);
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
            ov_text(FDM_X + 8, y + 1, g_fd_items[i].enabled ? RGBA(240, 240, 250) : RGBA(110, 110, 125), ov_fit(g_fd_items[i].label, FDM_W - 16));
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
    case SDL_SCANCODE_N: if (first) fd_next_disk(); return;
    case SDL_SCANCODE_P: if (first) fd_prev_disk(); return;
    case SDL_SCANCODE_D: if (first) fd_toggle_unit(); return;
    case SDL_SCANCODE_R: if (first) menu_reboot(); return;
    case SDL_SCANCODE_M: if (first) { menu_close(); memedit::show(); } return;
    case SDL_SCANCODE_C: if (first) { menu_close(); codeedit::show(); } return;
    default: return;
    }
    g_menu_note.clear();
}

static void on_event(SDL_Event& e, bool& running) {
    if (memedit::owns(e)) { memedit::on_event(e); return; }   // メモリエディタの窓あての入力
    if (codeedit::owns(e)) { codeedit::on_event(e); return; }   // コードエディタの窓あての入力
    if (e.type == g_ev_dialog) {
        std::string* path = (std::string*)e.user.data1;
        if (!path) return;
        if (e.user.code == DLG_INSERT) fd_insert(*path);
        else if (e.user.code == DLG_SAVE_D88) fd_save_d88(*path);
        else if (e.user.code == DLG_FD_FOLDER) fd_insert_folder(*path);
        delete path;
        return;
    }
    switch (e.type) {
    case SDL_EVENT_QUIT: running = false; break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED: running = false; break;   // 窓が 2 つあると、本体の窓を閉じても QUIT は来ない
    case SDL_EVENT_DROP_FILE:   // 動いている最中に落とされたフロッピーイメージは入れる
        if (e.drop.data && is_floppy_image(e.drop.data)) fd_insert(e.drop.data);
        else if (e.drop.data) show_toast("フロッピーイメージではありません");
        break;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE: if (g_mouse_shared && !g_mouse_btn) shared_out(); break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        if (g_mouse_shared) { g_mouse_btn = 0; shared_out(); SDL_CaptureMouse(false); }
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
        if (g_mouse_shared && g_menu == MENU_NONE) {
            // 共有モード: カーソルが画面（640x400）の上にある間だけ、その動きを PC-98 のドット数に直して送る
            SDL_ConvertEventToRenderCoordinates(g_ren, &e);
            bool in = e.motion.x >= 0 && e.motion.x < 640 && e.motion.y >= 0 && e.motion.y < 400;
            if (in || g_mouse_btn) {
                if (g_shared_in) { g_mouse_ax += e.motion.xrel * g_mouse_speed / 100.0f; g_mouse_ay += e.motion.yrel * g_mouse_speed / 100.0f; }
                g_shared_in = true;
            } else shared_out();
            break;
        }
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
            else if (h == HIT_FD_NEXT) fd_next_disk();
            else if (h == HIT_FD_PREV) fd_prev_disk();
            else if (h == HIT_FD_UNIT) fd_toggle_unit();
            else if (h == HIT_REBOOT) menu_reboot();
            else if (h == HIT_MEMEDIT) { menu_close(); memedit::show(); }
            else if (h == HIT_CODEEDIT) { menu_close(); codeedit::show(); }
            break;
        }
        if (e.button.button == SDL_BUTTON_MIDDLE) { if (down && g_middle_release) set_capture(false); break; }
        if (g_mouse_shared) {   // 共有モード: 画面の上で押したら送る。押している間は窓の外で離しても届くように捕まえる
            SDL_ConvertEventToRenderCoordinates(g_ren, &e);
            bool in = e.button.x >= 0 && e.button.x < 640 && e.button.y >= 0 && e.button.y < 400;
            if (down && in) { g_mouse_btn |= bit; g_shared_in = true; }
            else if (!down) g_mouse_btn &= ~bit;
            SDL_CaptureMouse(g_mouse_btn != 0);
            break;
        }
        if (!g_captured) { if (down && bit == 1 && g_p) set_capture(true); break; }
        if (down) g_mouse_btn |= bit; else g_mouse_btn &= ~bit;
        break; }
    default: break;
    }
}

// ---- 起動時の設定 ---------------------------------------------------------------
static std::string upper(std::string s) { for (auto& c : s) c = (char)toupper((unsigned char)c); return s; }

// dir の中の name（大文字小文字は区別しない）。無ければ空
static fs::path find_ci(const std::string& dir, const std::string& name) {
    std::error_code ec;
    std::string un = upper(name);
    for (auto& de : fs::directory_iterator(P(dir), ec))
        if (de.is_regular_file(ec) && upper(U8(de.path().filename())) == un) return de.path();
    return fs::path();
}
// INI が無いとき: 起動候補を探す
//  1) CONFIG.SYS の SHELL= がゲームの起動用プログラムならそれ（実機ではこれが最初に動く）
//  2) AUTOEXEC.BAT
//  3) *.BAT、次に *.EXE / *.COM
static std::string guess_start(const std::string& dir) {
    std::error_code ec;
    {
        std::ifstream f(find_ci(dir, "CONFIG.SYS"), std::ios::binary);
        std::string t((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (t.size() > 4096) t.resize(4096);
        t = upper(t);
        size_t p = t.find("SHELL");
        while (p != std::string::npos && p > 0 && t[p - 1] != '\n' && t[p - 1] != '\r' && t[p - 1] != ' ') p = t.find("SHELL", p + 1);
        size_t e = p == std::string::npos ? p : t.find('=', p);
        if (e != std::string::npos) {
            size_t s0 = e + 1;
            while (s0 < t.size() && (t[s0] == ' ' || t[s0] == '\t')) s0++;
            size_t s1 = s0;
            while (s1 < t.size() && t[s1] != ' ' && t[s1] != '\t' && t[s1] != '\r' && t[s1] != '\n') s1++;
            std::string prog = t.substr(s0, s1 - s0);
            size_t sl = prog.find_last_of("\\:");
            if (sl != std::string::npos) prog = prog.substr(sl + 1);
            if (!prog.empty() && prog != "COMMAND.COM" && !find_ci(dir, prog).empty()) return prog;
        }
    }
    if (!find_ci(dir, "AUTOEXEC.BAT").empty()) return "AUTOEXEC.BAT";
    std::vector<std::string> bats, exes;
    for (auto& de : fs::directory_iterator(P(dir), ec)) {
        if (!de.is_regular_file(ec)) continue;
        std::string n = upper(U8(de.path().filename()));
        size_t d = n.rfind('.');
        if (d == std::string::npos) continue;
        std::string ext = n.substr(d), base = n.substr(0, d);
        if (base.compare(0, 10, "PC98PLAYER") == 0 || base.find("INST") != std::string::npos || base == "AUTOEXEC" || base == "SETUP" || base == "CONFIG") continue;
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
      << "; （FloppyDisk= は FloppyImage= と同じ。どちらで書いてもよい）" << nl
      << "FloppyDisk=" << nl << "FloppyDrive=B" << nl
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
      << "; 1 にするとマウスを一切捕まえない（マウスを使わないソフト向け）" << nl
      << "; 2 にすると捕まえずに、カーソルがゲームの画面の上にある間だけマウスを送る（メモリエディタなど、ほかの窓と行き来するとき向け）" << nl
      << "MouseLockDisable=0" << nl
      << "; ゲームパッド: 音源ボードのジョイスティック端子（ATARI 仕様: 上下左右＋トリガ A・B）にあてる。0 で使わない" << nl << "Joystick=1" << nl
      << "; 方向の行き先: JOY（ジョイスティック）/ CURSOR（カーソルキー）/ TENKEY（テンキー）。JOY+CURSOR のように重ねられる" << nl
      << "JoyDirection=JOY" << nl
      << "; ボタンの割り当て: A / B（トリガ）, RAPIDA / RAPIDB（連射）, キー名（RETURN SPACE ESC KEYA F1 NUM5 など）, NONE" << nl
      << "; A+RETURN のように重ねられる。変えたら F11 → R（プログラム再起動）で読み直して試せる" << nl
      << "JoyButton1=A" << nl << "JoyButton2=B" << nl << "JoyButton3=SPACE" << nl << "JoyButton4=RETURN" << nl;
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

// ---- ゲームのフォルダを決める ------------------------------------------------------
static fs::path norm(const fs::path& p) {
    std::error_code ec;
    fs::path r = fs::weakly_canonical(fs::absolute(p, ec), ec);
    return r.empty() ? p : r;
}
static bool has_game(const fs::path& d) {
    std::error_code ec;
    return fs::is_directory(d, ec) && (fs::exists(d / "PC98PLAYER.INI", ec) || !guess_start(U8(d)).empty());
}
// フォルダ → そのフォルダ、*.INI → その INI、ほかのファイル → それがあるフォルダ
static void take_path(const fs::path& a, fs::path* dir, fs::path* ini) {
    std::error_code ec;
    if (fs::is_directory(a, ec)) { *dir = a; *ini = a / "PC98PLAYER.INI"; }
    else if (upper(U8(a.extension())) == ".INI") { *ini = a; *dir = a.parent_path(); }
    else { *dir = a.parent_path(); *ini = *dir / "PC98PLAYER.INI"; }
}
// ハードディスクイメージの中身を、イメージの横の同じ名前のフォルダへ展開して、そこをゲームのフォルダにする
//  戻り値: 1 = 展開した（*dir に展開先）, 0 = やめた, -1 = 失敗
static int extract_hd_image(const fs::path& image, fs::path* dir) {
    std::error_code ec;
    fs::path dest = image.parent_path() / image.stem();
    hdimage::Info info; std::string err;
    if (!hdimage::probe(U8(image), &info, &err)) {
        message(SDL_MESSAGEBOX_ERROR, "ハードディスクイメージを読めません:\n" + U8(image) + "\n\n" + err);
        return -1;
    }
    if (!ask_yes_no("インストールアシスタント - ハードディスクイメージの展開",
                    "「" + U8(image.filename()) + "」（" + info.format + "・MS-DOS の区画 " + std::to_string(info.parts.size()) +
                    " 個）の中身を、次のフォルダへ展開します。\n\n" + U8(dest) +
                    "\n\n同じ名前のファイルが既にあるときは上書きしません。よろしいですか？")) return 0;
    fs::create_directories(dest, ec);
    hdimage::Result res;
    if (!hdimage::extract(U8(image), U8(dest), &res, &err)) {
        message(SDL_MESSAGEBOX_ERROR, "展開できませんでした: " + err);
        return -1;
    }
    // 飛ばしたファイル・読めなかったもの・区画が複数あるときだけ、詳しい結果を見せる
    if (res.skipped || res.errors || info.parts.size() > 1) {
        char sum[160];
        snprintf(sum, sizeof(sum), "展開しました: ファイル %d 個・フォルダ %d 個（%.1f MB）", res.files, res.dirs, res.bytes / 1048576.0);
        std::string msg = sum;
        if (res.skipped) msg += "\n既にあったので飛ばしたファイル: " + std::to_string(res.skipped) + " 個";
        if (res.errors) msg += "\n読めなかったもの: " + std::to_string(res.errors) + " 個";
        msg += "\n";
        for (size_t i = 0; i < res.notes.size() && i < 12; i++) msg += "\n・" + res.notes[i];
        message(res.errors ? SDL_MESSAGEBOX_WARNING : SDL_MESSAGEBOX_INFORMATION, msg);
    }
    // INI が無ければ AUTOEXEC.BAT（あれば）で作る。それ以外は起動時のひな形作りに任せる
    if (!fs::exists(dest / "PC98PLAYER.INI", ec) && fs::is_regular_file(dest / "AUTOEXEC.BAT", ec))
        write_template_ini(U8(dest / "PC98PLAYER.INI"), "AUTOEXEC.BAT");
    *dir = dest;
    return 1;
}
// ---- ブートモード（DOS 以前の、IPL から起動するフロッピー） ---------------------------
//  渡されたフロッピーイメージに MS-DOS のファイル表が 1 枚も無く、起動用のセクタ（IPL）を持つものが
//  あれば、「フロッピーから起動する」モードで動かす（win32 版と同じ判断）。
struct BootPlan { bool on = false; std::string disk1, disk2; };
static BootPlan g_boot_override;   // 既にある INI（Boot= の無いもの）を使うときに、上から当てる
static bool plan_boot(const std::vector<std::string>& imgs, BootPlan* bp) {
    int boot = -1;
    for (size_t i = 0; i < imgs.size(); i++) {
        bool dos = false, bt = false;
        if (!floppy::probe_image(imgs[i], &dos, &bt)) return false;
        if (dos) return false;
        if (bt && boot < 0) boot = (int)i;
    }
    if (boot < 0) return false;
    bp->on = true;
    bp->disk1 = imgs[boot];
    bp->disk2.clear();
    for (size_t i = 0; i < imgs.size(); i++) if ((int)i != boot) { bp->disk2 = imgs[i]; break; }
    return true;
}
// INI に書く名前（INI と同じフォルダならファイル名だけ）
static std::string rel_to(const fs::path& dir, const std::string& p) {
    fs::path pp = P(p);
    return pp.parent_path() == dir ? U8(pp.filename()) : p;
}
static void write_boot_ini(const fs::path& path, const fs::path& dir, const BootPlan& bp) {
    const char* nl = "\r\n";
    std::ostringstream t;
    t << "; PC98PLAYER.INI  -- フロッピーから直接起動するゲーム（MS-DOS を使わない独自形式のディスク）" << nl
      << "[PC98PLAYER]" << nl
      << "; FD = MS-DOS を使わず、1 台目のドライブのフロッピーの IPL から起動する" << nl << "Boot=FD" << nl
      << "; 1 台目・2 台目のドライブに入れるディスク。ゲーム中の入れ替えは F11 の画面で（D キーでドライブを切り替え）" << nl
      << "FloppyDisk=" << rel_to(dir, bp.disk1) << nl
      << "FloppyDisk2=" << (bp.disk2.empty() ? "" : rel_to(dir, bp.disk2)) << nl
      << "; ウインドウの表示倍率（1 で 640x400）" << nl << "Scale=2" << nl
      << "; 仮想 CPU の速さ（MHz 相当）。昔のソフトは 8 や 10 くらいが当時の速さ" << nl << "CpuMHz=10" << nl
      << "; 音源ボード（86 / 26 / 0=なし）" << nl << "SoundBoard=26" << nl
      << "; ウインドウのタイトル（空ならフォルダ名）" << nl << "Title=" << nl
      << "; 1 にすると、入れるフロッピーを必ず書き込み禁止にする（ゲームのセーブもできなくなる）" << nl << "FloppyWriteProtect=0" << nl;
    std::error_code ec;
    if (fs::exists(path, ec)) return;
    std::ofstream o(path, std::ios::binary);
    o << t.str();
}
// フロッピーイメージ（1 枚以上）を渡されたとき。ブートモードで動かすなら true（*dir, *ini を決める）
static bool take_boot_images(const std::vector<std::string>& imgs0, fs::path* dir, fs::path* ini) {
    std::vector<std::string> imgs = imgs0;
    std::sort(imgs.begin(), imgs.end(), [](const std::string& x, const std::string& y) { return upper(x) < upper(y); });
    BootPlan bp;
    if (!plan_boot(imgs, &bp)) return false;
    std::error_code ec;
    *dir = P(bp.disk1).parent_path();
    *ini = *dir / "PC98PLAYER.INI";
    write_boot_ini(*ini, *dir, bp);
    if (!fs::exists(*ini, ec)) g_boot_override = bp;   // 書けない場所: INI 無しで動かす
    else {
        Ini chk; load_ini_any(U8(*ini), chk);
        std::string b = upper(chk.get("PC98PLAYER.BOOT", ""));
        if (!(b == "FD" || b == "FLOPPY" || b == "1")) g_boot_override = bp;   // ほかの用途の INI: 中身はそのまま、起動の仕方だけ当てる
    }
    return true;
}
// 引数やドロップで来たもの: ハードディスクイメージなら展開してから。false = 起動しない
static bool take_arg(const fs::path& a, fs::path* dir, fs::path* ini) {
    std::error_code ec;
    if (fs::is_regular_file(a, ec) && hdimage::is_hd_image(U8(a))) {
        fs::path d;
        if (extract_hd_image(a, &d) != 1) return false;
        take_path(d, dir, ini);
        return true;
    }
    // ブートモードで動くフロッピーイメージ。そうでないもの（MS-DOS のディスク）は従来どおり、それがあるフォルダ
    if (fs::is_regular_file(a, ec) && is_floppy_image(U8(a)) && take_boot_images({U8(a)}, dir, ini)) return true;
    take_path(a, dir, ini);
    return true;
}
// アプリを置いた場所。macOS の .app なら、その .app があるフォルダ
static fs::path app_dir(const fs::path& base) {
    for (fs::path p = base; p.has_relative_path(); p = p.parent_path()) {
        std::string n = U8(p.filename());
        if (n.size() > 4 && upper(n.substr(n.size() - 4)) == ".APP") return p.parent_path();
    }
    return base;
}
// 最後に開いたフォルダ（フォルダを選ぶ画面の初期位置）
static fs::path recent_file() {
    char* pp = SDL_GetPrefPath("", "PC98PLAYER");
    fs::path r = pp ? P(pp) / "recent.txt" : fs::path();
    SDL_free(pp);
    return r;
}
static std::string load_recent() {
    fs::path f = recent_file();
    if (f.empty()) return "";
    std::ifstream i(f, std::ios::binary);
    std::string s;
    std::getline(i, s);
    return s;
}
static void save_recent(const fs::path& dir) {
    fs::path f = recent_file();
    if (f.empty()) return;
    std::ofstream o(f, std::ios::binary);
    o << U8(dir) << "\n";
}
// フォルダを選ぶ画面を出して待つ（選ぶ前に .app へ落とされたものも受け付ける）
static bool pick_folder(fs::path* out) {
    std::string def = load_recent();
    SDL_PropertiesID pr = SDL_CreateProperties();
    SDL_SetStringProperty(pr, SDL_PROP_FILE_DIALOG_TITLE_STRING, "PC98PLAYER - ゲームのフォルダを選んでください");
    if (!def.empty()) SDL_SetStringProperty(pr, SDL_PROP_FILE_DIALOG_LOCATION_STRING, def.c_str());
    SDL_ShowFileDialogWithProperties(SDL_FILEDIALOG_OPENFOLDER, dialog_done, (void*)(intptr_t)DLG_FOLDER, pr);
    SDL_DestroyProperties(pr);
    SDL_Event e;
    while (SDL_WaitEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT) return false;
        if (e.type == SDL_EVENT_DROP_FILE && e.drop.data) { *out = P(e.drop.data); return true; }
        if (e.type == g_ev_dialog && e.user.code == DLG_FOLDER) {
            std::string* s = (std::string*)e.user.data1;
            if (!s) return false;
            *out = P(*s);
            delete s;
            return true;
        }
    }
    return false;
}
// 順に: 引数 → .app に落とされたもの → カレントフォルダ → アプリの隣 → 選ぶ画面
static bool find_game(int argc, char** argv, fs::path* dir, fs::path* ini) {
    std::error_code ec;
    if (argc >= 2 && argv[1][0] != '-') {
        // フロッピーイメージを何枚か渡されたら、まとめてブートモードにできるか調べる
        std::vector<std::string> imgs;
        for (int i = 1; i < argc; i++) {
            fs::path a = norm(P(argv[i]));
            if (argv[i][0] != '-' && fs::is_regular_file(a, ec) && is_floppy_image(U8(a))) imgs.push_back(U8(a));
        }
        if (imgs.size() > 1 && is_floppy_image(argv[1]) && take_boot_images(imgs, dir, ini)) return true;
        return take_arg(norm(P(argv[1])), dir, ini);
    }
#ifdef __APPLE__
    // Finder で .app にフォルダを落として起動すると、起動直後にドロップとして届く
    for (Uint64 until = SDL_GetTicks() + 300; SDL_GetTicks() < until;) {
        SDL_Event e;
        while (SDL_PollEvent(&e))
            if (e.type == SDL_EVENT_DROP_FILE && e.drop.data) {
                if (!take_arg(norm(P(e.drop.data)), dir, ini)) return false;
                save_recent(*dir);
                return true;
            }
        SDL_Delay(10);
    }
#endif
    const char* bp = SDL_GetBasePath();
    fs::path base = norm(P(bp ? bp : "."));
    fs::path cwd = norm(fs::current_path(ec));
    if (cwd != base && has_game(cwd)) { take_path(cwd, dir, ini); return true; }
    // 隔離（App Translocation）された .app の隣は読み取り専用の仮の場所なので使わない
    fs::path ad = app_dir(base);
    if (U8(ad).find("/AppTranslocation/") == std::string::npos && has_game(ad)) { take_path(ad, dir, ini); return true; }
    fs::path picked;
    if (!pick_folder(&picked)) return false;
    if (!take_arg(norm(picked), dir, ini)) return false;
    save_recent(*dir);
    return true;
}

// ---- INI → 設定（起動時とプログラム再起動のとき） ------------------------------------
static fs::path    g_ini_path;       // 読んだ INI（無い = 読み直さない）
static fs::path    g_base_dir;       // Root= を当てる前のゲームのフォルダ
static std::string g_ini_floppy;     // INI の FloppyDisk= / FloppyImage=（読み直しで変わったときだけ入れ替える）
static std::string g_ini_floppy2;    // 同じく FloppyDisk2=
static bool        g_ini_memedit = false;   // MemoryEditor=1
static bool        g_ini_codeedit = false;  // CodeEditor=1

static void apply_boot_override(PlayerSettings* ps) {
    if (!g_boot_override.on) return;
    ps->cfg.boot_fd = true;
    ps->cfg.floppy_image = g_boot_override.disk1;
    ps->cfg.floppy_image2 = g_boot_override.disk2;
}
// Root= を当ててゲームのフォルダを決め、PlayerSettings を作る（Start= が空なら推測）
static void resolve_settings(const Ini& ini, PlayerSettings* ps, fs::path* game_dir) {
    std::error_code ec;
    fs::path dir = g_base_dir;
    std::string rt = ini.get("PC98PLAYER.ROOT", "");   // Root=: ゲームのフォルダを INI とは別の場所にする（INI からの相対パス可）
    if (!rt.empty()) {
        fs::path r = P(rt);
        fs::path cand = norm(r.is_absolute() ? r : g_ini_path.parent_path() / r);
        if (fs::is_directory(cand, ec)) dir = cand;
        else message(SDL_MESSAGEBOX_WARNING, "Root= のフォルダが見つかりません:\n" + U8(cand) + "\n\nINI のあるフォルダで起動します。");
    }
    player_settings_from_ini(ini, U8(dir), ps);
    apply_boot_override(ps);
    if (ps->cfg.start.empty() && !ps->cfg.boot_fd) ps->cfg.start = guess_start(U8(dir));
    *game_dir = dir;
}
// ホスト側（マウス・音・ゲームパッド）の設定。読めなかったゲームパッドの割り当てを返す
static std::string apply_host_settings(const Ini& ini) {
    g_mouse_speed = ini.geti("PC98PLAYER.MOUSESPEED", 100);
    g_pause_inactive = ini.geti("PC98PLAYER.PAUSEINACTIVE", 0) != 0;
    g_middle_release = ini.geti("PC98PLAYER.MIDDLERELEASE", 1) != 0;
    {
        int md = ini.geti("PC98PLAYER.MOUSELOCKDISABLE", 0);
        g_mouse_lock_disable = md == 1;   // 1: マウスを使わない
        g_mouse_shared = md == 2;         // 2: 捕まえずに、画面の上にある間だけ送る
        g_shared_in = false;
    }
    if ((g_mouse_lock_disable || g_mouse_shared) && g_captured) set_capture(false);
    if (g_win) update_title();
    g_audio_frames = std::max(1, std::min(30, ini.geti("PC98PLAYER.AUDIOFRAMES", 4)));
    g_ini_memedit = ini.geti("PC98PLAYER.MEMORYEDITOR", 0) != 0;
    g_ini_codeedit = ini.geti("PC98PLAYER.CODEEDITOR", 0) != 0;
    pad::s_notice = show_toast;
    std::string bad;
    pad::configure(ini, g_p ? g_p->m : nullptr, &bad);
    while (!bad.empty() && bad.back() == '\n') bad.pop_back();
    for (auto& c : bad) if (c == '\n') c = ' ';
    return bad;
}
static bool reload_settings(PlayerSettings* out, std::string* err) {
    if (g_ini_path.empty()) return false;
    Ini ini;
    if (!load_ini_any(U8(g_ini_path), ini)) { *err = "INI を読めなかったので、今の設定のまま起動し直しました"; return false; }
    PlayerSettings ps;
    fs::path dir;
    resolve_settings(ini, &ps, &dir);
    if (ps.cfg.start.empty() && !ps.cfg.boot_fd) { *err = "INI の Start= が空なので、今の設定のまま起動し直しました"; return false; }
    // フロッピー: INI の FloppyDisk= が変わっていなければ、今入っているディスクのまま
    if (ps.cfg.floppy_image == g_ini_floppy) ps.cfg.floppy_image = floppy::current_path();
    else g_ini_floppy = ps.cfg.floppy_image;
    if (ps.cfg.floppy_image2 == g_ini_floppy2) ps.cfg.floppy_image2 = floppy::current_path_unit(1);
    else g_ini_floppy2 = ps.cfg.floppy_image2;
    if (ps.cfg.trace && !getenv("PC98PLAYER_LOG")) set_log_env(hostfs::join(ps.cfg.root, "PC98PLAYER.LOG"));
    std::string bad = apply_host_settings(ini);
    if (!bad.empty()) *err = "INI のゲームパッドの割り当てが読めません: " + bad;
    *out = ps;
    return true;
}
static void set_title_from(const PlayerSettings& ps) {
    g_title = (ps.title.empty() ? base_name(ps.cfg.root) : ps.title) + " - PC98PLAYER";
}
// 起動し直したあと、窓や音の出口など、本体の外の設定を新しい INI に合わせる
static void apply_live_settings(const PlayerSettings& o) {
    const PlayerSettings& n = g_p->ps;
    set_title_from(n);
    if (n.sample_rate != o.sample_rate) { if (g_audio) SDL_DestroyAudioStream(g_audio); g_audio = nullptr; audio_open(n.sample_rate); }
    if (n.cfg.midi != o.cfg.midi || n.midi_device != o.midi_device) {
        midi_close();
        g_p->midi_sink = nullptr;
        if (n.cfg.midi && !midi_open(n.midi_device)) show_toast("MIDI の出力を開けませんでした");
    }
    if (n.font_name != o.font_name || n.font_shift != o.font_shift) {
        std::string ferr;
        if (!hostfont_open(n.font_name, &ferr)) show_toast(ferr);
        hostfont_install(n.font_shift);
    }
    bool full = (SDL_GetWindowFlags(g_win) & SDL_WINDOW_FULLSCREEN) != 0;
    if (n.fullscreen != full) toggle_fullscreen();
    else if (n.scale != o.scale && !full) SDL_SetWindowSize(g_win, 640 * n.scale, 400 * n.scale);
    SDL_SetTextureScaleMode(g_tex, n.smooth ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
    if (g_ini_memedit && !memedit::visible()) { memedit::show(); SDL_RaiseWindow(g_win); }
    if (g_ini_codeedit && !codeedit::visible()) { codeedit::show(); SDL_RaiseWindow(g_win); }
}
// ブートモードなど: 入れ替えの一覧 = ゲームのフォルダにあるフロッピーイメージ（名前順）と、いま入っているもの
static void build_disk_list() {
    std::error_code ec;
    g_disk_list.clear();
    for (auto& de : fs::directory_iterator(P(g_p->ps.cfg.root), ec))
        if (de.is_regular_file(ec) && is_floppy_image(U8(de.path()))) g_disk_list.push_back(U8(de.path()));
    std::sort(g_disk_list.begin(), g_disk_list.end(), [](const std::string& x, const std::string& y) { return upper(x) < upper(y); });
    for (auto& a : {floppy::current_path_unit(0), floppy::current_path_unit(1)}) {   // フォルダの外のイメージも一覧へ
        if (a.empty() || floppy::is_device_spec(a)) continue;
        bool have = false;
        for (auto& x : g_disk_list) if (same_path(x, a)) have = true;
        if (!have) g_disk_list.push_back(a);
    }
}

int main(int argc, char** argv) {
    SDL_SetHint(SDL_HINT_APP_NAME, "PC98PLAYER");
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "512");
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    g_ev_dialog = SDL_RegisterEvents(1);
    std::error_code ec;
    fs::path dir, ini_path;
    if (!find_game(argc, argv, &dir, &ini_path)) { SDL_Quit(); return 0; }
    if (!fs::exists(ini_path, ec) && !g_boot_override.on) {
        std::string st = guess_start(U8(dir));
        write_template_ini(U8(ini_path), st);
        message(SDL_MESSAGEBOX_INFORMATION, "PC98PLAYER.INI が無かったので、ひな形を作りました。\n\n" +
                (st.empty() ? std::string("起動するファイルが見つかりません。INI の Start= に書いてから、もう一度起動してください。")
                            : "Start=" + st + " で起動します。違う場合は INI を書き換えてください。"));
        if (st.empty()) return 1;
    }
    Ini ini;
    if (fs::exists(ini_path, ec)) { load_ini_any(U8(ini_path), ini); g_ini_path = ini_path; }
    g_base_dir = dir;
    PlayerSettings ps;
    resolve_settings(ini, &ps, &dir);
    g_ini_floppy = ps.cfg.floppy_image;
    g_ini_floppy2 = ps.cfg.floppy_image2;
    if (ps.cfg.start.empty() && !ps.cfg.boot_fd) {
        message(SDL_MESSAGEBOX_ERROR, "PC98PLAYER.INI の Start= が空です。最初に実行するファイル名を書いてください。\n\n読んだ INI: " +
                U8(ini_path) + "\nゲームのフォルダ: " + U8(dir));
        return 1;
    }
    if (ps.cfg.trace && !getenv("PC98PLAYER_LOG")) set_log_env(hostfs::join(ps.cfg.root, "PC98PLAYER.LOG"));
    std::string startup_note = apply_host_settings(ini);
    if (!startup_note.empty()) startup_note = "INI のゲームパッドの割り当てが読めません: " + startup_note;

    std::string ferr;
    bool font_ok = hostfont_open(ps.font_name, &ferr);
    hostfont_install(ps.font_shift);

    set_title_from(ps);

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

    g_p = new Player();
    std::string err;
    if (!g_p->init(ps, &err)) { message(SDL_MESSAGEBOX_ERROR, err); return 1; }
    if (ps.cfg.boot_fd || !ps.cfg.floppy_image2.empty()) build_disk_list();
    audio_open(ps.sample_rate);
    if (ps.cfg.midi && !midi_open(ps.midi_device)) show_toast("MIDI の出力を開けませんでした");
    if (ps.fullscreen) toggle_fullscreen();
    update_title();
    if (!font_ok) show_toast(ferr);
    if (!g_p->floppy_error.empty()) show_toast(g_p->floppy_error);
    else if (!startup_note.empty()) show_toast(startup_note);
    if (g_ini_memedit) {   // メモリエディタを別の窓で開く（本体の窓の右に並べる）
        memedit::show();
        int x = 0, y = 0, w = 0, h = 0;
        SDL_GetWindowPosition(g_win, &x, &y);
        SDL_GetWindowSize(g_win, &w, &h);
        memedit::place_beside(x + w, y);
        SDL_RaiseWindow(g_win);   // キー入力はゲームの窓へ
    }
    if (g_ini_codeedit) {   // コードエディタを別の窓で開く（本体の窓の下に並べる）
        codeedit::show();
        int x = 0, y = 0, w = 0, h = 0, top = 0;
        SDL_GetWindowPosition(g_win, &x, &y);
        SDL_GetWindowSize(g_win, &w, &h);
        SDL_GetWindowBordersSize(g_win, &top, nullptr, nullptr, nullptr);
        codeedit::place_at(x, y + h + top);
        SDL_RaiseWindow(g_win);
    }
    if (getenv("PC98PLAYER_SHOW_RENDERER")) { g_title += std::string(" [") + SDL_GetRendererName(g_ren) + "]"; update_title(); }

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
        memedit::tick();
        codeedit::refresh();
        if (g_paused_by_focus) { SDL_WaitEventTimeout(nullptr, 20); next = t; continue; }
        {   // メモリエディタを操作している間は止める（チェックが入っているとき）
            bool mp = memedit::pause_requested() || codeedit::pause_requested();
            if (mp != g_memedit_paused) { g_memedit_paused = mp; update_title(); }
            if (mp) {
                if (g_audio && SDL_GetAudioStreamQueued(g_audio) > 0) audio_flush();
                SDL_WaitEventTimeout(nullptr, 20);
                next = t;
                continue;
            }
        }
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
        if (g_captured || (g_mouse_shared && g_shared_in)) {
            int dx = (int)g_mouse_ax, dy = (int)g_mouse_ay;
            g_mouse_ax -= dx; g_mouse_ay -= dy;
            machine_mouse(g_p->m, dx, dy, g_mouse_btn);
        }
        pad::poll(g_p->m, (SDL_GetWindowFlags(g_win) & SDL_WINDOW_INPUT_FOCUS) != 0);   // ゲームパッド（窓が前にいるときだけ）
        if (g_turbo) {
            // 早送り: 画面 1 枚ぶん（約 1/60 秒）の実時間いっぱいまでフレームを進め、最後の 1 枚だけ描く。
            // 音は合成しない（捨てるだけなので）
            double t0 = now();
            for (int i = 0; i < 64; i++) {
                bool last = now() - t0 >= 1.0 / 60 || i == 63;
                codeedit::tick();   // 書き込み続けるコード（%）
                g_p->run_frame(last, false);
                if (last || g_p->m->quit) break;
            }
            if (g_audio && SDL_GetAudioStreamQueued(g_audio) > 0) audio_flush();
        } else {
            codeedit::tick();   // 書き込み続けるコード（%）
            g_p->run_frame(true);
            audio_push_frame();
        }
        next += frame_sec;
        if (t - next > 0.25) next = t;
        present();

        if (g_p->m->quit && !ended_notice) {
            ended_notice = true;
            if (g_p->m->quit == 3) {   // ROM（N88-BASIC）が要るソフト
                message(SDL_MESSAGEBOX_INFORMATION, g_p->m->status);
                running = false;
            } else if (g_p->m->quit == 2) {
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
