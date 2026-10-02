// -----------------------------------------------------------------------------
//  memedit.inc  --  メモリエディタの SDL 版（main.cpp から #include する）
//
//  win32/memedit.inc と同じ働きを、SDL の別の窓に PC-98 の字（擬似漢字 ROM）で描いて作る。
//  INI の MemoryEditor=1、または F11 の画面の「M: メモリエディタ」で開く。
//  ・左: 640KB（00000h〜9FFFFh）の 16 進ダンプ。0.1 秒ごとに読み直し、変わったバイトは赤で示す。
//        クリックで選び、0〜9・A〜F を打つとその場でメモリを書き換える（上位→下位の 4 ビットずつ）。
//        W で選んだ番地へのコマンド入力（s99 / m999 / l99999 = 10 進の 1 / 2 / 4 バイト書き込み）。
//  ・右: 絞り込みサーチ（Search RESET / 値で検索 / 増加・減少・不動・変動）と候補の一覧。
//        値の書き方: FF → 1 バイト、FFFF → 2 バイト …（16 進）。s99 / m999 / l99999999 は 10 進。
//  画面は 912x528 の固定の大きさで描き、窓の大きさに合わせて拡大する。
// -----------------------------------------------------------------------------

namespace memedit {

static const uint32_t MEM_SIZE = 0xA0000;     // 640KB
static const int W = 912, H = 528;             // 描く大きさ
static const int ROW_BYTES = 16;
// ダンプ
static const int DX = 8, DY = 32, ROWS = 29;               // 見出しの行 + ROWS 行
static const int SB_X = 604, SB_W = 8;                     // スクロールバー
// 右の欄
static const int PX = 620, PW = 284;
static const int RESET_Y = 32, GAMEONLY_Y = 60, INPUT_Y = 84, RADIO_Y = 110, CMP_Y = 134, STATUS_Y = 162, LIST_Y = 214;
static const int LIST_ROWS = (H - 8 - LIST_Y) / 16;
// 上の段
static const int ADDR_X = 52, ADDR_W = 80, GO_X = 138, GO_W = 48, PAUSE_X = 196, HINT_X = 404;
// コマンド入力
static const int CMD_X = 60, CMD_Y = 200, CMD_W = 480, CMD_H = 96;

static SDL_Window*   s_win = nullptr;
static SDL_Renderer* s_ren = nullptr;
static SDL_Texture*  s_tex = nullptr;
static std::vector<uint32_t> s_px(W * H);
static bool   s_dirty = true;
static Uint64 s_next_tick = 0;

static uint32_t s_top = 0;                     // ダンプの先頭行のアドレス
static uint32_t s_cur = 0;                     // 選んでいるバイト
static int s_nib = 0;                          // 0 = 上位 4 ビットを待つ, 1 = 下位
static std::vector<uint8_t> s_prev;            // 前回見たときの内容
static std::vector<uint8_t> s_age;             // 変わってからの残り表示回数（赤く示す時間。約 1 秒）
static std::vector<uint8_t> s_snap;            // 検索バッファ
static std::vector<uint32_t> s_cand;           // 候補のアドレス
static bool s_searched = false;
static int s_list_top = 0;
static int s_list_sel = -1;
static int s_size = 1;                         // 比較の大きさ（1/2/4）
static bool s_pause = false;                   // 操作中はゲームを止める
static bool s_gameonly = true;                 // ゲームのメモリだけ探す
static std::vector<DosMemBlock> s_blocks;      // 「ゲームのメモリだけ」のときの対象（Search RESET のときに決める）
static bool s_use_blocks = false;
static std::string s_status;

// 文字の入力欄
struct Field { std::string text; bool selall = false; };
static Field s_addr{"00000"}, s_input, s_cmd;
enum { F_DUMP, F_ADDR, F_INPUT, F_CMD };
static int s_focus = F_DUMP;
static bool s_cmd_open = false;
static std::string s_cmd_msg;

static bool in_blocks(uint32_t a) {
    for (auto& b : s_blocks) if (a >= b.start && a < b.start + b.len) return true;
    return false;
}
static uint8_t* ram() { return (g_p && g_p->m) ? g_p->m->ram : nullptr; }
static uint32_t rd(const uint8_t* m, uint32_t a, int n) {
    uint32_t v = 0;
    for (int i = n - 1; i >= 0; i--) v = (v << 8) | m[a + i];
    return v;
}
static void set_status(const std::string& t) { s_status = t; s_dirty = true; }
static void update_count(const std::string& what) {
    if (!s_searched) { set_status("Search RESET を押すと、メモリを検索バッファに写して始めます"); return; }
    set_status(what + " 候補 " + std::to_string(s_cand.size()) + " 件");
}
static void snap_now() {
    uint8_t* m = ram(); if (!m) return;
    s_snap.assign(m, m + MEM_SIZE);
}
static void search_reset() {
    snap_now();
    s_cand.clear();
    s_use_blocks = false;
    std::string what = "検索バッファを初期化しました（640KB 全体）。";
    if (s_gameonly && g_p && g_p->m) {
        // ゲームのメモリだけ: 実行中のプログラムと親たちのメモリブロック（シェル・常駐ドライバ・DOS の領域は除く）
        dos_game_blocks(g_p->m, s_blocks);
        uint32_t total = 0;
        for (auto& b : s_blocks) total += b.len;
        if (!s_blocks.empty()) {
            s_use_blocks = true;
            s_cand.reserve(total);
            for (auto& b : s_blocks) for (uint32_t a = b.start; a < b.start + b.len && a < MEM_SIZE; a++) s_cand.push_back(a);
            std::string names;
            for (auto& b : s_blocks) {
                std::string n = b.name.empty() ? std::string("?") : b.name;
                if (names.find(n) == std::string::npos) names += (names.empty() ? "" : "・") + n;
            }
            char b[200]; snprintf(b, sizeof(b), "（%s、%u ブロック %uKB）で初期化しました。", names.c_str(), (unsigned)s_blocks.size(), total / 1024);
            what = "ゲームのメモリ" + std::string(b);
        } else what = "動いているプログラムが見つからないので 640KB 全体で初期化しました。";
    }
    if (!s_use_blocks) { s_cand.resize(MEM_SIZE); for (uint32_t i = 0; i < MEM_SIZE; i++) s_cand[i] = i; }
    s_searched = true;
    s_list_top = 0; s_list_sel = -1;
    update_count(what);
}
// 比較: op 0 '>' 1 '<' 2 '=' 3 '!='
static void compare(int op) {
    uint8_t* m = ram(); if (!m) return;
    if (!s_searched) search_reset();
    int n = s_size;
    std::vector<uint32_t> out; out.reserve(s_cand.size());
    for (uint32_t a : s_cand) {
        if (a + (uint32_t)n > MEM_SIZE) continue;
        uint32_t cur = rd(m, a, n), old = rd(s_snap.data(), a, n);
        bool keep = op == 0 ? cur > old : op == 1 ? cur < old : op == 2 ? cur == old : cur != old;
        if (keep) out.push_back(a);
    }
    s_cand.swap(out);
    snap_now();
    s_list_top = 0; s_list_sel = -1;
    static const char* nm[] = {"「増加+」:", "「減少-」:", "「不動=」:", "「変動!=」:"};
    update_count(std::string(nm[op]) + " " + std::to_string(n) + " バイト →");
}
static int hexd(char c) { return (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1; }
// 入力欄の値: 戻り値 false = 書式の誤り
static bool parse_value(const std::string& in, uint32_t* val, int* size, std::string* err) {
    std::string t;
    for (char c : in) if (c != ' ' && c != '\t') t += c;
    if (t.empty()) { *err = "値を入力してください"; return false; }
    char p = (char)tolower((unsigned char)t[0]);
    if ((p == 's' || p == 'm' || p == 'l') && t.size() >= 2) {
        int n = p == 's' ? 1 : p == 'm' ? 2 : 4;
        uint64_t v = 0;
        for (size_t i = 1; i < t.size(); i++) {
            if (t[i] < '0' || t[i] > '9') { *err = "10 進の数字ではありません: " + t; return false; }
            v = v * 10 + (uint64_t)(t[i] - '0');
            if (v > 0xFFFFFFFFull) break;
        }
        uint64_t mx = n == 1 ? 0xFF : n == 2 ? 0xFFFF : 0xFFFFFFFFull;
        if (v > mx) { *err = std::to_string(n) + " バイトに入りません（最大 " + std::to_string(mx) + "）"; return false; }
        *val = (uint32_t)v; *size = n; return true;
    }
    if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) t = t.substr(2);
    else if (!t.empty() && t[0] == '$') t = t.substr(1);
    else if (t.size() > 1 && (t.back() == 'h' || t.back() == 'H')) t.pop_back();
    if (t.empty() || t.size() > 8) { *err = "16 進は 8 桁（4 バイト）までです"; return false; }
    uint32_t v = 0;
    for (char c : t) {
        int d = hexd(c);
        if (d < 0) { *err = "16 進の数字ではありません: " + t + "（10 進は s99・m999・l99999 のように頭に s/m/l）"; return false; }
        v = (v << 4) | (uint32_t)d;
    }
    *val = v; *size = (int)((t.size() + 1) / 2);
    return true;
}
static void value_search() {
    uint8_t* m = ram(); if (!m) return;
    uint32_t v; int n; std::string err;
    if (!parse_value(s_input.text, &v, &n, &err)) { set_status(err); return; }
    if (!s_searched) search_reset();
    std::vector<uint32_t> out; out.reserve(s_cand.size() / 4 + 16);
    for (uint32_t a : s_cand) if (a + (uint32_t)n <= MEM_SIZE && rd(m, a, n) == v) out.push_back(a);
    s_cand.swap(out);
    snap_now();
    if (n == 1 || n == 2 || n == 4) s_size = n;
    s_list_top = 0; s_list_sel = -1;
    char w[96]; snprintf(w, sizeof(w), "値 %0*Xh（%u, %d バイト）:", n * 2, v, v, n);
    update_count(w);
    s_input.selall = true;   // 次の値はそのまま打てば置き換わる
}

// ---- ダンプの位置 ----------------------------------------------------------
static void clamp_top() {
    uint32_t maxtop = MEM_SIZE - (uint32_t)ROWS * ROW_BYTES;
    if (s_top > maxtop) s_top = maxtop;
}
static void ensure_visible() {
    uint32_t row = s_cur / ROW_BYTES * ROW_BYTES;
    if (row < s_top) s_top = row;
    if (row >= s_top + (uint32_t)ROWS * ROW_BYTES) s_top = row - (uint32_t)(ROWS - 1) * ROW_BYTES;
    clamp_top();
}
static void go_to(uint32_t a) {
    if (a >= MEM_SIZE) a = MEM_SIZE - 1;
    s_cur = a; s_nib = 0;
    uint32_t row = a / ROW_BYTES * ROW_BYTES;
    s_top = row >= (uint32_t)(ROWS / 2) * ROW_BYTES ? row - (uint32_t)(ROWS / 2) * ROW_BYTES : 0;
    ensure_visible();
    s_dirty = true;
}
static void scroll_rows(int64_t d) {
    int64_t t = (int64_t)s_top + d * ROW_BYTES;
    if (t < 0) t = 0;
    s_top = (uint32_t)t;
    clamp_top();
    s_dirty = true;
}
static bool is_cand(uint32_t a) {
    if (!s_searched || s_cand.size() > 200000) return false;
    return std::binary_search(s_cand.begin(), s_cand.end(), a);
}
static void address_go() {
    std::string t = s_addr.text;
    size_t colon = t.find(':');
    auto hex = [](const std::string& s, bool* ok) {
        uint32_t v = 0; *ok = !s.empty();
        for (char c : s) {
            int d = hexd(c);
            if (d < 0) { if (c == ' ') continue; *ok = false; return 0u; }
            v = (v << 4) | (uint32_t)d;
        }
        return v;
    };
    bool ok1 = true, ok2 = true;
    uint32_t a = colon != std::string::npos ? (hex(t.substr(0, colon), &ok1) << 4) + hex(t.substr(colon + 1), &ok2) : hex(t, &ok1);
    if (!ok1 || !ok2) { set_status("番地は 12345 か 1234:5678 のように 16 進で書きます"); return; }
    if (a >= MEM_SIZE) { set_status("番地は 00000〜9FFFF（640KB）の範囲です"); return; }
    go_to(a);
    s_focus = F_DUMP;
}

// ---- コマンド入力（ダンプで W） ---------------------------------------------
struct CmdResult { bool ok; std::string msg; };
static CmdResult run_command(const std::string& line, uint32_t addr) {
    uint8_t* m = ram();
    if (!m) return {false, "メモリがありません"};
    std::string t;
    for (char c : line) if (c != ' ' && c != '\t') t += c;
    if (t.empty()) return {false, "コマンドを入力してください（例: s99, m999, l99999）"};
    char k = (char)tolower((unsigned char)t[0]);
    if ((k == 's' || k == 'm' || k == 'l') && t.size() >= 2) {
        uint32_t v; int n; std::string err;
        if (!parse_value(t, &v, &n, &err)) return {false, err};
        if (addr + (uint32_t)n > MEM_SIZE) return {false, "640KB の終わりを越えます"};
        for (int i = 0; i < n; i++) m[addr + i] = (uint8_t)(v >> (8 * i));
        std::string bytes;
        for (int i = 0; i < n; i++) { char b[4]; snprintf(b, sizeof(b), "%02X", (v >> (8 * i)) & 0xFF); bytes += (i ? " " : "") + std::string(b); }
        char b[160]; snprintf(b, sizeof(b), "%05X に %u（%0*Xh）を %d バイトで書き込みました: %s", addr, v, n * 2, v, n, bytes.c_str());
        return {true, b};
    }
    return {false, "知らないコマンドです: " + t + "（今使えるのは s99・m999・l99999 の 10 進書き込み）"};
}
static void open_command() {
    s_cmd_open = true;
    s_cmd.selall = true;
    s_cmd_msg = "Enter で送信、Esc でやめる";
    s_focus = F_CMD;
    s_dirty = true;
}
static void close_command() { s_cmd_open = false; s_focus = F_DUMP; s_dirty = true; }
static void command_exec() {
    CmdResult r = run_command(s_cmd.text, s_cur);
    set_status(r.msg);
    if (r.ok) close_command();
    else { s_cmd_msg = r.msg; s_cmd.selall = true; }
}

// ---- 描く -----------------------------------------------------------------
static inline uint32_t C(int r, int g, int b) { return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b; }
static const uint32_t BG = 0xFF14161Eu;   // RGB(20, 22, 30)
static void fill(int x, int y, int w, int h, uint32_t c) {
    for (int yy = std::max(y, 0); yy < std::min(y + h, H); yy++)
        for (int xx = std::max(x, 0); xx < std::min(x + w, W); xx++) s_px[yy * W + xx] = c;
}
static void frame(int x, int y, int w, int h, uint32_t c) { fill(x, y, w, 1, c); fill(x, y + h - 1, w, 1, c); fill(x, y, 1, h, c); fill(x + w - 1, y, 1, h, c); }
static void glyph(int x, int y, const uint8_t* g, int wbytes, uint32_t c) {
    for (int yy = 0; yy < 16; yy++)
        for (int xx = 0; xx < wbytes * 8; xx++) {
            if (!(g[yy * wbytes + (xx >> 3)] & (0x80 >> (xx & 7)))) continue;
            int px = x + xx, py = y + yy;
            if (px >= 0 && px < W && py >= 0 && py < H) s_px[py * W + px] = c;
        }
}
static void ank(int x, int y, uint8_t ch, uint32_t c) { glyph(x, y, font_get_ank(ch), 1, c); }
// 文字の幅（UTF-8 の 1 文字ずつ: ASCII と半角カナは 8、ほかは 16）
static int char_w(const std::string& s, size_t i, size_t* len) {
    unsigned char b = (unsigned char)s[i];
    *len = b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : 4;
    if (b < 0x80) return 8;
    if (*len == 3 && i + 2 < s.size()) {   // U+FF61〜U+FF9F（半角カナ）
        unsigned cp = ((b & 0x0F) << 12) | (((unsigned char)s[i + 1] & 0x3F) << 6) | ((unsigned char)s[i + 2] & 0x3F);
        if (cp >= 0xFF61 && cp <= 0xFF9F) return 8;
    }
    return 16;
}
static int text_w(const std::string& s) { int w = 0; for (size_t i = 0, n; i < s.size(); i += n) w += char_w(s, i, &n); return w; }
static void text(int x, int y, uint32_t c, const std::string& utf8, int center_w = 0) {
    if (center_w > 0) x += (center_w - text_w(utf8)) / 2;
    std::string sj = hostfs::to_sjis(utf8);
    for (size_t i = 0; i < sj.size(); i++) {
        uint8_t b = (uint8_t)sj[i];
        if (sjis_lead(b) && i + 1 < sj.size()) {
            glyph(x, y, font_get_kanji(sjis_to_jis((uint16_t)((b << 8) | (uint8_t)sj[i + 1]))), 2, c);
            x += 16; i++;
        } else { ank(x, y, b, c); x += 8; }
    }
}
// 幅 w で折り返して最大 lines 行
static void text_wrap(int x, int y, int w, int lines, uint32_t c, const std::string& s) {
    size_t i = 0;
    for (int l = 0; l < lines && i < s.size(); l++) {
        size_t st = i; int cw = 0;
        while (i < s.size()) { size_t n; int ww = char_w(s, i, &n); if (cw + ww > w) break; cw += ww; i += n; }
        text(x, y + l * 16, c, s.substr(st, i - st));
    }
}
static void button(int x, int y, int w, int h, const char* t) {
    fill(x, y, w, h, C(225, 228, 236));
    frame(x, y, w, h, C(130, 135, 150));
    text(x, y + (h - 16) / 2, C(20, 20, 30), t, w);
}
static void checkbox(int x, int y, bool on, const char* t) {
    fill(x, y + 2, 12, 12, C(250, 250, 252)); frame(x, y + 2, 12, 12, C(110, 115, 130));
    if (on) fill(x + 3, y + 5, 6, 6, C(40, 90, 200));
    text(x + 18, y, C(30, 30, 40), t);
}
static void radio(int x, int y, bool on, const char* t) {
    fill(x + 1, y + 3, 10, 10, C(250, 250, 252)); frame(x, y + 2, 12, 12, C(110, 115, 130));
    if (on) fill(x + 4, y + 6, 4, 4, C(40, 90, 200));
    text(x + 18, y, C(30, 30, 40), t);
}
static void field(int x, int y, int w, const Field& f, bool focused) {
    fill(x, y, w, 20, C(255, 255, 255));
    frame(x, y, w, 20, focused ? C(60, 120, 230) : C(130, 135, 150));
    std::string t = f.text;
    while (!t.empty() && text_w(t) > w - 12) t.erase(0, 1);
    int tw = text_w(t);
    if (f.selall && !t.empty() && focused) fill(x + 3, y + 2, tw + 1, 16, C(170, 200, 250));
    text(x + 4, y + 2, C(20, 20, 30), t);
    if (focused && (SDL_GetTicks() / 500) % 2 == 0) fill(x + 4 + tw, y + 3, 1, 14, C(20, 20, 30));
}

static void age_bytes() {   // 変わったバイトを数回ぶん赤く残す
    uint8_t* m = ram();
    if (!m) return;
    if (s_prev.size() != MEM_SIZE) { s_prev.assign(m, m + MEM_SIZE); s_age.assign(MEM_SIZE, 0); }
    for (uint32_t a = 0; a < MEM_SIZE; a++) {
        if (s_prev[a] != m[a]) { s_prev[a] = m[a]; s_age[a] = 10; }
        else if (s_age[a]) s_age[a]--;
    }
}
static void draw() {
    std::fill(s_px.begin(), s_px.end(), C(236, 237, 242));
    uint8_t* m = ram();
    // 上の段
    text(8, 8, C(30, 30, 40), "番地:");
    field(ADDR_X, 6, ADDR_W, s_addr, s_focus == F_ADDR);
    button(GO_X, 6, GO_W, 20, "移動");
    checkbox(PAUSE_X, 8, s_pause, "操作中はゲームを止める");
    text(HINT_X, 8, C(80, 85, 100), "0-9 A-F:書き換え　W:コマンド");
    // ダンプ
    int dw = SB_X + SB_W - DX + 2;
    fill(DX - 2, DY - 2, dw, (ROWS + 1) * 16 + 4, BG);
    frame(DX - 2, DY - 2, dw, (ROWS + 1) * 16 + 4, s_focus == F_DUMP ? C(60, 120, 230) : C(130, 135, 150));
    {
        std::string hd = " 番地    ";
        for (int i = 0; i < ROW_BYTES; i++) { char b[8]; snprintf(b, sizeof(b), "+%X ", i); hd += b; }
        text(DX, DY, C(130, 140, 170), hd);
    }
    for (int r = 0; r < ROWS; r++) {
        uint32_t base = s_top + (uint32_t)r * ROW_BYTES;
        if (base >= MEM_SIZE) break;
        int y = DY + (r + 1) * 16;
        char ab[16]; snprintf(ab, sizeof(ab), " %05X", base);
        text(DX, y, C(150, 200, 255), ab);
        for (int i = 0; i < ROW_BYTES; i++) {
            uint32_t a = base + (uint32_t)i;
            uint8_t v = m ? m[a] : 0;
            bool changed = m && s_age.size() == MEM_SIZE && s_age[a] != 0;
            uint32_t fg = changed ? C(255, 90, 90) : C(225, 228, 235), bk = BG;
            if (s_use_blocks && !changed && !in_blocks(a)) fg = C(95, 98, 110);   // サーチの対象外（ゲーム以外）は暗く
            if (is_cand(a)) bk = C(70, 60, 20);
            if (a == s_cur) { bk = C(80, 150, 255); fg = C(255, 255, 255); }
            int hx = DX + (9 + i * 3) * 8;
            fill(hx, y, 16, 16, bk);
            char hb[4]; snprintf(hb, sizeof(hb), "%02X", v);
            text(hx, y, fg, hb);
            if (a == s_cur && s_nib == 1) fill(hx + 8, y + 14, 8, 2, C(255, 230, 120));   // 下位の 4 ビットを待っている印
            // 文字（ASCII と半角カナは PC-98 の字そのまま）
            int cx = DX + (9 + ROW_BYTES * 3 + 1 + i) * 8;
            if (a == s_cur) fill(cx, y, 8, 16, C(80, 150, 255));
            ank(cx, y, (v >= 0x20 && v < 0x7F) || (v >= 0xA1 && v <= 0xDF) ? v : '.', changed ? C(255, 90, 90) : C(150, 155, 170));
        }
    }
    {   // スクロールバー
        int sy = DY + 16, sh = ROWS * 16;
        fill(SB_X, sy, SB_W, sh, C(45, 48, 60));
        int th = std::max(8, sh * ROWS * ROW_BYTES / (int)MEM_SIZE);
        int ty = sy + (int)((int64_t)(sh - th) * s_top / (MEM_SIZE - ROWS * ROW_BYTES));
        fill(SB_X + 1, ty, SB_W - 2, th, C(150, 160, 190));
    }
    // 右の欄
    button(PX, RESET_Y, PW, 22, "Search RESET");
    checkbox(PX, GAMEONLY_Y, s_gameonly, "ゲームのメモリだけ探す");
    field(PX, INPUT_Y, PW - 74, s_input, s_focus == F_INPUT);
    button(PX + PW - 68, INPUT_Y, 68, 20, "検索");
    radio(PX, RADIO_Y, s_size == 1, "(1)BYTE");
    radio(PX + 95, RADIO_Y, s_size == 2, "(2)WORD");
    radio(PX + 190, RADIO_Y, s_size == 4, "(4)DWORD");
    {
        static const char* lab[4] = {"増加+", "減少-", "不動=", "変動!="};
        int bw = (PW - 18) / 4;
        for (int i = 0; i < 4; i++) button(PX + i * (bw + 6), CMP_Y, bw, 22, lab[i]);
    }
    text_wrap(PX, STATUS_Y, PW, 3, C(30, 30, 40), s_status);
    // 候補の一覧
    fill(PX, LIST_Y, PW, LIST_ROWS * 16 + 4, BG);
    frame(PX, LIST_Y, PW, LIST_ROWS * 16 + 4, C(130, 135, 150));
    if (!s_searched) text(PX + 4, LIST_Y + 2, C(130, 140, 170), "（候補はまだありません）");
    else if (s_cand.empty()) text(PX + 4, LIST_Y + 2, C(255, 120, 120), "候補がなくなりました");
    for (int r = 0; r < LIST_ROWS && m; r++) {
        size_t k = (size_t)s_list_top + (size_t)r;
        if (k >= s_cand.size()) break;
        uint32_t a = s_cand[k];
        int n = s_size; if (a + (uint32_t)n > MEM_SIZE) n = 1;
        uint32_t cur = rd(m, a, n), old = s_snap.size() == MEM_SIZE ? rd(s_snap.data(), a, n) : cur;
        char b[96];
        snprintf(b, sizeof(b), n == 1 ? "%05X  %02X  %u" : n == 2 ? "%05X  %04X  %u" : "%05X  %08X  %u", a, cur, cur);
        int y = LIST_Y + 2 + r * 16;
        if ((int)k == s_list_sel) fill(PX + 1, y, PW - 2, 16, C(60, 90, 150));
        text(PX + 4, y, cur != old ? C(255, 90, 90) : C(225, 228, 235), b);
    }
    // コマンド入力
    if (s_cmd_open) {
        fill(CMD_X, CMD_Y, CMD_W, CMD_H, C(236, 237, 242));
        frame(CMD_X, CMD_Y, CMD_W, CMD_H, C(60, 120, 230));
        char t[64]; snprintf(t, sizeof(t), "コマンド入力 - 番地 %05X", s_cur);
        text(CMD_X + 8, CMD_Y + 6, C(30, 60, 140), t);
        text(CMD_X + 8, CMD_Y + 24, C(30, 30, 40), "10 進で書き込む: s99 = 1 バイト、m999 = 2、l99999 = 4");
        field(CMD_X + 8, CMD_Y + 44, CMD_W - 16, s_cmd, true);
        text_wrap(CMD_X + 8, CMD_Y + 68, CMD_W - 16, 1, C(160, 40, 40), s_cmd_msg);
    }
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
    s_win = SDL_CreateWindow("メモリエディタ - PC98PLAYER", W * sc, H * sc, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN);
    if (!s_win) return;
    s_ren = SDL_CreateRenderer(s_win, nullptr);
    SDL_SetRenderLogicalPresentation(s_ren, W, H, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    SDL_SetRenderDrawColor(s_ren, 236, 237, 242, 255);
    s_tex = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, W, H);
    SDL_SetTextureScaleMode(s_tex, SDL_SCALEMODE_NEAREST);
    SDL_SetWindowMinimumSize(s_win, W / 2, H / 2);
    // 一度入れて切ると、SDL が窓から IME を外す（外さないと、ダンプで打った英字を IME が取ってしまう）
    SDL_StartTextInput(s_win);
    SDL_StopTextInput(s_win);
    update_count("");
}
static void show() {
    create();
    if (!s_win) return;
    SDL_ShowWindow(s_win);
    SDL_RaiseWindow(s_win);
    s_dirty = true;
}
static void place_beside(int x, int y) {
    if (!s_win) return;
    SDL_Rect ub;
    int w = 0, h = 0;
    SDL_GetWindowSize(s_win, &w, &h);
    if (SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(s_win), &ub) && x + w > ub.x + ub.w) x = std::max(ub.x, ub.x + ub.w - w);
    SDL_SetWindowPosition(s_win, x, y);
}
// 「操作中はゲームを止める」: チェックが入っていて、メモリエディタを操作しているとき
static bool pause_requested() { return s_pause && visible() && (SDL_GetWindowFlags(s_win) & SDL_WINDOW_INPUT_FOCUS); }
// 0.1 秒ごとに読み直す（main のループから毎回呼ぶ）
static void tick() {
    if (!visible() || (SDL_GetWindowFlags(s_win) & SDL_WINDOW_MINIMIZED)) return;
    Uint64 now = SDL_GetTicks();
    if (now >= s_next_tick) { s_next_tick = now + 100; age_bytes(); s_dirty = true; }
    present();
}
// 再起動などでメモリが入れ替わったとき（候補は新しいメモリでは意味がないので消す）
static void on_reboot() {
    s_cand.clear(); s_snap.clear(); s_searched = false; s_prev.clear(); s_age.clear(); s_blocks.clear(); s_use_blocks = false;
    update_count("");
}
static bool owns(const SDL_Event& e) { return s_win && SDL_GetWindowFromEvent(&e) == s_win; }

// ---- 入力 -----------------------------------------------------------------
static bool in(float x, float y, int rx, int ry, int rw, int rh) { return x >= rx && x < rx + rw && y >= ry && y < ry + rh; }
static Field* focused_field() { return s_focus == F_ADDR ? &s_addr : s_focus == F_INPUT ? &s_input : s_focus == F_CMD ? &s_cmd : nullptr; }
static void dump_char(char c) {
    if (c == 'w' || c == 'W') { open_command(); return; }
    int d = hexd(c);
    uint8_t* m = ram();
    if (d < 0 || !m) return;
    uint8_t v = m[s_cur];
    v = s_nib == 0 ? (uint8_t)((v & 0x0F) | (d << 4)) : (uint8_t)((v & 0xF0) | d);
    m[s_cur] = v;                 // その場で書き換える
    if (s_nib == 0) s_nib = 1;
    else { s_nib = 0; if (s_cur + 1 < MEM_SIZE) s_cur++; ensure_visible(); }
}
// 打ったキーの文字（表示できる ASCII だけ。キー配列と Shift に従う）。無ければ 0
//  文字入力（SDL の TEXT_INPUT）は IME を通り、英字が IME に取られるので使わない
static char key_char(const SDL_KeyboardEvent& k) {
    if (k.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) return 0;
    if (k.key >= SDLK_KP_1 && k.key <= SDLK_KP_9) return (char)('1' + (k.key - SDLK_KP_1));
    if (k.key == SDLK_KP_0) return '0';
    SDL_Keycode c = SDL_GetKeyFromScancode(k.scancode, k.mod, false);
    return (c >= 0x20 && c < 0x7F) ? (char)c : 0;
}
static void on_key(const SDL_KeyboardEvent& k) {
    if (Field* f = focused_field()) {
        if ((k.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) && k.key == SDLK_A) { f->selall = true; return; }
        switch (k.key) {
        case SDLK_BACKSPACE:
            if (f->selall) { f->text.clear(); f->selall = false; }
            else if (!f->text.empty()) f->text.pop_back();
            return;
        case SDLK_RETURN: case SDLK_KP_ENTER:
            if (s_focus == F_ADDR) address_go();
            else if (s_focus == F_INPUT) value_search();
            else command_exec();
            return;
        case SDLK_ESCAPE: if (s_focus == F_CMD) close_command(); else s_focus = F_DUMP; return;
        case SDLK_TAB: if (s_focus != F_CMD) s_focus = s_focus == F_ADDR ? F_INPUT : F_DUMP; return;
        default: break;
        }
        if (char c = key_char(k)) {
            if (f->selall) { f->text.clear(); f->selall = false; }
            if (f->text.size() < 40) f->text += s_focus == F_ADDR ? (char)toupper((unsigned char)c) : c;
        }
        return;
    }
    if (char c = key_char(k)) { if (hexd(c) >= 0 || c == 'w' || c == 'W') { dump_char(c); return; } }   // 16 進で書き換え、W でコマンド
    int64_t c = s_cur;
    switch (k.key) {
    case SDLK_LEFT: c -= 1; break;
    case SDLK_RIGHT: c += 1; break;
    case SDLK_UP: c -= ROW_BYTES; break;
    case SDLK_DOWN: c += ROW_BYTES; break;
    case SDLK_PAGEUP: c -= (int64_t)ROWS * ROW_BYTES; scroll_rows(-ROWS); break;
    case SDLK_PAGEDOWN: c += (int64_t)ROWS * ROW_BYTES; scroll_rows(ROWS); break;
    case SDLK_HOME: c -= c % ROW_BYTES; break;
    case SDLK_END: c = c - c % ROW_BYTES + ROW_BYTES - 1; break;
    case SDLK_TAB: s_focus = F_ADDR; return;
    default: return;
    }
    if (c < 0) c = 0;
    if (c >= (int64_t)MEM_SIZE) c = MEM_SIZE - 1;
    s_cur = (uint32_t)c; s_nib = 0;
    ensure_visible();
}
static void on_click(float x, float y) {
    if (s_cmd_open) { if (!in(x, y, CMD_X, CMD_Y, CMD_W, CMD_H)) close_command(); return; }
    if (in(x, y, ADDR_X, 6, ADDR_W, 20)) { s_focus = F_ADDR; s_addr.selall = true; return; }
    if (in(x, y, GO_X, 6, GO_W, 20)) { address_go(); return; }
    if (in(x, y, PAUSE_X, 6, 200, 20)) { s_pause = !s_pause; return; }
    if (in(x, y, PX, RESET_Y, PW, 22)) { search_reset(); return; }
    if (in(x, y, PX, GAMEONLY_Y, 200, 18)) { s_gameonly = !s_gameonly; return; }
    if (in(x, y, PX, INPUT_Y, PW - 74, 20)) { s_focus = F_INPUT; return; }
    if (in(x, y, PX + PW - 68, INPUT_Y, 68, 20)) { value_search(); s_focus = F_INPUT; return; }
    for (int i = 0; i < 3; i++) if (in(x, y, PX + i * 95, RADIO_Y, 90, 18)) { s_size = i == 0 ? 1 : i == 1 ? 2 : 4; return; }
    {
        int bw = (PW - 18) / 4;
        for (int i = 0; i < 4; i++) if (in(x, y, PX + i * (bw + 6), CMP_Y, bw, 22)) { compare(i); return; }
    }
    if (in(x, y, PX, LIST_Y, PW, LIST_ROWS * 16 + 4)) {
        size_t k = (size_t)s_list_top + (size_t)((y - LIST_Y - 2) / 16);
        if (k < s_cand.size()) { s_list_sel = (int)k; go_to(s_cand[k]); s_focus = F_DUMP; }   // そのまま 16 進を打てば書き換えられる
        return;
    }
    if (in(x, y, SB_X, DY + 16, SB_W, ROWS * 16)) {   // スクロールバー: その位置へ
        double r = (y - DY - 16) / (ROWS * 16.0);
        s_top = (uint32_t)(r * (MEM_SIZE - ROWS * ROW_BYTES)) / ROW_BYTES * ROW_BYTES;
        clamp_top();
        return;
    }
    if (in(x, y, DX, DY + 16, SB_X - DX, ROWS * 16)) {
        s_focus = F_DUMP;
        int cx = (int)(x - DX) / 8, r = (int)(y - DY - 16) / 16;
        int col = -1;
        if (cx >= 9 && cx < 9 + ROW_BYTES * 3) col = (cx - 9) / 3;
        else if (cx >= 9 + ROW_BYTES * 3 + 1 && cx < 9 + ROW_BYTES * 4 + 1) col = cx - (9 + ROW_BYTES * 3 + 1);
        if (col >= 0) { s_cur = s_top + (uint32_t)r * ROW_BYTES + (uint32_t)col; if (s_cur >= MEM_SIZE) s_cur = MEM_SIZE - 1; s_nib = 0; }
        return;
    }
}
static void on_event(SDL_Event& e) {
    switch (e.type) {
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED: SDL_HideWindow(s_win); return;   // 閉じても中身（候補）は残す。もう一度開ける
    case SDL_EVENT_KEY_DOWN: on_key(e.key); break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (e.button.button != SDL_BUTTON_LEFT) return;
        SDL_ConvertEventToRenderCoordinates(s_ren, &e);
        on_click(e.button.x, e.button.y);
        break;
    case SDL_EVENT_MOUSE_WHEEL: {
        float mx = e.wheel.mouse_x, my = e.wheel.mouse_y;
        SDL_RenderCoordinatesFromWindow(s_ren, mx, my, &mx, &my);
        int d = (int)(e.wheel.y * 3);
        if (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) d = -d;
        if (mx >= PX) {
            int64_t t = (int64_t)s_list_top - d;
            int64_t mxl = (int64_t)s_cand.size() - LIST_ROWS;
            if (t > mxl) t = mxl;
            if (t < 0) t = 0;
            s_list_top = (int)t;
        } else scroll_rows(-d);
        break; }
    case SDL_EVENT_WINDOW_EXPOSED: case SDL_EVENT_WINDOW_RESIZED: break;
    default: return;
    }
    s_dirty = true;
}

}  // namespace memedit
