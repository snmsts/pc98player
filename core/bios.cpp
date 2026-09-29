// SPDX-License-Identifier: MIT
// -----------------------------------------------------------------------------
//  bios.cpp  --  PC-9801 BIOS の HLE（ROM は使わない）
//
//  INT 18h（キーボード / CRT / グラフィック）、INT 1Ch（カレンダ・タイマ）、
//  IRQ1（キーボード）、INT 33h（マウスドライバ）、INT DCh（DOS 拡張）と、
//  MS-DOS のコンソール出力（ESC シーケンスつき）をここで扱う。
// -----------------------------------------------------------------------------
#include "machine.h"
#include "floppy.h"
#include <string.h>
#include <time.h>
#include <stdio.h>

#define ROMSEG 0xF000
static const uint16_t STUB_BASE = 0x1000;   // F000:1000 から各ベクタの入口を並べる

static inline uint16_t AX(Machine* m) { return (uint16_t)m->cpu.r[EAX]; }
static inline uint16_t BX(Machine* m) { return (uint16_t)m->cpu.r[EBX]; }
static inline uint16_t CX(Machine* m) { return (uint16_t)m->cpu.r[ECX]; }
static inline uint16_t DX(Machine* m) { return (uint16_t)m->cpu.r[EDX]; }
static inline uint8_t AH(Machine* m) { return (uint8_t)(m->cpu.r[EAX] >> 8); }
static inline uint8_t AL(Machine* m) { return (uint8_t)m->cpu.r[EAX]; }
static inline void SETAX(Machine* m, uint16_t v) { m->cpu.r[EAX] = (m->cpu.r[EAX] & 0xFFFF0000u) | v; }
static inline void SETBX(Machine* m, uint16_t v) { m->cpu.r[EBX] = (m->cpu.r[EBX] & 0xFFFF0000u) | v; }
static inline void SETCX(Machine* m, uint16_t v) { m->cpu.r[ECX] = (m->cpu.r[ECX] & 0xFFFF0000u) | v; }
static inline void SETDX(Machine* m, uint16_t v) { m->cpu.r[EDX] = (m->cpu.r[EDX] & 0xFFFF0000u) | v; }
static inline void SETAL(Machine* m, uint8_t v) { m->cpu.r[EAX] = (m->cpu.r[EAX] & 0xFFFFFF00u) | v; }
static inline void SETAH(Machine* m, uint8_t v) { m->cpu.r[EAX] = (m->cpu.r[EAX] & 0xFFFF00FFu) | ((uint32_t)v << 8); }
static inline void SETBH(Machine* m, uint8_t v) { m->cpu.r[EBX] = (m->cpu.r[EBX] & 0xFFFF00FFu) | ((uint32_t)v << 8); }
static inline uint32_t lin(uint16_t s, uint16_t o) { return ((uint32_t)s << 4) + o; }

// ---- 入口の組み立て ----------------------------------------------------------
static uint16_t s_stub_ptr;
static uint16_t put_stub(Machine* m, const uint8_t* code, int len) {
    uint16_t off = s_stub_ptr;
    for (int i = 0; i < len; i++) m->ram[lin(ROMSEG, (uint16_t)(off + i))] = code[i];
    s_stub_ptr = (uint16_t)(s_stub_ptr + len);
    return off;
}
static void set_vec(Machine* m, int vec, uint16_t seg, uint16_t off) {
    m->ram[vec * 4] = (uint8_t)off; m->ram[vec * 4 + 1] = (uint8_t)(off >> 8);
    m->ram[vec * 4 + 2] = (uint8_t)seg; m->ram[vec * 4 + 3] = (uint8_t)(seg >> 8);
}
uint16_t bios_hle_stub(Machine* m, uint8_t n) {
    uint8_t code[3] = {0xF1, n, 0xCF};
    return put_stub(m, code, 3);
}
void bios_hook_vec(Machine* m, int vec, uint8_t n) {
    set_vec(m, vec, ROMSEG, bios_hle_stub(m, n));
}

// ---- キーボード --------------------------------------------------------------
static const uint8_t k_norm[0x60] = {
    0x1B,'1','2','3','4','5','6','7','8','9','0','-','^','\\',0x08,0x09,
    'q','w','e','r','t','y','u','i','o','p','@','[',0x0D,'a','s','d',
    'f','g','h','j','k','l',';',':',']','z','x','c','v','b','n','m',
    ',','.','/','_',' ',0x00,0x00,0x00,0x00,0x7F,0x0B,0x08,0x0C,0x0A,0x1A,0x00,
    '-','/','7','8','9','*','4','5','6','+','1','2','3','=','0',',',
    '.',0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 };
static const uint8_t k_shift[0x60] = {
    0x1B,'!','"','#','$','%','&','\'','(',')','0','=','`','|',0x08,0x09,
    'Q','W','E','R','T','Y','U','I','O','P','~','{',0x0D,'A','S','D',
    'F','G','H','J','K','L','+','*','}','Z','X','C','V','B','N','M',
    '<','>','?','_',' ',0x00,0x00,0x00,0x00,0x7F,0x0B,0x08,0x0C,0x0A,0x1A,0x00,
    '-','/','7','8','9','*','4','5','6','+','1','2','3','=','0',',',
    '.',0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 };

static void kb_push(Machine* m, uint16_t code) {
    uint8_t* r = m->ram;
    uint8_t cnt = r[0x528];
    if (cnt >= 16) return;
    uint16_t tail = (uint16_t)(r[0x526] | (r[0x527] << 8));
    r[tail] = (uint8_t)code; r[tail + 1] = (uint8_t)(code >> 8);
    tail = (uint16_t)(tail + 2);
    if (tail >= 0x522) tail = 0x502;
    r[0x526] = (uint8_t)tail; r[0x527] = (uint8_t)(tail >> 8);
    r[0x528] = (uint8_t)(cnt + 1);
}
int bios_key_available(Machine* m) { return m->ram[0x528]; }
uint16_t bios_key_read(Machine* m, bool remove) {
    uint8_t* r = m->ram;
    if (!r[0x528]) return 0;
    uint16_t head = (uint16_t)(r[0x524] | (r[0x525] << 8));
    uint16_t v = (uint16_t)(r[head] | (r[head + 1] << 8));
    if (remove) {
        head = (uint16_t)(head + 2);
        if (head >= 0x522) head = 0x502;
        r[0x524] = (uint8_t)head; r[0x525] = (uint8_t)(head >> 8);
        r[0x528]--;
    }
    return v;
}
static void kb_clear(Machine* m) {
    m->ram[0x524] = 0x02; m->ram[0x525] = 0x05;
    m->ram[0x526] = 0x02; m->ram[0x527] = 0x05;
    m->ram[0x528] = 0;
}

void bios_key_irq(Machine* m) {
    uint8_t d = io_in8(m, 0x41);
    uint8_t sc = d & 0x7F;
    bool brk = (d & 0x80) != 0;
    uint8_t* bm = &m->ram[0x52A];
    if (brk) bm[sc >> 3] &= (uint8_t)~(1u << (sc & 7));
    else bm[sc >> 3] |= (uint8_t)(1u << (sc & 7));
    uint8_t& sh = m->ram[0x53A];
    if (sc >= 0x70 && sc <= 0x74) {
        uint8_t bit = (uint8_t)(1u << (sc - 0x70));
        if (brk) sh &= (uint8_t)~bit; else sh |= bit;
        return;
    }
    if (brk) return;
    uint8_t ch = 0;
    if (sc < 0x60) ch = (sh & 1) ? k_shift[sc] : k_norm[sc];
    if ((sh & 0x02) && ch >= 'a' && ch <= 'z') ch = (uint8_t)(ch - 0x20);   // CAPS
    if ((sh & 0x10) && ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'))) ch = (uint8_t)((ch & 0x1F));
    kb_push(m, (uint16_t)((sc << 8) | ch));
}

// ---- コンソール（MS-DOS の CON）-------------------------------------------------
struct Console {
    int x, y;
    uint8_t attr;
    int esc;           // 0=通常 1=ESC 2=ESC[ 3=ESC= 4=ESC=y 5=ESC) / ESC(
    char buf[32];
    int blen;
    uint8_t sj1;       // シフト JIS 第 1 バイト待ち
    int sx, sy;
    int rows;
};
static Console s_con;
static bool s_rev_by_bg;   // 反転が 40-47 由来か（con_sgr を参照）

static void con_cell(Machine* m, int x, int y, uint16_t code, uint8_t attr) {
    if (x < 0 || x >= 80 || y < 0 || y >= 25) return;
    uint32_t i = (uint32_t)(y * 80 + x) * 2;
    m->tvram[i] = (uint8_t)code; m->tvram[i + 1] = (uint8_t)(code >> 8);
    m->tvram[0x2000 + i] = attr;
}
static void con_clear_range(Machine* m, int from, int to) {
    for (int i = from; i < to; i++) con_cell(m, i % 80, i / 80, 0x20, s_con.attr);
}
static void con_scroll(Machine* m) {
    int rows = s_con.rows;
    memmove(m->tvram, m->tvram + 160, (size_t)(rows - 1) * 160);
    memmove(m->tvram + 0x2000, m->tvram + 0x2000 + 160, (size_t)(rows - 1) * 160);
    con_clear_range(m, (rows - 1) * 80, rows * 80);
}
static void con_newline(Machine* m) {
    s_con.y++;
    if (s_con.y >= s_con.rows) { s_con.y = s_con.rows - 1; con_scroll(m); }
}
// MS-DOS のワークエリア: 0000:071Ch = カーソルの桁、0000:0710h = カーソルの行（Borland C の conio などが直接読み書きする）
static void con_sync_cursor(Machine* m) {
    m->gdcm.ead = (uint32_t)(s_con.y * 80 + s_con.x);
    m->ram[0x71C] = (uint8_t)s_con.x;
    m->ram[0x710] = (uint8_t)s_con.y;
}
static void con_load_state(Machine* m) {
    s_con.attr = m->ram[0x71D];
    int x = m->ram[0x71C], y = m->ram[0x710];
    if (x < 80) s_con.x = x;
    if (y < s_con.rows) s_con.y = y;
}
void console_reset(Machine* m) {
    memset(&s_con, 0, sizeof(s_con));
    s_con.attr = 0xE1;
    s_con.rows = 25;
    s_rev_by_bg = false;
    (void)m;
}
// 40-47（色つき反転）で付いた反転は、続く 30-37 で外れる（NEC の ANSI の振る舞い。
// Ray のメニューは ESC[46m で強調し ESC[36m で戻す）。ESC[7m の反転はそのまま残す。
static void con_sgr(int n) {
    uint8_t& a = s_con.attr;
    if (n == 0) { a = 0xE1; s_rev_by_bg = false; }
    else if (n == 7) { a |= 0x04; s_rev_by_bg = false; }
    else if (n == 1) a |= 0; // 強調なし
    else if (n == 2) a = (uint8_t)((a & 0x1F) | 0xE0 & 0);   // なし
    else if (n == 4) a |= 0x08;
    else if (n == 5) a |= 0x02;
    else if (n == 8 || n == 16) a &= (uint8_t)~1;
    else if (n >= 30 && n <= 37) {
        static const uint8_t col[8] = {0, 2, 4, 6, 1, 3, 5, 7};   // ANSI -> PC-98 GRB
        if (s_rev_by_bg) { a &= (uint8_t)~0x04; s_rev_by_bg = false; }
        a = (uint8_t)((a & 0x1F) | (col[n - 30] << 5) | 1);
    } else if (n >= 17 && n <= 23) {
        static const uint8_t col[7] = {2, 4, 6, 1, 3, 5, 7};
        a = (uint8_t)((a & 0x1F) | (col[n - 17] << 5) | 1);
    } else if (n >= 40 && n <= 47) {
        static const uint8_t col[8] = {0, 2, 4, 6, 1, 3, 5, 7};
        if (!(a & 0x04)) s_rev_by_bg = true;
        a = (uint8_t)((col[n - 40] << 5) | 0x05);
    }
}
static void con_csi(Machine* m, char fin) {
    int args[8] = {0}, na = 0;
    bool priv = false;
    const char* p = s_con.buf;
    if (*p == '>' || *p == '?') { priv = true; p++; }
    while (*p && na < 8) {
        int v = 0; bool any = false;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; any = true; }
        args[na++] = any ? v : -1;
        if (*p == ';') p++; else break;
    }
    int a0 = na > 0 && args[0] >= 0 ? args[0] : -1;
    int n1 = a0 > 0 ? a0 : 1;
    switch (fin) {
    case 'H': case 'f': {
        int yy = (na > 0 && args[0] > 0) ? args[0] - 1 : 0;
        int xx = (na > 1 && args[1] > 0) ? args[1] - 1 : 0;
        s_con.x = xx < 80 ? xx : 79; s_con.y = yy < s_con.rows ? yy : s_con.rows - 1; break; }
    case 'A': s_con.y -= n1; if (s_con.y < 0) s_con.y = 0; break;
    case 'B': s_con.y += n1; if (s_con.y >= s_con.rows) s_con.y = s_con.rows - 1; break;
    case 'C': s_con.x += n1; if (s_con.x > 79) s_con.x = 79; break;
    case 'D': s_con.x -= n1; if (s_con.x < 0) s_con.x = 0; break;
    case 'J':
        if (a0 == 2) { con_clear_range(m, 0, 80 * 25); s_con.x = s_con.y = 0; }
        else if (a0 == 1) con_clear_range(m, 0, s_con.y * 80 + s_con.x + 1);
        else con_clear_range(m, s_con.y * 80 + s_con.x, 80 * 25);
        break;
    case 'K':
        if (a0 == 2) con_clear_range(m, s_con.y * 80, s_con.y * 80 + 80);
        else if (a0 == 1) con_clear_range(m, s_con.y * 80, s_con.y * 80 + s_con.x + 1);
        else con_clear_range(m, s_con.y * 80 + s_con.x, s_con.y * 80 + 80);
        break;
    case 'm': if (na == 0) con_sgr(0); for (int i = 0; i < na; i++) con_sgr(args[i] < 0 ? 0 : args[i]); break;
    case 's': s_con.sx = s_con.x; s_con.sy = s_con.y; break;
    case 'u': s_con.x = s_con.sx; s_con.y = s_con.sy; break;
    case 'h': case 'l':
        if (priv) {
            bool set = fin == 'h';
            if (a0 == 5) { if (set) m->gdcm.csrform[0] &= 0x7F; else m->gdcm.csrform[0] |= 0x80; }
            // 1: ファンクションキー表示, 3: 20/25 行 ―― 表示だけの問題なので受け流す
        }
        break;
    case 'M': {   // 行削除
        for (int k = 0; k < n1; k++) {
            int rows = s_con.rows;
            int y = s_con.y;
            memmove(m->tvram + y * 160, m->tvram + (y + 1) * 160, (size_t)(rows - 1 - y) * 160);
            memmove(m->tvram + 0x2000 + y * 160, m->tvram + 0x2000 + (y + 1) * 160, (size_t)(rows - 1 - y) * 160);
            con_clear_range(m, (rows - 1) * 80, rows * 80);
        }
        break; }
    case 'L': {   // 行挿入
        for (int k = 0; k < n1; k++) {
            int rows = s_con.rows, y = s_con.y;
            memmove(m->tvram + (y + 1) * 160, m->tvram + y * 160, (size_t)(rows - 1 - y) * 160);
            memmove(m->tvram + 0x2000 + (y + 1) * 160, m->tvram + 0x2000 + y * 160, (size_t)(rows - 1 - y) * 160);
            con_clear_range(m, y * 80, y * 80 + 80);
        }
        break; }
    }
}

static void console_putc_body(Machine* m, uint8_t c);
// MS-DOS のワークエリア 0000:071Dh が「今の表示属性」。ソフトが直接読み書きするので、ここと常にそろえる
void console_putc(Machine* m, uint8_t c) {
    con_load_state(m);
    console_putc_body(m, c);
    m->ram[0x71D] = s_con.attr;
    m->ram[0x71C] = (uint8_t)s_con.x;
    m->ram[0x710] = (uint8_t)s_con.y;
}
static void console_putc_body(Machine* m, uint8_t c) {
    Console& k = s_con;
    if (k.esc) {
        if (k.esc == 1) {
            if (c == '[') { k.esc = 2; k.blen = 0; k.buf[0] = 0; return; }
            if (c == '=') { k.esc = 3; return; }
            if (c == ')' || c == '(') { k.esc = 5; return; }
            if (c == '*') { con_clear_range(m, 0, 80 * 25); k.x = k.y = 0; k.esc = 0; return; }
            if (c == 'D') { con_newline(m); k.esc = 0; return; }
            if (c == 'E') { k.x = 0; con_newline(m); k.esc = 0; return; }
            if (c == 'M') { if (k.y > 0) k.y--; k.esc = 0; return; }
            k.esc = 0; return;
        }
        if (k.esc == 2) {
            if ((c >= '0' && c <= '9') || c == ';' || c == '>' || c == '?') {
                if (k.blen < 30) { k.buf[k.blen++] = (char)c; k.buf[k.blen] = 0; }
                return;
            }
            con_csi(m, (char)c); k.esc = 0; con_sync_cursor(m); return;
        }
        if (k.esc == 3) { k.sy = c - 0x20; k.esc = 4; return; }
        if (k.esc == 4) { k.y = k.sy; k.x = c - 0x20; if (k.x < 0) k.x = 0; if (k.x > 79) k.x = 79;
                          if (k.y < 0) k.y = 0; if (k.y >= k.rows) k.y = k.rows - 1; k.esc = 0; con_sync_cursor(m); return; }
        if (k.esc == 5) { k.esc = 0; return; }
    }
    if (k.sj1) {
        uint16_t jis = sjis_to_jis((uint16_t)((k.sj1 << 8) | c));
        k.sj1 = 0;
        uint8_t row = (uint8_t)((jis >> 8) - 0x20), cell = (uint8_t)(jis & 0xFF);
        if (row >= 0x09 && row <= 0x0B) {   // 半角
            con_cell(m, k.x, k.y, (uint16_t)((cell << 8) | row), k.attr);
            if (++k.x >= 80) { k.x = 0; con_newline(m); }
        } else {
            if (k.x >= 79) { k.x = 0; con_newline(m); }
            con_cell(m, k.x, k.y, (uint16_t)((cell << 8) | row), k.attr);
            con_cell(m, k.x + 1, k.y, (uint16_t)(((cell | 0x80) << 8) | row), k.attr);
            k.x += 2;
            if (k.x >= 80) { k.x = 0; con_newline(m); }
        }
        con_sync_cursor(m);
        return;
    }
    switch (c) {
    case 0x1B: k.esc = 1; return;
    case 0x07: m->beepw.push_back({m->ticks, 3}); return;   // ベル
    case 0x08: if (k.x > 0) k.x--; con_sync_cursor(m); return;
    case 0x09: k.x = (k.x + 8) & ~7; if (k.x >= 80) { k.x = 0; con_newline(m); } con_sync_cursor(m); return;
    case 0x0A: con_newline(m); con_sync_cursor(m); return;
    case 0x0B: if (k.y > 0) k.y--; con_sync_cursor(m); return;
    case 0x0C: if (k.x < 79) k.x++; con_sync_cursor(m); return;
    case 0x0D: k.x = 0; con_sync_cursor(m); return;
    case 0x1A: con_clear_range(m, 0, 80 * 25); k.x = k.y = 0; con_sync_cursor(m); return;
    case 0x1E: k.x = k.y = 0; con_sync_cursor(m); return;
    }
    if ((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC)) { k.sj1 = c; return; }
    con_cell(m, k.x, k.y, c, k.attr);
    if (++k.x >= 80) { k.x = 0; con_newline(m); }
    con_sync_cursor(m);
}

// ---- INT 18h -----------------------------------------------------------------
static void wait_and_retry(Machine* m) {
    // 入力待ち: いまの F1 18 をもう一度実行するように戻し、割込みを許して HLT する
    m->cpu.ip = (uint16_t)(m->cpu.ip - 2);
    m->cpu.fl |= FL_IF;
    m->cpu.halted = 1;
}

static void int18(Machine* m) {
    uint8_t ah = AH(m);
    switch (ah) {
    case 0x00:
        if (!bios_key_available(m)) { wait_and_retry(m); return; }
        SETAX(m, bios_key_read(m, true)); return;
    case 0x01:
        if (bios_key_available(m)) { SETAX(m, bios_key_read(m, false)); SETBH(m, 1); }
        else SETBH(m, 0);
        return;
    case 0x02: SETAL(m, m->ram[0x53A]); return;
    case 0x03: kb_clear(m); return;
    case 0x04: SETAH(m, m->ram[0x52A + (AL(m) & 0x0F)]); return;
    case 0x05:
        if (bios_key_available(m)) { SETAX(m, bios_key_read(m, true)); SETBH(m, 1); }
        else SETBH(m, 0);
        return;
    case 0x0A: {
        uint8_t al = AL(m);
        m->ram[0x53C] = al;
        m->gdcm.csrform[0] = (uint8_t)((m->gdcm.csrform[0] & 0xE0) | ((al & 1) ? 19 : 15));
        s_con.rows = (al & 1) ? 20 : 25;
        m->ram[0x712] = (uint8_t)(s_con.rows - 1);
        m->modeff[0] = (al & 4) ? 1 : 0;   // 属性 bit4 = 簡易グラフ(1) / バーチカルライン(0)
        m->modeff[2] = (al & 2) ? 1 : 0;   // 40 桁
        m->modeff[5] = (al & 8) ? 1 : 0;   // コードアクセス
        return; }
    case 0x0B: SETAL(m, m->ram[0x53C]); return;
    case 0x0C: m->gdcm.display = 1; return;
    case 0x0D: m->gdcm.display = 0; return;
    case 0x0E: m->gdcm.pram[0] = (uint8_t)(DX(m) >> 1); m->gdcm.pram[1] = (uint8_t)(DX(m) >> 9); return;
    case 0x0F: return;
    case 0x10: return;
    case 0x11: m->gdcm.csrform[0] |= 0x80; return;
    case 0x12: m->gdcm.csrform[0] &= 0x7F; return;
    case 0x13: m->gdcm.ead = (uint32_t)(DX(m) >> 1); return;
    case 0x14: {   // フォント読み出し
        uint16_t code = DX(m);
        uint32_t buf = lin(BX(m), CX(m));
        if ((code & 0xFF00) == 0 || (code & 0xFF00) == 0x8000) {
            const uint8_t* g = font_get_ank((uint8_t)code);
            mem_wb(m, buf, 1); mem_wb(m, buf + 1, 2);
            for (int i = 0; i < 16; i++) mem_wb(m, buf + 2 + i, g[i]);
        } else {
            uint16_t jis = code & 0x7F7F;
            const uint8_t* g = font_get_kanji(jis);
            uint8_t row = (uint8_t)(jis >> 8);
            if (row >= 0x29 && row <= 0x2B) {
                mem_wb(m, buf, 1); mem_wb(m, buf + 1, 2);
                for (int i = 0; i < 16; i++) mem_wb(m, buf + 2 + i, g[i * 2]);
            } else {
                mem_wb(m, buf, 2); mem_wb(m, buf + 1, 2);
                for (int i = 0; i < 32; i++) mem_wb(m, buf + 2 + i, g[i]);
            }
        }
        return; }
    case 0x16: {
        uint8_t ch = (uint8_t)DX(m), at = (uint8_t)(DX(m) >> 8);
        for (int i = 0; i < 0x1000; i++) { m->tvram[i * 2] = ch; m->tvram[i * 2 + 1] = 0; m->tvram[0x2000 + i * 2] = at; }
        return; }
    case 0x17: m->portc &= (uint8_t)~0x08; m->beepw.push_back({m->ticks, 1}); return;
    case 0x18: m->portc |= 0x08; m->beepw.push_back({m->ticks, 0}); return;
    case 0x1A: {   // 外字の定義: DX=JIS コード（76xx/77xx）, BX:CX=バッファ（先頭 2 バイトの後に 32 バイト）
        uint16_t code = DX(m);
        uint32_t buf = lin(BX(m), CX(m));
        if (fontrom_is_gaiji(code))
            for (int y = 0; y < 16; y++) {
                fontrom_gaiji_write(code, y, true, mem_rb(m, buf + 2 + y * 2));
                fontrom_gaiji_write(code, y, false, mem_rb(m, buf + 3 + y * 2));
            }
        return; }
    case 0x40: m->gdcs.display = 1; m->ram[0x54C] |= 0x80; return;
    case 0x41: m->gdcs.display = 0; m->ram[0x54C] &= 0x7F; return;
    case 0x42: {
        uint8_t ch = (uint8_t)(CX(m) >> 8);
        int mode = ch >> 6;
        // CH bit7-6: 01 = VRAM の後半 200 ライン（SAD=8000 ワード）/ 10 = 前半 200 ライン / 11 = 400 ライン
        // （master.lib の graph_200line(1) は前者で、前半を隠し VRAM に使う: 東方夢時空）
        memset(m->gdcs.pram, 0, 4);
        if (mode == 3) { m->gfx_200 = 0; m->gdcs.zoom = 0; }
        else {
            m->gfx_200 = 1; m->gfx_200_lower = 0; m->gdcs.zoom = 0;
            if (mode == 1) { m->gdcs.pram[0] = (uint8_t)(8000 & 0xFF); m->gdcs.pram[1] = (uint8_t)(8000 >> 8); }
            m->gdcs.pram[3] = 0x40;
        }
        m->gfx_color = (ch & 0x20) ? 0 : 1;
        m->disp_bank = (ch >> 4) & 1;
        return; }
    case 0x43: {   // パレット（デジタル）
        uint32_t a = lin(m->cpu.sr[DS_], BX(m));
        for (int i = 0; i < 4; i++) m->degpal[i] = mem_rb(m, a + i);
        return; }
    case 0x44: m->border = (uint8_t)(CX(m) >> 8); return;
    default:
        if (m->cfg.trace) plog("[bios] INT 18h AH=%02Xh 未対応\n", ah);
        return;
    }
}

// ---- INT 1Ch -----------------------------------------------------------------
static inline uint8_t bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static void int1c(Machine* m) {
    uint8_t ah = AH(m);
    if (ah == 0x00) {
        PcTime lt; machine_now(m, &lt);
        uint32_t a = lin(m->cpu.sr[ES_], BX(m));
        mem_wb(m, a + 0, bcd(lt.year % 100));
        mem_wb(m, a + 1, (uint8_t)((lt.month << 4) | lt.wday));
        mem_wb(m, a + 2, bcd(lt.day));
        mem_wb(m, a + 3, bcd(lt.hour));
        mem_wb(m, a + 4, bcd(lt.min));
        mem_wb(m, a + 5, bcd(lt.sec));
        return;
    }
    if (ah == 0x02 || ah == 0x03) {
        // インターバルタイマ: CX*10ms 後に INT 07h（ES:BX を INT 07h に据える）
        set_vec(m, 0x07, m->cpu.sr[ES_], BX(m));
        m->ram[0x58A] = (uint8_t)CX(m); m->ram[0x58B] = (uint8_t)(CX(m) >> 8);
        io_out8(m, 0x77, 0x36);
        uint16_t cnt = 19968;   // 10ms @ 1.9968MHz
        io_out8(m, 0x71, (uint8_t)cnt); io_out8(m, 0x71, (uint8_t)(cnt >> 8));
        io_out8(m, 0x02, (uint8_t)(io_in8(m, 0x02) & ~1));
        return;
    }
}

// ---- INT 33h（マウスドライバ）-------------------------------------------------
static void int33(Machine* m) {
    Mouse* ms = &m->mouse;
    uint16_t ax = AX(m);
    switch (ax) {
    case 0x00: SETAX(m, 0xFFFF); SETBX(m, 2); ms->visible = 0; return;
    case 0x01: ms->visible = 1; return;
    case 0x02: ms->visible = 0; return;
    case 0x03: {
        int b = (ms->buttons & 1) | ((ms->buttons & 2) ? 2 : 0);
        SETBX(m, (uint16_t)b); SETCX(m, (uint16_t)ms->x); SETDX(m, (uint16_t)ms->y); return; }
    case 0x04: ms->x = (int16_t)CX(m); ms->y = (int16_t)DX(m); return;
    case 0x05: case 0x06: {
        int b = BX(m) & 1;
        int bstate = (ms->buttons & 1) | ((ms->buttons & 2) ? 2 : 0);
        SETAX(m, (uint16_t)bstate);
        if (ax == 5) { SETBX(m, (uint16_t)ms->press_cnt[b]); SETCX(m, (uint16_t)ms->press_x[b]); SETDX(m, (uint16_t)ms->press_y[b]); ms->press_cnt[b] = 0; }
        else { SETBX(m, (uint16_t)ms->release_cnt[b]); SETCX(m, (uint16_t)ms->release_x[b]); SETDX(m, (uint16_t)ms->release_y[b]); ms->release_cnt[b] = 0; }
        return; }
    case 0x07: ms->minx = (int16_t)CX(m); ms->maxx = (int16_t)DX(m); if (ms->minx > ms->maxx) { int t = ms->minx; ms->minx = ms->maxx; ms->maxx = t; } return;
    case 0x08: ms->miny = (int16_t)CX(m); ms->maxy = (int16_t)DX(m); if (ms->miny > ms->maxy) { int t = ms->miny; ms->miny = ms->maxy; ms->maxy = t; } return;
    case 0x0B: SETCX(m, (uint16_t)ms->hle_dx); SETDX(m, (uint16_t)ms->hle_dy); ms->hle_dx = ms->hle_dy = 0; return;
    case 0x0F: ms->mickey_x = CX(m); ms->mickey_y = DX(m); return;
    // NEC の MOUSE.COM: 10h = 横の範囲、11h = 縦の範囲（CX=最小, DX=最大）
    case 0x10: case 0x11: {
        int lo = (int16_t)CX(m), hi = (int16_t)DX(m);
        if (lo > hi) { int t = lo; lo = hi; hi = t; }
        if (ax == 0x10) { ms->minx = lo; ms->maxx = hi; if (ms->x < lo) ms->x = lo; if (ms->x > hi) ms->x = hi; }
        else { ms->miny = lo; ms->maxy = hi; if (ms->y < lo) ms->y = lo; if (ms->y > hi) ms->y = hi; }
        return; }
    case 0x21: SETAX(m, 0xFFFF); SETBX(m, 2); return;
    default:
        if (m->cfg.trace) plog("[bios] INT 33h AX=%04Xh 未対応\n", ax);
        return;
    }
}

// ---- INT DCh（MS-DOS の PC-98 拡張）-------------------------------------------
static void intdc(Machine* m) {
    uint8_t cl = (uint8_t)CX(m);
    if (cl == 0x10) {
        uint8_t ah = AH(m);
        con_load_state(m);
        switch (ah) {
        case 0x00: console_putc(m, (uint8_t)DX(m)); return;
        case 0x01: { uint32_t a = lin(m->cpu.sr[DS_], DX(m)); for (int i = 0; i < 4096; i++) { uint8_t c = mem_rb(m, a + i); if (c == '$') break; console_putc(m, c); } return; }
        case 0x02: s_con.attr = (uint8_t)DX(m); m->ram[0x71D] = s_con.attr; return;
        case 0x03: s_con.x = (uint8_t)DX(m); s_con.y = (uint8_t)(DX(m) >> 8); if (s_con.x > 79) s_con.x = 79; if (s_con.y >= s_con.rows) s_con.y = s_con.rows - 1; con_sync_cursor(m); return;
        case 0x0A: con_clear_range(m, s_con.y * 80 + s_con.x, s_con.y * 80 + 80); return;
        case 0x0A + 1: con_clear_range(m, 0, 80 * 25); return;
        default: return;
        }
    }
    // 0Ch/0Dh: ファンクションキー定義の取得/設定 ―― 何もしない
}

// ---- 振り分け ----------------------------------------------------------------
void bios_hle(Machine* m, uint8_t n) {
    switch (n) {
    case HLE_INT18: int18(m); return;
    case HLE_INT1C: int1c(m); return;
    case HLE_INT09: bios_key_irq(m); machine_eoi(m, 1); return;
    case HLE_IRQ_EOI_M: {
        // 受け付け中の最上位を落とす
        for (int i = 0; i < 8; i++) if (m->pic[0].isr & (1u << i)) { m->pic[0].isr &= (uint8_t)~(1u << i); break; }
        pic_update_hint(m);
        return; }
    case HLE_IRQ_EOI_S: {
        for (int i = 0; i < 8; i++) if (m->pic[1].isr & (1u << i)) { m->pic[1].isr &= (uint8_t)~(1u << i); break; }
        if (!m->pic[1].isr) m->pic[0].isr &= (uint8_t)~0x80;
        pic_update_hint(m);
        return; }
    case HLE_INT1A: SETAH(m, 0x00); return;
    case HLE_INT1B:
        if (floppy::is_fd_da(AL(m))) { floppy::bios_int1b(m); return; }
        if (m->cfg.trace) plog("[fd] INT1B AX=%04X（フロッピー以外の装置: 未接続を返す）\n", (unsigned)(m->cpu.r[EAX] & 0xFFFF));
        SETAH(m, 0x60); set_cf(m, true); return;
    case HLE_INT1F: SETAH(m, 0x00); set_cf(m, true); return;
    case HLE_INT33: int33(m); return;
    case HLE_INTDC: intdc(m); return;
    case HLE_INT06: {
        uint32_t a = lin(m->cpu.sr[SS_], (uint16_t)m->cpu.r[ESP]);
        uint16_t ip = mem_rw(m, a), cs = mem_rw(m, a + 2);
        plog("[bios] 未定義命令で停止しました: %04X:%04X (%02X %02X %02X)\n", cs, ip,
             mem_rb(m, lin(cs, ip)), mem_rb(m, lin(cs, (uint16_t)(ip + 1))), mem_rb(m, lin(cs, (uint16_t)(ip + 2))));
        m->status = "未対応の CPU 命令に遭遇しました";
        m->quit = 2;
        return; }
    case HLE_NOP: return;
    default:
        if (n >= HLE_LIO && n < HLE_LIO + 16) { lio_hle(m, n); return; }
        if (m->cfg.trace) plog("[bios] HLE %02X 未対応\n", n);
        return;
    }
}

// ---- 初期化 ------------------------------------------------------------------
void bios_init(Machine* m) {
    s_stub_ptr = STUB_BASE;
    uint8_t* r = m->ram;
    // 既定: 全ベクタは IRET
    uint8_t iret[1] = {0xCF};
    uint16_t iret_off = put_stub(m, iret, 1);
    for (int v = 0; v < 256; v++) set_vec(m, v, ROMSEG, iret_off);

    // ハードウェア割込みの既定（EOI して戻る）
    uint16_t eoi_m = bios_hle_stub(m, HLE_IRQ_EOI_M);
    uint16_t eoi_s = bios_hle_stub(m, HLE_IRQ_EOI_S);
    for (int v = 0x08; v < 0x10; v++) set_vec(m, v, ROMSEG, eoi_m);
    for (int v = 0x10; v < 0x18; v++) set_vec(m, v, ROMSEG, eoi_s);

    // IRQ0: BIOS インターバルタイマ
    static const uint8_t timer_code[] = {
        0x50, 0x1E, 0x31, 0xC0, 0x8E, 0xD8,       // push ax; push ds; xor ax,ax; mov ds,ax
        0xB0, 0x20, 0xE6, 0x00,                   // mov al,20h; out 00h,al
        0xFF, 0x0E, 0x8A, 0x05,                   // dec word [058Ah]
        0x75, 0x08,                               // jnz done
        0xE4, 0x02, 0x0C, 0x01, 0xE6, 0x02,       // in al,02h; or al,1; out 02h,al
        0xCD, 0x07,                               // int 07h
        0x1F, 0x58, 0xCF                          // done: pop ds; pop ax; iret
    };
    set_vec(m, 0x08, ROMSEG, put_stub(m, timer_code, sizeof(timer_code)));
    bios_hook_vec(m, 0x09, HLE_INT09);
    bios_hook_vec(m, 0x06, HLE_INT06);
    bios_hook_vec(m, 0x18, HLE_INT18);
    bios_hook_vec(m, 0x1A, HLE_INT1A);
    bios_hook_vec(m, 0x1B, HLE_INT1B);
    bios_hook_vec(m, 0x1C, HLE_INT1C);
    bios_hook_vec(m, 0x1F, HLE_INT1F);
    bios_hook_vec(m, 0xDC, HLE_INTDC);
    if (m->cfg.emulate_mouse) bios_hook_vec(m, 0x33, HLE_INT33);

    // BIOS ワークエリア
    r[0x500] = 0x00;
    r[0x501] = 0x80 | 0x04;         // 8MHz 系, メモリ 640KB
    r[0x53C] = 0x00;
    r[0x54C] = 0x4E;                // 16 色ボードあり 等
    r[0x54D] = 0x40;                // EGC あり
    r[0x480] = 0x00;
    r[0x481] = 0x00;
    r[0x458] = 0x00;
    r[0x55C] = 0x03;               // 1MB の FDD がユニット 0・1 に付いている（仮想フロッピーはユニット 0）
    r[0x712] = 24;                 // MS-DOS: テキストの行数-1（master.lib などが参照）
    r[0x71D] = 0xE1;               // 同: 今の表示属性（白）。Borland C の conio などが初期値として読む
    r[0x714] = 0xE1;               // 同: 画面消去に使う属性
    r[0x710] = 0;                  // 同: カーソル表示など
    kb_clear(m);
    console_reset(m);
    // テキスト VRAM を空白に
    for (int i = 0; i < 0x1000; i++) { m->tvram[i * 2] = 0x20; m->tvram[i * 2 + 1] = 0; m->tvram[0x2000 + i * 2] = 0xE1; }
    // リセットベクタ（使わないが念のため）
    r[0xFFFF0] = 0xF4;
    lio_init(m);
    // ROM の識別に使われることがある領域
    r[0xFFFFE] = 0xFE;
}

// ---- ステートセーブ ----------------------------------------------------------
#include "state.h"
void bios_state_save(StateW& w) { w.tag("BIOS"); w.pod(s_con); }
void bios_state_load(StateR& r) { r.tag("BIOS"); r.pod(s_con); }
