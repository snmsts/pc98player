// SPDX-License-Identifier: MIT
// -----------------------------------------------------------------------------
//  lio.cpp  --  ROM 内のグラフィック LIO（INT A0h-AFh）の入口
//
//  実機では F990:0000 からグラフィック LIO の ROM があり、先頭は割込みベクタの表:
//    +0: 組の数（11h）、+4 から 4 バイトずつ（ベクタ番号 word, 入口のオフセット word）が並ぶ
//    （A0h〜AFh と CEh。セグメントは F990h）。プログラムによってはこの表を自分で割込みベクタへ
//    写してから INT A0h（GINIT）を呼ぶ（Ray、ランス３など）。ここが空だと写す回数が 0（=65536 回）に
//    なったり、0000:0000 の続きを実行したりして止まってしまう。
//  いまは入口の表と HLE の受け口だけを用意し、各機能は「正常終了（AH=0）」を返す。
//  画面モード等はプログラムが自分で GDC/パレットを設定する前提（Trace=1 で呼び出しを記録）。
// -----------------------------------------------------------------------------
#include "machine.h"

static const uint16_t LIOSEG = 0xF990;
static inline uint32_t lin(uint16_t s, uint16_t o) { return ((uint32_t)s << 4) + o; }

void lio_init(Machine* m) {
    uint8_t* r = m->ram;
    uint32_t base = lin(LIOSEG, 0);
    for (int i = 0; i < 0x100; i++) r[base + i] = 0;
    // +0: 組の数、+4〜: (ベクタ番号, オフセット) が 17 組（A0h〜AFh、CEh）。入口は +80h から 4 バイトずつ
    r[base + 0] = 17;
    for (int i = 0; i < 17; i++) {
        uint16_t off = (uint16_t)(0x80 + i * 4);
        int v = i < 16 ? 0xA0 + i : 0xCE;
        r[base + 4 + i * 4] = (uint8_t)v; r[base + 5 + i * 4] = 0;
        r[base + 6 + i * 4] = (uint8_t)off; r[base + 7 + i * 4] = (uint8_t)(off >> 8);
        uint8_t n = i < 16 ? (uint8_t)(HLE_LIO + i) : (uint8_t)HLE_NOP;
        r[base + off] = 0xF1; r[base + off + 1] = n; r[base + off + 2] = 0xCF;   // HLE n; IRET
        if (i < 16) {
            r[v * 4] = (uint8_t)off; r[v * 4 + 1] = (uint8_t)(off >> 8);
            r[v * 4 + 2] = (uint8_t)LIOSEG; r[v * 4 + 3] = (uint8_t)(LIOSEG >> 8);
        }
    }
}

void lio_hle(Machine* m, uint8_t n) {
    static const char* const names[16] = {
        "GINIT", "GSCREEN", "GVIEW", "GCOLOR1", "GCOLOR2", "GCLS", "GPSET", "GLINE",
        "GCIRCLE", "GPAINT1", "GPAINT2", "GGET", "GPUT1", "GPUT2", "GROLL", "GPOINT2" };
    int f = (n - HLE_LIO) & 15;
    if (m->cfg.trace) {
        uint32_t a = lin(m->cpu.sr[DS_], (uint16_t)m->cpu.r[EBX]);
        plog("[lio] INT %02Xh %s DS:BX=%04X:%04X param:", 0xA0 + f, names[f], m->cpu.sr[DS_], (unsigned)(m->cpu.r[EBX] & 0xFFFF));
        for (int i = 0; i < 16; i++) plog(" %02X", m->ram[(a + i) & 0xFFFFF]);
        plog("\n");
    }
    m->cpu.r[EAX] &= 0xFFFF00FFu;   // AH=0: 正常終了
}
