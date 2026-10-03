// SPDX-License-Identifier: MIT
//  codeedit.inc -- コードエディタの SDL 版（F11 の画面の「C: コードエディタ」、または INI の CodeEditor=1）
//
//  win32/codeedit.inc と同じ働きを、SDL の別の窓に PC-98 の字で描いて作る（描く道具はメモリエディタと共通）。
//  1 行 = 「番地 接続記号 値」:
//    番地     16 進（1A2B0）または セグメント:オフセット（1A2B:0000）。通常メモリ 640KB の中
//             「開始-終点」（00100-00200）なら、その範囲（両端を含む）を値の繰り返しで埋める
//    接続記号 半角スペース … 「実行」を押した瞬間に 1 回だけ書き込む
//             半角 %       … 「実行」を押してから「停止」を押すまで、毎フレーム書き込み続ける
//    値       FF4422… = 16 進のバイト列 / s99 / m999 / l99999 = 10 進の 1 / 2 / 4 バイト
//  ; から行末まではコメント。書いた内容はゲームのフォルダの PC98PLAYER.SAV/CODES.TXT に残す。
//  入力はキーから文字を取る（IME を通さない）。日本語のコメントはクリップボードから貼り付けられる（Ctrl+V）。

namespace codeedit {

using memedit::C;
static const int W = 640, H = 516;
static const int RUN_X = 8, STOP_X = 114, BTN_W = 100, PAUSE_X = 226;
static const int HELP_Y = 34, ED_X = 8, ED_Y = 104, ED_W = 624, ED_ROWS = 16;
static const int MON_LBL_Y = ED_Y + ED_ROWS * 16 + 8, MON_Y = MON_LBL_Y + 18, MON_ROWS = 5, STATUS_Y = MON_Y + MON_ROWS * 16 + 8;

static SDL_Window*   s_win = nullptr;
static SDL_Renderer* s_ren = nullptr;
static SDL_Texture*  s_tex = nullptr;
static std::vector<uint32_t> s_px(W * H);
static bool s_dirty = true;
static bool s_pause = false;
static std::string s_status;

// 文字の欄（行ごとの UTF-8）
static std::vector<std::string> s_lines{""};
static int s_row = 0;            // カーソルの行
static size_t s_col = 0;         // カーソルの位置（行の中のバイト位置。文字の切れ目）
static int s_top = 0;            // 表示している先頭の行
static int s_xoff = 0;           // 横に送っている量（ドット）
static bool s_selall = false;    // Ctrl+A（次に打つ・消すと全部を置き換える）
static int s_err_line = -1;      // 読めなかった行（赤く示す。書き換えると消える）

struct Code { uint32_t addr; uint32_t len; std::vector<uint8_t> bytes; int line; std::string src; };   // len = 書く長さ（範囲なら範囲の長さ）
static std::vector<Code> s_active;     // 「%」の行（停止を押すまで毎フレーム書く）
static std::string s_loaded_root;      // CODES.TXT を読んだゲームのフォルダ

static void set_status(const std::string& t) { s_status = t; s_dirty = true; }

// ---- 文字の欄 ---------------------------------------------------------------
static std::string get_text() {
    std::string t;
    for (size_t i = 0; i < s_lines.size(); i++) { if (i) t += "\r\n"; t += s_lines[i]; }
    return t;
}
static void set_text(const std::string& t) {
    s_lines.assign(1, "");
    for (char c : t) {
        if (c == '\r') continue;
        if (c == '\n') s_lines.push_back("");
        else s_lines.back() += c;
    }
    s_row = 0; s_col = 0; s_top = 0; s_xoff = 0; s_selall = false; s_err_line = -1;
    s_dirty = true;
}
static size_t prev_char(const std::string& s, size_t i) { if (i == 0) return 0; i--; while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--; return i; }
static size_t next_char(const std::string& s, size_t i) { if (i >= s.size()) return s.size(); i++; while (i < s.size() && ((unsigned char)s[i] & 0xC0) == 0x80) i++; return i; }
static int col_x(int row, size_t col) { return memedit::text_w(s_lines[row].substr(0, col)); }
// 行の中で x ドット目に一番近い文字の切れ目
static size_t col_at(int row, int x) {
    const std::string& s = s_lines[row];
    size_t i = 0; int w = 0;
    while (i < s.size()) { size_t n; int cw = memedit::char_w(s, i, &n); if (w + cw / 2 >= x) break; w += cw; i += n; }
    return i;
}
static void keep_visible() {
    if (s_row < s_top) s_top = s_row;
    if (s_row >= s_top + ED_ROWS) s_top = s_row - ED_ROWS + 1;
    int cx = col_x(s_row, s_col);
    if (cx - s_xoff > ED_W - 16) s_xoff = cx - (ED_W - 16);
    if (cx - s_xoff < 0) s_xoff = std::max(0, cx - 64);
}
static void clear_all_if_selected() {
    if (!s_selall) return;
    s_lines.assign(1, ""); s_row = 0; s_col = 0; s_top = 0; s_xoff = 0; s_selall = false;
}
static void insert_text(const std::string& t) {   // 改行を含んでもよい
    clear_all_if_selected();
    s_err_line = -1;
    for (char c : t) {
        if (c == '\r') continue;
        if (c == '\n') {
            std::string rest = s_lines[s_row].substr(s_col);
            s_lines[s_row].resize(s_col);
            s_lines.insert(s_lines.begin() + s_row + 1, rest);
            s_row++; s_col = 0;
        } else if ((unsigned char)c >= 0x20 || c == '\t') {
            s_lines[s_row].insert(s_col, 1, c == '\t' ? ' ' : c);
            s_col++;
        }
    }
    keep_visible();
}
static void backspace() {
    if (s_selall) { clear_all_if_selected(); return; }
    s_err_line = -1;
    if (s_col > 0) { size_t p = prev_char(s_lines[s_row], s_col); s_lines[s_row].erase(p, s_col - p); s_col = p; }
    else if (s_row > 0) { s_col = s_lines[s_row - 1].size(); s_lines[s_row - 1] += s_lines[s_row]; s_lines.erase(s_lines.begin() + s_row); s_row--; }
    keep_visible();
}
static void del() {
    if (s_selall) { clear_all_if_selected(); return; }
    s_err_line = -1;
    if (s_col < s_lines[s_row].size()) s_lines[s_row].erase(s_col, next_char(s_lines[s_row], s_col) - s_col);
    else if (s_row + 1 < (int)s_lines.size()) { s_lines[s_row] += s_lines[s_row + 1]; s_lines.erase(s_lines.begin() + s_row + 1); }
    keep_visible();
}
static void move_row(int d) {
    int x = col_x(s_row, s_col);
    s_row = std::max(0, std::min((int)s_lines.size() - 1, s_row + d));
    s_col = col_at(s_row, x);
}

// ---- CODES.TXT ---------------------------------------------------------------
static fs::path codes_path() { return g_p ? P(hostfs::join(hostfs::join(g_p->ps.cfg.root, "PC98PLAYER.SAV"), "CODES.TXT")) : fs::path(); }
static void save_text() {
    if (!g_p || !s_win) return;
    hostfs::mkdir(hostfs::join(g_p->ps.cfg.root, "PC98PLAYER.SAV"));
    std::ofstream o(codes_path(), std::ios::binary);
    o << get_text();
}
static void load_text() {
    if (!g_p) return;
    s_loaded_root = g_p->ps.cfg.root;
    std::ifstream f(codes_path(), std::ios::binary);
    if (!f) {
        set_text("; 例（; から後ろはコメント）\n"
                 "; 34784 3F420F00   ← 「実行」を押したときに 1 回だけ書く（16 進のバイト列）\n"
                 "; 34784%l999999    ← 「停止」を押すまで書き続ける（10 進 4 バイト）\n"
                 "; 00100-00200 m999 ← 範囲を E7 03 E7 03 … で埋める\n");
        return;
    }
    set_text(std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()));
}

// ---- コードを解く・書く -------------------------------------------------------
// 番地: 16 進（〜5 桁）または SSSS:OOOO
static bool parse_addr(const std::string& a, uint32_t* out) {
    auto hex = [](const std::string& s, uint32_t lim, uint32_t* v) {
        if (s.empty() || s.size() > 8) return false;
        uint32_t x = 0;
        for (char ch : s) { int d = memedit::hexd(ch); if (d < 0) return false; x = (x << 4) | (uint32_t)d; }
        if (x > lim) return false;
        *v = x; return true;
    };
    size_t c = a.find(':');
    if (c != std::string::npos) {
        uint32_t sg, of;
        if (!hex(a.substr(0, c), 0xFFFF, &sg) || !hex(a.substr(c + 1), 0xFFFF, &of)) return false;
        *out = (sg << 4) + of;
        return true;
    }
    return hex(a, 0xFFFFF, out);
}
// 1 行を解く。空行・コメントだけなら kind = 0、1 回 = 1、毎フレーム = 2
static bool parse_line(std::string line, int* kind, Code* code, std::string* err) {
    size_t sc = line.find(';');
    if (sc != std::string::npos) line = line.substr(0, sc);
    {   // 全角の空白・タブは半角の空白に
        std::string t;
        for (size_t i = 0; i < line.size(); i++) {
            if (line.compare(i, 3, "\xE3\x80\x80") == 0) { t += ' '; i += 2; }
            else t += line[i] == '\t' ? ' ' : line[i];
        }
        line = t;
    }
    while (!line.empty() && line.back() == ' ') line.pop_back();
    size_t b = line.find_first_not_of(' ');
    if (b == std::string::npos) { *kind = 0; return true; }
    line = line.substr(b);
    size_t p = line.find_first_of(" %");   // 番地の終わり = 最初のスペースか %（どちらかが接続記号）
    if (p == std::string::npos) { *err = "接続記号（半角スペースか %）と値がありません"; return false; }
    std::string a = line.substr(0, p);
    bool every = line[p] == '%';
    std::string v = line.substr(p + 1);
    size_t q = v.find_first_not_of(' ');   // 「34784 % FF」のように記号の前後に空白があってもよい
    if (!every && q != std::string::npos && v[q] == '%') { every = true; v = v.substr(q + 1); }
    q = v.find_first_not_of(' ');
    if (q == std::string::npos) { *err = "値がありません"; return false; }
    v = v.substr(q);
    uint32_t hi = 0;
    bool range = false;
    size_t dash = a.find('-');
    if (dash != std::string::npos) {   // 「開始-終点」: 範囲を値の繰り返しで埋める
        range = true;
        if (!parse_addr(a.substr(0, dash), &code->addr)) { *err = "開始の番地が読めません: " + a.substr(0, dash); return false; }
        if (!parse_addr(a.substr(dash + 1), &hi)) { *err = "終点の番地が読めません: " + a.substr(dash + 1); return false; }
        if (hi < code->addr) { *err = "終点が開始より前です: " + a; return false; }
    } else if (!parse_addr(a, &code->addr)) { *err = "番地が読めません: " + a; return false; }
    code->bytes.clear();
    char k0 = (char)tolower((unsigned char)v[0]);
    if ((k0 == 's' || k0 == 'm' || k0 == 'l') && v.size() >= 2) {
        uint32_t val; int n;
        if (!memedit::parse_value(v, &val, &n, err)) return false;
        for (int i = 0; i < n; i++) code->bytes.push_back((uint8_t)(val >> (8 * i)));
    } else if (!memedit::parse_hex_bytes(v, &code->bytes, err)) return false;
    code->len = range ? hi - code->addr + 1 : (uint32_t)code->bytes.size();
    if (code->addr + code->len > memedit::MEM_SIZE) { *err = "640KB（9FFFFh）の終わりを越えます"; return false; }
    *kind = every ? 2 : 1;
    return true;
}
static void apply(const Code& c) {
    uint8_t* m = memedit::ram();
    if (!m) return;
    for (uint32_t i = 0; i < c.len; i++) m[c.addr + i] = c.bytes[i % c.bytes.size()];
}
static void update_title() {
    if (!s_win) return;
    std::string t = "コードエディタ - PC98PLAYER";
    if (!s_active.empty()) t += "  [書き込み続けています: " + std::to_string(s_active.size()) + " 行]";
    SDL_SetWindowTitle(s_win, t.c_str());
}
static void run() {
    // 「実行」のたびに、書き込み続けていたコードはいったん全部止めて、今の内容だけで作り直す
    size_t was = s_active.size();
    s_active.clear();
    update_title();
    std::string stopped = was ? "（前に書き込み続けていた " + std::to_string(was) + " 行は止めました）" : "";
    std::vector<Code> once, every;
    for (size_t i = 0; i < s_lines.size(); i++) {
        int kind = 0; Code c; std::string err;
        if (!parse_line(s_lines[i], &kind, &c, &err)) {
            set_status(std::to_string(i + 1) + " 行目: " + err + "（何も書き込んでいません）" + stopped);
            s_err_line = (int)i; s_row = (int)i; s_col = 0; s_selall = false; keep_visible();   // その行へ
            return;
        }
        c.line = (int)i + 1;
        std::string t = s_lines[i];
        size_t sc = t.find(';'); if (sc != std::string::npos) t = t.substr(0, sc);
        size_t a = t.find_first_not_of(" \t"), z = t.find_last_not_of(" \t");
        c.src = a == std::string::npos ? "" : t.substr(a, z - a + 1);
        if (kind == 1) once.push_back(c);
        else if (kind == 2) every.push_back(c);
    }
    if (once.empty() && every.empty()) { save_text(); set_status("実行するコードがありません" + stopped); return; }
    for (auto& c : once) apply(c);
    s_active = every;
    for (auto& c : s_active) apply(c);
    save_text();
    char b[160];
    snprintf(b, sizeof(b), "1 回だけの書き込み %u 行を実行。書き込み続ける行: %u 行", (unsigned)once.size(), (unsigned)every.size());
    set_status(b + (was ? "（前の " + std::to_string(was) + " 行は止めて読み直し）" : std::string()));
    update_title();
}
static void stop() {
    if (s_active.empty()) return;
    s_active.clear();
    set_status("書き込み続けていたコードを止めました");
    update_title();
}
// 毎フレーム、エミュレーションを 1 フレーム進める前に呼ぶ
static void tick() { for (auto& c : s_active) apply(c); }

// ---- 描く -----------------------------------------------------------------
static void draw() {
    using namespace memedit;
    canvas(s_px, W, H);
    std::fill(s_px.begin(), s_px.end(), C(236, 237, 242));
    button(RUN_X, 6, BTN_W, 22, "実行");
    button(STOP_X, 6, BTN_W, 22, "停止");
    if (s_active.empty()) { fill(STOP_X + 1, 7, BTN_W - 2, 20, C(225, 228, 236)); text(STOP_X, 9, C(150, 150, 160), "停止", BTN_W); }
    checkbox(PAUSE_X, 9, s_pause, "操作中はゲームを止める");
    text_wrap(8, HELP_Y, W - 16, 4, C(60, 64, 80),
              "1 行に「番地 値」（スペース = 実行したときに 1 回）か「番地%値」（停止するまで書き続ける）。番地を「開始-終点」にすると範囲を値の繰り返しで埋める。"
              "値は FF4422（16 進のバイト列）か s99 / m999 / l99999（10 進）。; から後ろはコメント。Ctrl+Enter でも実行、Ctrl+V で貼り付け");
    // 文字の欄
    fill(ED_X, ED_Y, ED_W, ED_ROWS * 16 + 4, C(255, 255, 255));
    frame(ED_X, ED_Y, ED_W, ED_ROWS * 16 + 4, C(60, 120, 230));
    for (int r = 0; r < ED_ROWS; r++) {
        int ln = s_top + r;
        if (ln >= (int)s_lines.size()) break;
        int y = ED_Y + 2 + r * 16;
        if (s_selall) fill(ED_X + 1, y, ED_W - 2, 16, C(170, 200, 250));
        else if (ln == s_err_line) fill(ED_X + 1, y, ED_W - 2, 16, C(255, 205, 205));
        const std::string& s = s_lines[ln];
        bool comment = s.find_first_not_of(" \t") != std::string::npos && s[s.find_first_not_of(" \t")] == ';';
        // 横に送った分を飛ばして、欄に入るところまで描く
        int x = ED_X + 4 - s_xoff;
        for (size_t i = 0, n; i < s.size(); i += n) {
            int cw = char_w(s, i, &n);
            if (x + cw > ED_X + ED_W - 2) break;
            if (x >= ED_X + 2) text(x, y, comment ? C(40, 120, 60) : C(20, 20, 30), s.substr(i, n));
            x += cw;
        }
        if (ln == s_row && (SDL_GetTicks() / 500) % 2 == 0) {
            int cx = ED_X + 4 - s_xoff + col_x(ln, s_col);
            if (cx >= ED_X + 2 && cx < ED_X + ED_W - 2) fill(cx, y + 1, 1, 14, C(20, 20, 30));
        }
    }
    // 書き込み続けているコード
    text(8, MON_LBL_Y, C(30, 30, 40), "書き込み続けているコード: " + std::to_string(s_active.size()) + " 行（「停止」で全部止まる）");
    fill(ED_X, MON_Y, ED_W, MON_ROWS * 16 + 4, BG);
    frame(ED_X, MON_Y, ED_W, MON_ROWS * 16 + 4, C(130, 135, 150));
    if (s_active.empty()) text(ED_X + 4, MON_Y + 2, C(130, 140, 170), "（なし）");
    for (int i = 0; i < MON_ROWS && i < (int)s_active.size(); i++) {
        const Code& c = s_active[i];
        char b[64];
        if (i == MON_ROWS - 1 && (int)s_active.size() > MON_ROWS) snprintf(b, sizeof(b), "… ほか %d 行", (int)s_active.size() - i);
        else if (c.len == c.bytes.size()) snprintf(b, sizeof(b), "%4d 行目  ", c.line);
        else snprintf(b, sizeof(b), "%4d 行目  [%05X-%05X]  ", c.line, c.addr, c.addr + c.len - 1);
        std::string t = b;
        if (!(i == MON_ROWS - 1 && (int)s_active.size() > MON_ROWS)) t += c.src;
        text(ED_X + 4, MON_Y + 2 + i * 16, C(225, 228, 235), t);
    }
    text_wrap(8, STATUS_Y, W - 16, 2, C(30, 30, 40), s_status);
}
static void present() {
    if (!s_win || !s_dirty) return;
    s_dirty = false;
    draw();
    SDL_UpdateTexture(s_tex, nullptr, s_px.data(), W * 4);
    SDL_RenderClear(s_ren);
    SDL_RenderTexture(s_ren, s_tex, nullptr, nullptr);
    SDL_RenderPresent(s_ren);
}

// ---- 窓 -------------------------------------------------------------------
static bool visible() { return s_win && !(SDL_GetWindowFlags(s_win) & SDL_WINDOW_HIDDEN); }
static void create() {
    if (s_win) return;
    float k = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    int sc = k >= 1.75f ? 2 : 1;
    s_win = SDL_CreateWindow("コードエディタ - PC98PLAYER", W * sc, H * sc, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN);
    if (!s_win) return;
    s_ren = SDL_CreateRenderer(s_win, nullptr);
    SDL_SetRenderLogicalPresentation(s_ren, W, H, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    SDL_SetRenderDrawColor(s_ren, 236, 237, 242, 255);
    s_tex = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, W, H);
    SDL_SetTextureScaleMode(s_tex, SDL_SCALEMODE_NEAREST);
    SDL_SetWindowMinimumSize(s_win, W / 2, H / 2);
    SDL_StartTextInput(s_win);   // 一度入れて切ると、SDL が窓から IME を外す（メモリエディタと同じ）
    SDL_StopTextInput(s_win);
    load_text();
    set_status("「実行」で書き込みます");
}
static void show() {
    create();
    if (!s_win) return;
    if (g_p && g_p->ps.cfg.root != s_loaded_root) load_text();   // INI を読み直してゲームのフォルダが変わった
    SDL_ShowWindow(s_win);
    SDL_RaiseWindow(s_win);
    s_dirty = true;
}
static void place_at(int x, int y) {
    if (!s_win) return;
    SDL_Rect ub;
    int w = 0, h = 0;
    SDL_GetWindowSize(s_win, &w, &h);
    if (SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(s_win), &ub) && y + h > ub.y + ub.h) y = std::max(ub.y, ub.y + ub.h - h);
    SDL_SetWindowPosition(s_win, x, y);
}
static bool pause_requested() { return s_pause && visible() && (SDL_GetWindowFlags(s_win) & SDL_WINDOW_INPUT_FOCUS); }
// 描き直し（main のループから毎回呼ぶ。カーソルの点滅のため 0.5 秒ごとにも描く）
static Uint64 s_next_blink = 0;
static void refresh() {
    if (!visible() || (SDL_GetWindowFlags(s_win) & SDL_WINDOW_MINIMIZED)) return;
    if (SDL_GetTicks() >= s_next_blink) { s_next_blink = SDL_GetTicks() + 500; s_dirty = true; }
    present();
}
// メモリエディタの候補一覧でダブルクリック: 末尾に「番地 」を書き足し、続けて値を打てるようにする
static void append_addr(uint32_t a) {
    create();
    if (!s_win) return;
    if (g_p && g_p->ps.cfg.root != s_loaded_root) load_text();
    s_selall = false;
    std::string& last = s_lines.back();
    bool blank = last.find_first_not_of(" \t") == std::string::npos;
    char b[16]; snprintf(b, sizeof(b), "%05X ", a);
    if (blank) last = b; else s_lines.push_back(b);
    s_row = (int)s_lines.size() - 1; s_col = s_lines[s_row].size();
    keep_visible();
    set_status(std::string("番地 ") + b + "を書き足しました。続けて値を打って「実行」（Ctrl+Enter）");
    SDL_ShowWindow(s_win);
    SDL_RaiseWindow(s_win);
    s_dirty = true;
}
// プログラム再起動: 書き込み続けるコードは止める（起動の途中のメモリを書き換えて止まらないように）
static void on_reboot() {
    if (!s_active.empty()) { s_active.clear(); set_status("プログラム再起動したので、書き込み続けていたコードを止めました"); update_title(); }
}
static bool owns(const SDL_Event& e) { return s_win && SDL_GetWindowFromEvent(&e) == s_win; }

// ---- 入力 -----------------------------------------------------------------
static void on_key(const SDL_KeyboardEvent& k) {
    bool ctrl = (k.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) != 0;
    if (ctrl) {
        switch (k.key) {
        case SDLK_A: s_selall = true; return;
        case SDLK_RETURN: case SDLK_KP_ENTER: run(); return;
        case SDLK_V: if (char* t = SDL_GetClipboardText()) { insert_text(t); SDL_free(t); } return;
        case SDLK_C: SDL_SetClipboardText(s_selall ? get_text().c_str() : s_lines[s_row].c_str()); return;   // 全部（Ctrl+A のあと）か、今の行
        case SDLK_HOME: s_row = 0; s_col = 0; s_selall = false; keep_visible(); return;
        case SDLK_END: s_row = (int)s_lines.size() - 1; s_col = s_lines[s_row].size(); s_selall = false; keep_visible(); return;
        default: return;
        }
    }
    switch (k.key) {
    case SDLK_BACKSPACE: backspace(); return;
    case SDLK_DELETE: del(); return;
    case SDLK_RETURN: case SDLK_KP_ENTER: insert_text("\n"); return;
    case SDLK_LEFT:
        s_selall = false;
        if (s_col > 0) s_col = prev_char(s_lines[s_row], s_col); else if (s_row > 0) { s_row--; s_col = s_lines[s_row].size(); }
        keep_visible(); return;
    case SDLK_RIGHT:
        s_selall = false;
        if (s_col < s_lines[s_row].size()) s_col = next_char(s_lines[s_row], s_col); else if (s_row + 1 < (int)s_lines.size()) { s_row++; s_col = 0; }
        keep_visible(); return;
    case SDLK_UP: s_selall = false; move_row(-1); keep_visible(); return;
    case SDLK_DOWN: s_selall = false; move_row(1); keep_visible(); return;
    case SDLK_PAGEUP: s_selall = false; move_row(-ED_ROWS); keep_visible(); return;
    case SDLK_PAGEDOWN: s_selall = false; move_row(ED_ROWS); keep_visible(); return;
    case SDLK_HOME: s_selall = false; s_col = 0; keep_visible(); return;
    case SDLK_END: s_selall = false; s_col = s_lines[s_row].size(); keep_visible(); return;
    default: break;
    }
    if (char c = memedit::key_char(k)) insert_text(std::string(1, c));
}
static void on_click(float x, float y) {
    using memedit::in;
    if (in(x, y, RUN_X, 6, BTN_W, 22)) { run(); return; }
    if (in(x, y, STOP_X, 6, BTN_W, 22)) { stop(); return; }
    if (in(x, y, PAUSE_X, 6, 200, 22)) { s_pause = !s_pause; return; }
    if (in(x, y, ED_X, ED_Y, ED_W, ED_ROWS * 16 + 4)) {
        s_selall = false;
        s_row = std::min((int)s_lines.size() - 1, s_top + (int)(y - ED_Y - 2) / 16);
        s_col = col_at(s_row, (int)x - (ED_X + 4) + s_xoff);
        keep_visible();
    }
}
static void on_event(SDL_Event& e) {
    switch (e.type) {
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED: save_text(); SDL_HideWindow(s_win); return;   // 閉じても書き込み続ける行はそのまま（止めるのは「停止」）
    case SDL_EVENT_KEY_DOWN: on_key(e.key); break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (e.button.button != SDL_BUTTON_LEFT) return;
        SDL_ConvertEventToRenderCoordinates(s_ren, &e);
        on_click(e.button.x, e.button.y);
        break;
    case SDL_EVENT_MOUSE_WHEEL: {
        int d = (int)(e.wheel.y * 3);
        if (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) d = -d;
        s_top = std::max(0, std::min((int)s_lines.size() - 1, s_top - d));
        break; }
    case SDL_EVENT_WINDOW_EXPOSED: case SDL_EVENT_WINDOW_RESIZED: break;
    default: return;
    }
    s_dirty = true;
}

}  // namespace codeedit
