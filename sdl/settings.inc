// -----------------------------------------------------------------------------
//  settings.inc  --  画面＆サウンド設定の SDL 版（main.cpp から #include する）
//
//  win32/settings.inc と同じ働きを、SDL の別の窓に PC-98 の字（擬似漢字 ROM）で描いて作る。
//  F11 の画面の「S: 設定」で開く。INI の主な項目を変え、INI そのものを書き換えて保存する。
//  ・すぐ反映できる項目（音量・倍率・なめらか・マウスの速さなど）は、変えたその場で効く。
//    「キャンセル」で閉じると元に戻す。
//  ・＊の付いた項目（音源ボード・MIDI・CPU の速さ・フォントなど）は、プログラム再起動で反映する。
//    保存したときにそれらが変わっていれば、今すぐ再起動するかを尋ねる。
//  ・INI の書き換えは win32/ini_rewrite.h（値だけを置き換え、コメントや並びは残す）。文字コードは元のまま
//    （Shift_JIS か UTF-8。Shift_JIS で書けない文字が入るときだけ UTF-8 にする）。
//  操作: ↑↓/Tab で項目を移り、←→ で値を変える（チェックは Space）。数は打って直せる。
//        フォントや SoundFont の欄には、ファイルを窓に落とせば入る。Enter で保存、Esc でキャンセル。
// -----------------------------------------------------------------------------
#include "../win32/ini_rewrite.h"

namespace settingsdlg {

using memedit::C;
static const int W = 800, H = 532;
static SDL_Window*   s_win = nullptr;
static SDL_Renderer* s_ren = nullptr;
static SDL_Texture*  s_tex = nullptr;
static std::vector<uint32_t> s_px(W * H);
static bool s_dirty = true;
static Uint64 s_blink = 0;

enum Kind { K_CHECK, K_LIST, K_TEXT, K_NUM, K_VOL };
enum Apply { A_LIVE, A_RESTART, A_START };   // すぐ反映 / 再起動で反映 / 次に起動したときから
struct Opt { std::string val, label; };
struct Field {
    const char* key;
    Kind kind;
    const char* label;
    std::string def;
    Apply ap;
    std::vector<Opt> opts;
    int lo = 0, hi = 1000;
    const char* unit = "";
    // 置き場所（layout() が決める）。ラベルは (x, y)、部品は (cx, y, cw)
    int x = 0, y = 0, cx = 0, cw = 0;
    std::string init, cur;
};
static std::vector<Field> s_f;
static int s_focus = 0;               // 項目の番号。s_f.size() 以上はボタン（BT_*）
enum { BT_DEFAULT, BT_SAVE, BT_CANCEL, BT_COUNT };
static bool s_selall = false;         // 数・文字の欄: 次に打った文字で置き換える
static int  s_drag = -1;              // 音量のつまみを引いている項目
static int  s_midi_hint_y = 0;
static const int VOL_MAX = 300;       // つまみの右端（％。打てば 1000 まで）
static const int BTN_Y = H - 38, BTN_H = 28;
static const int BT_X[BT_COUNT] = {12, W - 12 - 120 - 8 - 136, W - 12 - 120};
static const int BT_W[BT_COUNT] = {124, 136, 120};
static const char* const BT_T[BT_COUNT] = {"既定値に戻す", "保存して閉じる", "キャンセル"};

static Field* find(const char* key) {
    for (auto& f : s_f) if (SDL_strcasecmp(f.key, key) == 0) return &f;
    return nullptr;
}
static int ival(const std::string& s, int def) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == 'x' || s[i] == 'X')) i++;
    if (i >= s.size()) return def;
    return atoi(s.c_str() + i);
}
static int count() { return (int)s_f.size() + BT_COUNT; }

// ---- 項目の一覧 ---------------------------------------------------------------------
static void build_fields() {
    s_f.clear();
    auto num_opts = [](std::initializer_list<int> vs, const char* suffix) {
        std::vector<Opt> o;
        for (int v : vs) o.push_back({std::to_string(v), std::to_string(v) + suffix});
        return o;
    };
    // 画面
    {
        std::vector<Opt> sc;
        for (int i = 1; i <= 6; i++) sc.push_back({std::to_string(i), std::to_string(i) + " 倍（" + std::to_string(640 * i) + "x" + std::to_string(400 * i) + "）"});
        s_f.push_back({"Scale", K_LIST, "表示倍率", "2", A_LIVE, sc});
    }
    s_f.push_back({"Smooth", K_CHECK, "拡大したときになめらかにする", "0", A_LIVE});
    s_f.push_back({"FullScreen", K_CHECK, "全画面で起動する（Alt+Enter でも切替）", "0", A_START});
    // Font=: GDI は書体名、FreeType はフォントファイルのパス（空なら既定）
    s_f.push_back({"Font", K_TEXT, "漢字のフォント", "", A_RESTART});
    s_f.push_back({"FontShift", K_CHECK, "半角文字を 1 ドット右へ寄せる（ROM 風）", "1", A_RESTART});
    s_f.push_back({"KanjiJIS", K_LIST, "漢字の字形", "78", A_RESTART, {{"78", "JIS78（当時の PC-98）"}, {"83", "JIS83"}}});
    s_f.push_back({"GDCClock", K_LIST, "GDC クロック", "2.5", A_RESTART,
                   {{"2.5", "2.5MHz（DIP SW 2-8 OFF）"}, {"5", "5MHz（DIP SW 2-8 ON）"}}});
    // 動作
    s_f.push_back({"CpuMHz", K_LIST, "CPU の速さ", "16", A_RESTART, num_opts({8, 10, 12, 16, 20, 25, 33, 40, 66, 75, 100, 133}, " MHz"), 1, 1000});
    s_f.push_back({"MouseSpeed", K_NUM, "マウスの速さ", "100", A_LIVE, {}, 10, 1000, "%"});
    s_f.push_back({"KeyRepeat", K_CHECK, "キーを押し続けたら繰り返す", "1", A_LIVE});
    s_f.push_back({"PauseInactive", K_CHECK, "ほかの窓を使っている間は一時停止する", "0", A_LIVE});
    s_f.push_back({"ExitOnEnd", K_CHECK, "ゲームが終わったら窓を閉じる", "1", A_LIVE});
    // サウンド
    s_f.push_back({"SoundBoard", K_LIST, "音源ボード", "86", A_RESTART,
                   {{"86", "PC-9801-86（OPNA・PCM）"}, {"26", "PC-9801-26K（OPN）"}, {"0", "なし"}}});
    s_f.push_back({"SoundIRQ", K_LIST, "音源の割込み", "12", A_RESTART, num_opts({3, 10, 12, 13}, "")});
    s_f.push_back({"SoundBIOS", K_CHECK, "サウンド BIOS を載せる（MIMPI など）", "0", A_RESTART});
    s_f.push_back({"Sound", K_CHECK, "FM 音源を鳴らす", "1", A_RESTART});
    s_f.push_back({"SampleRate", K_LIST, "サンプリング", "44100", A_RESTART, num_opts({22050, 44100, 48000}, " Hz")});
    // MIDI
    s_f.push_back({"MIDI", K_CHECK, "MIDI を使う（MPU-PC98II を載せる）", "1", A_RESTART});
#ifdef _WIN32
    {
        std::vector<Opt> md = {{"-1", "既定（MIDI マッパー）"}};
        UINT n = midiOutGetNumDevs();
        for (UINT i = 0; i < n && i < 32; i++) {
            MIDIOUTCAPSW caps = {};
            if (midiOutGetDevCapsW(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) continue;
            char nm[128] = {};
            WideCharToMultiByte(CP_UTF8, 0, caps.szPname, -1, nm, sizeof(nm), nullptr, nullptr);
            md.push_back({std::to_string(i), std::to_string(i) + ": " + nm});
        }
        s_f.push_back({"MidiDevice", K_LIST, "MIDI の出力先", "-1", A_LIVE, md});
    }
#endif
    s_f.push_back({"MidiSoundFont", K_TEXT, "SoundFont", "", A_RESTART});
    s_f.push_back({"MidiIRQ", K_LIST, "MIDI の割込み", "6", A_RESTART, num_opts({3, 5, 6, 10, 12, 13}, "")});
    s_f.push_back({"MidiSpeedFix", K_NUM, "MIDI の演奏速度", "100", A_LIVE, {}, 10, 1000, "%"});
    // 音量
    s_f.push_back({"Volume", K_VOL, "全体", "100", A_LIVE, {}, 0, 1000, "%"});
    s_f.push_back({"FMVolume", K_VOL, "FM 音源", "100", A_LIVE, {}, 0, 1000, "%"});
    s_f.push_back({"SSGVolume", K_VOL, "SSG（PSG）", "100", A_LIVE, {}, 0, 1000, "%"});
    s_f.push_back({"PCMVolume", K_VOL, "PCM（86 音源）", "100", A_LIVE, {}, 0, 1000, "%"});
    s_f.push_back({"BeepVolume", K_VOL, "ビープ", "50", A_LIVE, {}, 0, 1000, "%"});
}

// INI の今の値（無ければ既定）。チェックと数は読み方（YES / x2 など）をそろえる
static void read_ini_values() {
    Ini ini;
    load_ini_any(U8(g_ini_path), ini);
    for (auto& f : s_f) {
        std::string raw = ini.get("PC98PLAYER." + upper(f.key), "");
        if (raw.empty() && SDL_strcasecmp(f.key, "CpuMHz") == 0) raw = ini.get("PC98PLAYER.CPUCLOCK", "");
        Ini one; one.kv["X.K"] = raw;
        std::string v;
        if (f.kind == K_CHECK) {
            int d = atoi(f.def.c_str());
            v = (raw.empty() ? d : one.geti("X.K", d)) ? "1" : "0";
        } else if (f.kind == K_TEXT) v = raw;
        else v = raw.empty() ? f.def : std::to_string(one.geti("X.K", atoi(f.def.c_str())));
        if (SDL_strcasecmp(f.key, "Scale") == 0) { int s = ival(v, 2); v = std::to_string(s < 1 ? 1 : s > 8 ? 8 : s); }
        if (SDL_strcasecmp(f.key, "GDCClock") == 0) {   // 2.5 は数として読むと 2 になるので、INI の文字のまま見る（core/player.cpp と同じ読み方）
            std::string u = upper(raw);
            v = (u == "5" || u == "5MHZ" || u == "5.0") ? "5" : "2.5";
        }
        if (SDL_strcasecmp(f.key, "KanjiJIS") == 0) v = ival(v, 78) == 83 ? "83" : "78";
        f.init = f.cur = v;
        if (f.kind == K_LIST) {   // 一覧に無い値（INI に手で書いたもの）も選べるように足す
            bool hit = false;
            for (auto& o : f.opts) if (o.val == v) hit = true;
            if (!hit) f.opts.push_back({v, v + "（INI の値）"});
        }
    }
}

// ---- すぐ反映する項目 -----------------------------------------------------------------
static void apply_live(const Field& f) {
    if (!g_p || f.ap != A_LIVE || f.cur.empty()) return;
    PlayerSettings& ps = g_p->ps;
    int v = ival(f.cur, atoi(f.def.c_str()));
    auto clampv = [](int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; };
    std::string k = f.key;
    if (k == "Scale") {
        ps.scale = clampv(v, 1, 8);
        if (!(SDL_GetWindowFlags(g_win) & SDL_WINDOW_FULLSCREEN)) SDL_SetWindowSize(g_win, 640 * ps.scale, 400 * ps.scale);
    }
    else if (k == "Smooth") { ps.smooth = v != 0; SDL_SetTextureScaleMode(g_tex, ps.smooth ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST); }
    else if (k == "MouseSpeed") g_mouse_speed = clampv(v, 1, 1000);
    else if (k == "KeyRepeat") { ps.cfg.key_repeat = v; if (g_p->m) g_p->m->cfg.key_repeat = v; }
    else if (k == "PauseInactive") g_pause_inactive = v != 0;
    else if (k == "ExitOnEnd") ps.exit_on_end = v != 0;
    else if (k == "MidiSpeedFix") { int s = clampv(v, 10, 1000); ps.cfg.midi_speed = s; if (g_p->m) g_p->m->cfg.midi_speed = s; }
#ifdef _WIN32
    else if (k == "MidiDevice") {
        if (ps.midi_device != v) {
            ps.midi_device = v;
            // ホストの MIDI 出力を使っているときだけ、出口をつなぎ直す（SoundFont のときはそのまま）
            if (g_p->midi_sink == midi_sink) { midi_close(); if (!midi_open(v)) show_toast("MIDI の出力を開けませんでした"); }
        }
    }
#endif
    else if (k == "Volume") ps.volume = clampv(v, 0, 1000);
    else if (k == "PCMVolume") ps.pcm_volume = clampv(v, 0, 1000);
    else if (k == "BeepVolume") ps.beep_volume = clampv(v, 0, 1000);
    else if (k == "FMVolume" || k == "SSGVolume") {
        if (k == "FMVolume") ps.fm_volume = clampv(v, 0, 1000); else ps.ssg_volume = clampv(v, 0, 1000);
        if (g_p->opna) g_p->opna->set_balance(1.0 * ps.fm_volume / 100.0, 0.20 * ps.ssg_volume / 100.0);
    }
}

// 保存する値に直す（数は範囲に丸める）。空なら既定
static std::string final_value(const Field& f) {
    if (f.kind == K_NUM || f.kind == K_VOL || (f.kind == K_LIST && SDL_strcasecmp(f.key, "CpuMHz") == 0)) {
        int v = ival(f.cur, atoi(f.def.c_str()));
        v = v < f.lo ? f.lo : v > f.hi ? f.hi : v;
        return std::to_string(v);
    }
    if (f.cur.empty()) return f.def;
    return f.cur;
}
static bool any_changed() {
    for (auto& f : s_f) if (final_value(f) != f.init) return true;
    return false;
}

// ---- INI に書く ---------------------------------------------------------------------
// ini_rewrite.h は wstring で扱うので、UTF-8 と行き来する（wchar_t は Windows で 16 ビット、ほかは 32 ビット）
static std::wstring widen(const std::string& s) {
    std::wstring w;
    for (size_t i = 0; i < s.size();) {
        unsigned char b = (unsigned char)s[i];
        int n = b < 0x80 ? 0 : (b & 0xE0) == 0xC0 ? 1 : (b & 0xF0) == 0xE0 ? 2 : 3;
        uint32_t cp = n == 0 ? b : n == 1 ? (b & 0x1F) : n == 2 ? (b & 0x0F) : (b & 0x07);
        for (int k = 1; k <= n && i + k < s.size(); k++) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        i += (size_t)n + 1;
        if (sizeof(wchar_t) == 2 && cp >= 0x10000) { cp -= 0x10000; w += (wchar_t)(0xD800 + (cp >> 10)); w += (wchar_t)(0xDC00 + (cp & 0x3FF)); }
        else w += (wchar_t)cp;
    }
    return w;
}
static std::string narrow(const std::wstring& w) {
    std::string s;
    for (size_t i = 0; i < w.size(); i++) {
        uint32_t cp = (uint32_t)w[i];
        if (sizeof(wchar_t) == 2 && cp >= 0xD800 && cp < 0xDC00 && i + 1 < w.size()) cp = 0x10000 + ((cp - 0xD800) << 10) + ((uint32_t)w[++i] - 0xDC00);
        if (cp < 0x80) s += (char)cp;
        else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
        else { s += (char)(0xF0 | (cp >> 18)); s += (char)(0x80 | ((cp >> 12) & 0x3F)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
    }
    return s;
}
static bool write_ini(const std::vector<std::pair<std::string, std::string>>& sets, std::string* err) {
    std::string data;
    {
        std::ifstream f(g_ini_path, std::ios::binary);
        if (f) data.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }
    bool bom = data.size() >= 3 && (uint8_t)data[0] == 0xEF && (uint8_t)data[1] == 0xBB && (uint8_t)data[2] == 0xBF;
    if (bom) data.erase(0, 3);
    bool ascii = true;
    for (char c : data) if ((uint8_t)c >= 0x80) { ascii = false; break; }
    bool utf8 = bom || (!ascii && is_utf8(data));
    std::vector<std::pair<std::wstring, std::wstring>> wsets;
    for (auto& s : sets) wsets.push_back({widen(s.first), widen(s.second)});
    std::string out = narrow(inirw::rewrite(widen(utf8 ? data : hostfs::from_sjis(data)), wsets));
    std::string bytes;
    if (!utf8) {   // Shift_JIS で書けない文字があれば UTF-8（BOM 付き）にする
        bytes = hostfs::to_sjis(out);
        if (hostfs::from_sjis(bytes) != out) { utf8 = true; bom = true; }
    }
    if (utf8) bytes = (bom ? "\xEF\xBB\xBF" : "") + out;
    std::error_code ec;
    fs::path tmp = g_ini_path; tmp += ".tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        if (!o) { *err = "INI のあるフォルダに書き込めません（読み取り専用の場所かもしれません）"; return false; }
        o.write(bytes.data(), (std::streamsize)bytes.size());
        if (!o) { o.close(); fs::remove(tmp, ec); *err = "INI を書き換えられません（空き容量を確かめてください）"; return false; }
    }
    fs::rename(tmp, g_ini_path, ec);
    if (ec) { fs::remove(tmp, ec); *err = "INI を書き換えられません（読み取り専用になっていないか確かめてください）"; return false; }
    return true;
}

// ---- 窓を閉じる・保存する -------------------------------------------------------------
static bool visible() { return s_win && !(SDL_GetWindowFlags(s_win) & SDL_WINDOW_HIDDEN); }
static void close_wnd() {
    if (!s_win) return;
    SDL_HideWindow(s_win);
    s_drag = -1;
    SDL_CaptureMouse(false);
    SDL_RaiseWindow(g_win);
}
// やめる: すぐ反映した項目を元に戻す
static void revert_and_close() {
    for (auto& f : s_f) if (f.ap == A_LIVE && f.cur != f.init) { f.cur = f.init; apply_live(f); }
    close_wnd();
}
static void save_and_close() {
    std::vector<std::pair<std::string, std::string>> sets;
    std::string restart_list;
    for (auto& f : s_f) {
        std::string v = final_value(f);
        if (v == f.init) continue;
        sets.push_back({f.key, v});
        if (f.ap == A_RESTART) restart_list += std::string("\n・") + f.label;
    }
    if (sets.empty()) { close_wnd(); show_toast("設定は変わっていません"); return; }
    std::string err;
    if (!write_ini(sets, &err)) { SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "画面＆サウンド設定", err.c_str(), s_win); return; }
    for (auto& f : s_f) { f.cur = final_value(f); apply_live(f); f.init = f.cur; }
    if (!restart_list.empty()) {
        bool yes = ask_yes_no("画面＆サウンド設定 - プログラム再起動",
                              "INI に保存しました。\n\n次の項目は、プログラム再起動で反映されます:" + restart_list +
                              "\n\n今すぐプログラムを再起動しますか？\n（ゲームでセーブしていない進行は失われます。ステートセーブは残ります）",
                              true, s_win);
        close_wnd();
        if (yes) do_reboot();
        else show_toast("INI に保存しました（＊の項目は F11 → R の再起動から反映）");
    } else {
        close_wnd();
        show_toast("INI に保存しました");
    }
}
static void set_defaults() {
    for (auto& f : s_f) { f.cur = f.def; apply_live(f); }
    s_selall = false;
}
// 窓の × : 変わっていれば保存するか尋ねる
static void on_close_request() {
    if (!any_changed()) { revert_and_close(); return; }
    const SDL_MessageBoxButtonData bt[3] = {
        {(Uint32)SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "はい"},
        {0, 2, "いいえ"},
        {(Uint32)SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "キャンセル"},
    };
    SDL_MessageBoxData d = {SDL_MESSAGEBOX_INFORMATION | SDL_MESSAGEBOX_BUTTONS_LEFT_TO_RIGHT, s_win, "画面＆サウンド設定",
                            "変更を INI に保存しますか？", 3, bt, nullptr};
    int id = 0;
    if (!SDL_ShowMessageBox(&d, &id)) return;
    if (id == 1) save_and_close();
    else if (id == 2) revert_and_close();
}

// ---- 値を変える ---------------------------------------------------------------------
static void changed(Field& f) { apply_live(f); s_dirty = true; }
static void list_step(Field& f, int d) {
    if (f.opts.empty()) return;
    int sel = 0;
    for (size_t i = 0; i < f.opts.size(); i++) if (f.opts[i].val == f.cur) sel = (int)i;
    sel = (sel + d + (int)f.opts.size()) % (int)f.opts.size();
    f.cur = f.opts[sel].val;
    changed(f);
}
static void num_step(Field& f, int d) {
    int v = ival(f.cur, atoi(f.def.c_str())) + d;
    int hi = f.kind == K_VOL ? std::max(VOL_MAX, f.hi) : f.hi;
    v = v < f.lo ? f.lo : v > hi ? hi : v;
    f.cur = std::to_string(v);
    s_selall = false;
    changed(f);
}
static void toggle(Field& f) { f.cur = f.cur == "1" ? "0" : "1"; changed(f); }
// ←→: 値を一つ動かす
static void step(Field& f, int d) {
    if (f.kind == K_CHECK) toggle(f);
    else if (f.kind == K_LIST) list_step(f, d);
    else if (f.kind == K_NUM) num_step(f, d * 10);
    else if (f.kind == K_VOL) num_step(f, d * 5);
}
static void type_char(Field& f, char c) {
    if ((f.kind == K_NUM || f.kind == K_VOL) && !(c >= '0' && c <= '9')) return;
    if (f.kind != K_NUM && f.kind != K_VOL && f.kind != K_TEXT) return;
    if (s_selall) { f.cur.clear(); s_selall = false; }
    if (f.cur.size() < 260) f.cur += c;
    changed(f);
}
static void backspace(Field& f) {
    if (s_selall) { f.cur.clear(); s_selall = false; }
    else {
        while (!f.cur.empty() && ((unsigned char)f.cur.back() & 0xC0) == 0x80) f.cur.pop_back();   // UTF-8 の 1 文字ぶん
        if (!f.cur.empty()) f.cur.pop_back();
    }
    changed(f);
}
static void focus(int i) {
    s_focus = (i + count()) % count();
    s_selall = true;
    s_blink = SDL_GetTicks();
}
static void press_button(int b) {
    if (b == BT_SAVE) save_and_close();
    else if (b == BT_CANCEL) revert_and_close();
    else if (b == BT_DEFAULT) set_defaults();
}

// ---- 置き場所と描く -------------------------------------------------------------------
static const int LX = 12, RX = 408, GW = 380, R = 24;
struct Group { const char* title; int x, y, w, h; };
static std::vector<Group> s_groups;
static void layout() {
    s_groups.clear();
    auto place = [&](const char* key, int x, int y, int lw, int cw) {
        Field* f = find(key);
        if (!f) return;
        f->x = x; f->y = y; f->cx = x + lw; f->cw = cw;
    };
    // 左: 画面
    int y = 6;
    s_groups.push_back({"画面", LX, y, GW, 22 + 7 * R + 4});
    const char* scr[] = {"Scale", "Smooth", "FullScreen", "Font", "FontShift", "KanjiJIS", "GDCClock"};
    for (int i = 0; i < 7; i++) place(scr[i], LX + 10, y + 22 + i * R, 140, 222);
    // 左: 動作
    y += 22 + 7 * R + 4 + 6;
    s_groups.push_back({"動作", LX, y, GW, 22 + 5 * R + 4});
    const char* act[] = {"CpuMHz", "MouseSpeed", "KeyRepeat", "PauseInactive", "ExitOnEnd"};
    for (int i = 0; i < 5; i++) place(act[i], LX + 10, y + 22 + i * R, 140, i == 1 ? 64 : 160);
    int left_end = y + 22 + 5 * R + 4;
    // 右: サウンド
    y = 6;
    s_groups.push_back({"サウンド", RX, y, GW, 22 + 5 * R + 4});
    const char* snd[] = {"SoundBoard", "SoundIRQ", "SoundBIOS", "Sound", "SampleRate"};
    for (int i = 0; i < 5; i++) place(snd[i], RX + 10, y + 22 + i * R, 140, 222);
    // 右: MIDI（見出しの下に、ゲーム側の設定についての一言）
    y += 22 + 5 * R + 4 + 6;
    const char* mid[] = {"MidiDevice", "MidiSoundFont", "MidiIRQ", "MidiSpeedFix"};
    int rows = 0;
    for (auto k : mid) if (find(k)) rows++;
    s_groups.push_back({"MIDI", RX, y, GW, 22 + R + 50 + rows * R + 4});
    place("MIDI", RX + 10, y + 22, 140, 222);
    s_midi_hint_y = y + 22 + R - 2;
    int yy = y + 22 + R + 50;
    for (auto k : mid) if (find(k)) { place(k, RX + 10, yy, 140, std::string(k) == "MidiSpeedFix" ? 64 : 222); yy += R; }
    int right_end = yy + 4;
    // 下: 音量（2 列）
    y = std::max(left_end, right_end) + 6;
    s_groups.push_back({"音量（変えるとすぐ聞こえます）", LX, y, W - 24, 22 + 3 * R + 4});
    const char* vol[] = {"Volume", "FMVolume", "SSGVolume", "PCMVolume", "BeepVolume"};
    for (int i = 0; i < 5; i++) place(vol[i], (i % 2 ? RX : LX) + 10, y + 22 + (i / 2) * R, 120, 230);
}
// 音量のつまみの範囲（部品の左側）
static const int VOL_SB_W = 160;
static int vol_pos(const Field& f) { int v = ival(f.cur, 0); return std::min(std::max(v, 0), VOL_MAX) * (VOL_SB_W - 8) / VOL_MAX; }

static std::string fit(const std::string& s, int w) {
    if (memedit::text_w(s) <= w) return s;
    std::string t = s;
    while (!t.empty() && memedit::text_w(t + "…") > w) {
        size_t k = t.size() - 1;
        while (k > 0 && ((unsigned char)t[k] & 0xC0) == 0x80) k--;
        t.resize(k);
    }
    return t + "…";
}
static void draw() {
    using namespace memedit;
    canvas(s_px, W, H);
    const uint32_t ink = C(30, 30, 40), dim = C(110, 110, 125), sel = C(60, 120, 230);
    fill(0, 0, W, H, C(236, 237, 242));
    for (auto& g : s_groups) {
        frame(g.x, g.y + 8, g.w, g.h - 8, C(170, 172, 185));
        fill(g.x + 8, g.y, text_w(g.title) + 8, 16, C(236, 237, 242));
        text(g.x + 12, g.y, C(40, 60, 140), g.title);
    }
    bool blink_on = ((SDL_GetTicks() - s_blink) / 500) % 2 == 0;
    for (size_t i = 0; i < s_f.size(); i++) {
        const Field& f = s_f[i];
        bool foc = (int)i == s_focus;
        std::string lab = f.label;
        if (f.ap == A_RESTART) lab += " ＊";
        if (f.kind == K_CHECK) {
            checkbox(f.x, f.y + 2, f.cur == "1", lab.c_str());
            if (foc) frame(f.x - 3, f.y, text_w(lab) + 24, 20, sel);
            continue;
        }
        text(f.x, f.y + 2, ink, fit(lab, f.cx - f.x - 4));
        if (f.kind == K_LIST) {
            fill(f.cx, f.y, f.cw, 20, C(255, 255, 255));
            frame(f.cx, f.y, f.cw, 20, foc ? sel : C(130, 135, 150));
            std::string v = f.cur;
            for (auto& o : f.opts) if (o.val == f.cur) v = o.label;
            text(f.cx + 4, f.y + 2, dim, "<");
            text(f.cx + f.cw - 12, f.y + 2, dim, ">");
            text(f.cx + 16, f.y + 2, C(20, 20, 30), fit(v, f.cw - 32));
        } else if (f.kind == K_TEXT || f.kind == K_NUM) {
            int fw = f.cw;
            fill(f.cx, f.y, fw, 20, C(255, 255, 255));
            frame(f.cx, f.y, fw, 20, foc ? sel : C(130, 135, 150));
            std::string t = f.cur;
            while (!t.empty() && text_w(t) > fw - 12) {   // 長いときは後ろ（ファイル名の側）を見せる
                size_t n = 1; while (n < t.size() && ((unsigned char)t[n] & 0xC0) == 0x80) n++;
                t.erase(0, n);
            }
            int tw = text_w(t);
            if (foc && s_selall && !t.empty()) fill(f.cx + 3, f.y + 2, tw + 1, 16, C(170, 200, 250));
            if (t.empty() && !foc && f.kind == K_TEXT) text(f.cx + 4, f.y + 2, dim, "（既定）");
            else text(f.cx + 4, f.y + 2, C(20, 20, 30), t);
            if (foc && blink_on) fill(f.cx + 4 + tw, f.y + 3, 1, 14, C(20, 20, 30));
            if (f.unit[0]) text(f.cx + fw + 6, f.y + 2, ink, f.unit);
        } else if (f.kind == K_VOL) {
            int sx = f.cx, sy = f.y + 6;
            fill(sx, sy + 3, VOL_SB_W, 3, C(160, 165, 180));
            int p = vol_pos(f);
            fill(sx + 100 * (VOL_SB_W - 8) / VOL_MAX + 4, sy - 2, 1, 13, C(150, 150, 165));   // 100% の印
            fill(sx + p, sy - 3, 8, 15, foc ? sel : C(90, 95, 115));
            int ex = sx + VOL_SB_W + 8, ew = 44;
            fill(ex, f.y, ew, 20, C(255, 255, 255));
            frame(ex, f.y, ew, 20, foc ? sel : C(130, 135, 150));
            int tw = text_w(f.cur);
            if (foc && s_selall && !f.cur.empty()) fill(ex + 3, f.y + 2, tw + 1, 16, C(170, 200, 250));
            text(ex + 4, f.y + 2, C(20, 20, 30), f.cur);
            if (foc && blink_on) fill(ex + 4 + tw, f.y + 3, 1, 14, C(20, 20, 30));
            text(ex + ew + 4, f.y + 2, ink, "%");
        }
    }
    if (Field* m = find("MIDI")) {
        bool on = m->cur == "1";
        text_wrap(RX + 28, s_midi_hint_y + 2, GW - 40, 3, on ? C(0, 110, 40) : C(190, 40, 20),
                  on ? "ゲーム側の音源設定も MIDI（MPU-401 / MPU-PC98）にしてください。"
                     : "OFF のままだと、ゲームで MIDI を選んでも鳴りません（MPU が無いので止まるソフトもあります）。");
    }
    text(LX + 4, BTN_Y - 26, dim, "＊ は保存後のプログラム再起動で反映（保存のときに尋ねます）。ほかはすぐ反映されます。");
    for (int b = 0; b < BT_COUNT; b++) {
        button(BT_X[b], BTN_Y, BT_W[b], BTN_H, BT_T[b]);
        if (s_focus == (int)s_f.size() + b) frame(BT_X[b] - 2, BTN_Y - 2, BT_W[b] + 4, BTN_H + 4, sel);
    }
    text(BT_X[BT_DEFAULT] + BT_W[BT_DEFAULT] + 12, BTN_Y + 6, dim, "Enter: 保存  Esc: キャンセル");
}
static void present() {
    if (!visible()) return;
    draw();
    SDL_UpdateTexture(s_tex, nullptr, s_px.data(), W * 4);
    SDL_RenderClear(s_ren);
    SDL_RenderTexture(s_ren, s_tex, nullptr, nullptr);
    SDL_RenderPresent(s_ren);
    s_dirty = false;
}

// ---- 窓 -------------------------------------------------------------------------------
static void create() {
    if (s_win) return;
    float k = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    int sc = k >= 1.75f ? 2 : 1;
    s_win = SDL_CreateWindow("画面＆サウンド設定 - PC98PLAYER", W * sc, H * sc, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN);
    if (!s_win) return;
    s_ren = SDL_CreateRenderer(s_win, nullptr);
    SDL_SetRenderLogicalPresentation(s_ren, W, H, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    SDL_SetRenderDrawColor(s_ren, 236, 237, 242, 255);
    s_tex = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, W, H);
    SDL_SetTextureScaleMode(s_tex, SDL_SCALEMODE_NEAREST);
    SDL_SetWindowMinimumSize(s_win, W / 2, H / 2);
    SDL_StartTextInput(s_win);   // 一度入れて切ると、SDL が窓から IME を外す（memedit.inc と同じ）
    SDL_StopTextInput(s_win);
}
static void open() {
    if (visible()) { SDL_RaiseWindow(s_win); return; }
    create();
    if (!s_win) return;
    build_fields();
    read_ini_values();
    layout();
    s_focus = 0; s_selall = false; s_drag = -1;
    std::string ini_name = base_name(U8(g_ini_path));
    SDL_SetWindowTitle(s_win, ("画面＆サウンド設定 - " + ini_name).c_str());
    {   // 本体の窓の真ん中へ（画面からはみ出さないように）
        int mx = 0, my = 0, mw = 0, mh = 0, w = 0, h = 0;
        SDL_GetWindowPosition(g_win, &mx, &my);
        SDL_GetWindowSize(g_win, &mw, &mh);
        SDL_GetWindowSize(s_win, &w, &h);
        int x = mx + (mw - w) / 2, y = my + (mh - h) / 2;
        SDL_Rect ub;
        if (SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(g_win), &ub)) {
            x = std::max(ub.x, std::min(x, ub.x + ub.w - w));
            y = std::max(ub.y + 30, std::min(y, ub.y + ub.h - h));
        }
        SDL_SetWindowPosition(s_win, x, y);
    }
    SDL_ShowWindow(s_win);
    SDL_RaiseWindow(s_win);
    s_dirty = true;
}
// main のループから毎回呼ぶ（変わったときと、カーソルの点滅のときだけ描く）
static void tick() {
    if (!visible() || (SDL_GetWindowFlags(s_win) & SDL_WINDOW_MINIMIZED)) return;
    static Uint64 last_phase = 0;
    Uint64 ph = (SDL_GetTicks() - s_blink) / 500;
    if (ph != last_phase) { last_phase = ph; s_dirty = true; }
    if (s_dirty) present();
}
static bool owns(const SDL_Event& e) { return s_win && SDL_GetWindowFromEvent(&e) == s_win; }

// ---- 入力 -----------------------------------------------------------------
static bool is_edit(const Field& f) { return f.kind == K_TEXT || f.kind == K_NUM || f.kind == K_VOL; }
static void on_key(const SDL_KeyboardEvent& k) {
    int n = (int)s_f.size();
    Field* f = s_focus < n ? &s_f[s_focus] : nullptr;
    bool shift = (k.mod & SDL_KMOD_SHIFT) != 0;
    if (f && is_edit(*f) && (k.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) && k.key == SDLK_A) { s_selall = true; return; }
    switch (k.key) {
    case SDLK_ESCAPE: revert_and_close(); return;
    case SDLK_RETURN: case SDLK_KP_ENTER:
        if (s_focus >= n) press_button(s_focus - n); else save_and_close();
        return;
    case SDLK_TAB: focus(s_focus + (shift ? -1 : 1)); return;
    case SDLK_UP: focus(s_focus - 1); return;
    case SDLK_DOWN: focus(s_focus + 1); return;
    case SDLK_LEFT: case SDLK_RIGHT: {
        int d = k.key == SDLK_LEFT ? -1 : 1;
        if (f && f->kind != K_TEXT) step(*f, d);
        else if (!f) focus(s_focus + d);
        return; }
    case SDLK_SPACE:
        if (f && f->kind == K_CHECK) { toggle(*f); return; }
        if (f && f->kind == K_LIST) { step(*f, 1); return; }
        if (!f) { press_button(s_focus - n); return; }
        break;
    case SDLK_BACKSPACE: if (f && is_edit(*f)) backspace(*f); return;
    case SDLK_DELETE: if (f && is_edit(*f)) { f->cur.clear(); s_selall = false; changed(*f); } return;
    default: break;
    }
    if (f && is_edit(*f)) if (char c = memedit::key_char(k)) type_char(*f, c);
}
static bool in(float x, float y, int rx, int ry, int rw, int rh) { return x >= rx && x < rx + rw && y >= ry && y < ry + rh; }
// その位置の項目（部品の上、またはチェックの文字の上）。無ければ -1
static int hit(float x, float y) {
    for (size_t i = 0; i < s_f.size(); i++) {
        const Field& f = s_f[i];
        if (f.kind == K_CHECK) { if (in(x, y, f.x - 3, f.y, memedit::text_w(f.label) + 48, 20)) return (int)i; continue; }
        int w = f.kind == K_VOL ? VOL_SB_W + 8 + 44 : f.cw;
        if (in(x, y, f.cx, f.y - 2, w, 24)) return (int)i;
    }
    return -1;
}
static void vol_drag(Field& f, float x) {
    int v = (int)((x - f.cx - 4) * VOL_MAX / (VOL_SB_W - 8) + 0.5f);
    v = std::min(std::max(v, 0), VOL_MAX);
    if (std::to_string(v) != f.cur) { f.cur = std::to_string(v); s_selall = false; changed(f); }
}
static void on_click(float x, float y, int button) {
    for (int b = 0; b < BT_COUNT; b++)
        if (in(x, y, BT_X[b], BTN_Y, BT_W[b], BTN_H)) { if (button == SDL_BUTTON_LEFT) { s_focus = (int)s_f.size() + b; press_button(b); } return; }
    int i = hit(x, y);
    if (i < 0) return;
    Field& f = s_f[i];
    bool again = s_focus == i;
    s_focus = i;
    s_blink = SDL_GetTicks();
    if (f.kind == K_CHECK) { if (button == SDL_BUTTON_LEFT) toggle(f); s_selall = false; }
    else if (f.kind == K_LIST) list_step(f, button == SDL_BUTTON_RIGHT || x < f.cx + 16 ? -1 : 1);   // 左端の < か右クリックで前へ
    else if (f.kind == K_VOL && x < f.cx + VOL_SB_W) {
        s_selall = false;
        vol_drag(f, x);
        s_drag = i;
        SDL_CaptureMouse(true);
    }
    else s_selall = !again;   // 初めて選んだ欄は、打てば置き換わる
}
static const char* ext_of(const std::string& p) { size_t d = p.rfind('.'); return d == std::string::npos ? "" : p.c_str() + d + 1; }
static void on_event(SDL_Event& e) {
    switch (e.type) {
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED: on_close_request(); return;
    case SDL_EVENT_WINDOW_FOCUS_GAINED: g_paused_by_focus = false; break;   // 設定の窓へ移ったときは止めない（音量を聞きながら変えられるように）
    case SDL_EVENT_KEY_DOWN: on_key(e.key); break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        SDL_ConvertEventToRenderCoordinates(s_ren, &e);
        on_click(e.button.x, e.button.y, e.button.button);
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if (s_drag < 0) return;
        SDL_ConvertEventToRenderCoordinates(s_ren, &e);
        vol_drag(s_f[s_drag], e.motion.x);
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (s_drag < 0) return;
        s_drag = -1;
        SDL_CaptureMouse(false);
        break;
    case SDL_EVENT_MOUSE_WHEEL: {   // 選択や数の上で回すと値が変わる
        float mx = e.wheel.mouse_x, my = e.wheel.mouse_y;
        SDL_RenderCoordinatesFromWindow(s_ren, mx, my, &mx, &my);
        int i = hit(mx, my);
        if (i < 0 || s_f[i].kind == K_CHECK || s_f[i].kind == K_TEXT) return;
        int d = e.wheel.y > 0 ? 1 : e.wheel.y < 0 ? -1 : 0;
        if (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) d = -d;
        if (s_f[i].kind == K_LIST) d = -d;   // 上へ回すと一覧の前へ
        if (d) { s_focus = i; step(s_f[i], d); }
        break; }
    case SDL_EVENT_DROP_FILE: {   // フォントや SoundFont のファイルを落とすと、その欄に入れる
        if (!e.drop.data) return;
        std::string p = e.drop.data;
        const char* x = ext_of(p);
        Field* f = nullptr;
        if (!SDL_strcasecmp(x, "sf2")) f = find("MidiSoundFont");
        else if (!SDL_strcasecmp(x, "ttf") || !SDL_strcasecmp(x, "ttc") || !SDL_strcasecmp(x, "otf") || !SDL_strcasecmp(x, "otc")) f = find("Font");
        else if (s_focus < (int)s_f.size() && s_f[s_focus].kind == K_TEXT) f = &s_f[s_focus];
        if (!f) return;
        f->cur = p;
        s_focus = (int)(f - &s_f[0]);
        s_selall = false;
        changed(*f);
        break; }
    case SDL_EVENT_WINDOW_EXPOSED: case SDL_EVENT_WINDOW_RESIZED: break;
    default: return;
    }
    s_dirty = true;
}

}  // namespace settingsdlg

// F11 の画面の「S: 設定」
static void open_settings() {
    if (g_ini_path.empty()) {   // INI を使わずに動いているとき（ブートモードで落としたイメージなど）
        g_menu_note = "INI が無いので設定を変えられません";
        return;
    }
    menu_close();
    settingsdlg::open();
}
