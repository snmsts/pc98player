// SPDX-License-Identifier: MIT
//  player.cpp -- 1 フレーム進めて、画面と音を取り出す
#include "player.h"
#include "opna_renderer.h"
#include "hostfs.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <algorithm>
#include <time.h>
#include "state.h"

static std::string up(std::string s) { for (auto& c : s) if (c >= 'a' && c <= 'z') c = (char)(c - 32); return s; }
static std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
    return s.substr(a, b - a);
}

bool Ini::load(const std::string& path) {
    void* h = hostfs::open(path, 0, false, false);
    if (!h) return false;
    std::string data;
    char buf[4096];
    int n;
    while ((n = hostfs::read(h, buf, sizeof(buf))) > 0) data.append(buf, (size_t)n);
    hostfs::close(h);
    if (data.size() >= 3 && (uint8_t)data[0] == 0xEF && (uint8_t)data[1] == 0xBB && (uint8_t)data[2] == 0xBF) data.erase(0, 3);
    std::string sec;
    size_t p = 0;
    while (p < data.size()) {
        size_t e = data.find('\n', p);
        if (e == std::string::npos) e = data.size();
        std::string line = trim(data.substr(p, e - p));
        p = e + 1;
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') { size_t r = line.find(']'); sec = up(trim(line.substr(1, r == std::string::npos ? std::string::npos : r - 1))); continue; }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = up(trim(line.substr(0, eq)));
        std::string v = trim(line.substr(eq + 1));
        // 行末コメント（; の前に空白があるもの）
        size_t sc = v.find(" ;");
        if (sc != std::string::npos) v = trim(v.substr(0, sc));
        if (v.size() >= 2 && v[0] == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        kv[sec + "." + k] = v;
    }
    return true;
}
std::string Ini::get(const std::string& key, const std::string& def) const {
    auto it = kv.find(up(key));
    return it == kv.end() ? def : it->second;
}
int Ini::geti(const std::string& key, int def) const {
    std::string v = get(key, "");
    if (v.empty()) return def;
    std::string lv = up(v);
    if (lv == "YES" || lv == "ON" || lv == "TRUE") return 1;
    if (lv == "NO" || lv == "OFF" || lv == "FALSE") return 0;
    if (!lv.empty() && lv[0] == 'X') lv.erase(0, 1);   // x2 のような書き方
    return atoi(lv.c_str());
}

bool player_settings_from_ini(const Ini& ini, const std::string& root, PlayerSettings* ps) {
    const char* S = "PC98PLAYER.";
    auto G = [&](const char* k, const char* d) { return ini.get(std::string(S) + k, d); };
    auto I = [&](const char* k, int d) { return ini.geti(std::string(S) + k, d); };
    ps->cfg.root = root;
    ps->cfg.start = G("START", "");
    ps->cfg.args = G("ARGS", "");
    std::string drv = G("DRIVE", "A");
    ps->cfg.drive = drv.empty() ? 'A' : (char)toupper((unsigned char)drv[0]);
    ps->cfg.cpu_mhz = I("CPUMHZ", I("CPUCLOCK", 16));
    ps->cfg.sound_board = I("SOUNDBOARD", 86);
    ps->cfg.sound_irq = I("SOUNDIRQ", 12);
    ps->cfg.fm_enable = I("SOUND", 1) != 0;
    ps->cfg.memory_kb = I("MEMORYKB", 640);
    ps->cfg.emulate_mouse = I("MOUSEDRIVER", 1) != 0;
    ps->cfg.trace = I("TRACE", 0);
    ps->cfg.key_repeat = I("KEYREPEAT", 1);
    std::string ver = G("DOSVERSION", "5.00");
    {
        int maj = atoi(ver.c_str()), mnr = 0;
        size_t d = ver.find('.');
        if (d != std::string::npos) mnr = atoi(ver.c_str() + d + 1);
        ps->cfg.dos_version = (maj << 8) | mnr;
    }
    ps->scale = I("SCALE", 2);
    if (ps->scale < 1) ps->scale = 1;
    if (ps->scale > 8) ps->scale = 8;
    ps->volume = I("VOLUME", 100);
    ps->fm_volume = I("FMVOLUME", 100);
    ps->ssg_volume = I("SSGVOLUME", 100);
    ps->beep_volume = I("BEEPVOLUME", 50);
    ps->pcm_volume = I("PCMVOLUME", 100);
    ps->fullscreen = I("FULLSCREEN", 0) != 0;
    ps->smooth = I("SMOOTH", 0) != 0;
    ps->exit_on_end = I("EXITONEND", 1) != 0;
    ps->title = G("TITLE", "");
    ps->font_name = G("FONT", "");
    ps->rhythm_rom = G("RHYTHMROM", "");
    ps->jis78 = I("KANJIJIS", 78) != 83;
    ps->sample_rate = I("SAMPLERATE", 44100);
    ps->cfg.midi = I("MIDI", 0);
    ps->cfg.midi_irq = I("MIDIIRQ", 6);
    ps->midi_device = I("MIDIDEVICE", -1);
    ps->cfg.ems = I("EMS", 1) != 0;
    ps->cfg.ems_kb = I("EMSKB", 4096);
    if (ps->cfg.ems_kb < 16) ps->cfg.ems = false;
    if (ps->cfg.ems_kb > 32768) ps->cfg.ems_kb = 32768;
    ps->cfg.xms_kb = I("XMS", 1) ? I("XMSKB", 8192) : 0;
    if (ps->cfg.xms_kb > 65535) ps->cfg.xms_kb = 65535;
    return !ps->cfg.start.empty();
}

bool Player::init(const PlayerSettings& s, std::string* err) {
    ps = s;
    fontrom_set_jis78(ps.jis78);
    m = machine_create(ps.cfg);
    opna = new OpnaRenderer();
    opna->init(MASTER_CLOCK, ps.sample_rate);
    opna->set_balance(1.0 * ps.fm_volume / 100.0, 0.20 * ps.ssg_volume / 100.0);
    if (!ps.rhythm_rom.empty()) {
        std::string path = ps.rhythm_rom;
        void* h = hostfs::open(hostfs::join(ps.cfg.root, path), 0, false, false);
        if (!h) h = hostfs::open(path, 0, false, false);
        if (h) {
            std::vector<uint8_t> d(0x2000);
            int n = hostfs::read(h, d.data(), (int)d.size());
            hostfs::close(h);
            if (n > 0) opna->set_rhythm_rom(d.data(), n);
        }
    }
    std::string cmd = ps.cfg.start;
    if (!ps.cfg.args.empty()) cmd += " " + ps.cfg.args;
    shell_start(m, cmd);
    (void)err;
    return true;
}

// MIDI バイト列をメッセージに区切る（ランニングステータス対応）
void Player::midi_feed(uint8_t b) {
    auto emit = [&]() { if (midi_sink && !midi_msg.empty()) midi_sink(midi_user, midi_msg.data(), (int)midi_msg.size()); };
    if (b >= 0xF8) { uint8_t x = b; if (midi_sink) midi_sink(midi_user, &x, 1); return; }   // リアルタイム
    if (midi_status == 0xF0) {                       // SysEx の途中
        if (b == 0xF7 || b >= 0x80) {
            midi_msg.push_back(0xF7); emit(); midi_msg.clear(); midi_status = 0;
            if (b == 0xF7) return;
        } else { if (midi_msg.size() < 65536) midi_msg.push_back(b); return; }
    }
    if (b & 0x80) {
        midi_msg.clear(); midi_msg.push_back(b);
        if (b == 0xF0) { midi_status = 0xF0; return; }
        if (b >= 0xF0) {                             // システムコモン
            midi_need = (b == 0xF1 || b == 0xF3) ? 1 : (b == 0xF2) ? 2 : 0;
            midi_status = 0;
            if (!midi_need) { emit(); midi_msg.clear(); }
            return;
        }
        midi_status = b;
        midi_need = ((b & 0xE0) == 0xC0) ? 1 : 2;
        return;
    }
    if (midi_msg.empty()) {                          // ランニングステータス
        if (!midi_status || midi_status >= 0xF0) return;
        midi_msg.push_back(midi_status);
        midi_need = ((midi_status & 0xE0) == 0xC0) ? 1 : 2;
    }
    midi_msg.push_back(b);
    if (midi_need && --midi_need == 0) { emit(); midi_msg.clear(); }
}

void Player::run_frame(bool render_video) {
    uint64_t t0 = m->ticks;
    machine_run_frame(m);
    uint64_t t1 = m->ticks;
    if (!m->midi_out.empty()) { for (uint8_t b : m->midi_out) midi_feed(b); m->midi_out.clear(); }
    // 音: 区間 [t0, t1) のサンプルを作る
    audio.clear();
    double step = (double)MASTER_CLOCK / ps.sample_rate;
    size_t wi = 0, bi = 0, pi = 0;
    float vol = ps.volume / 100.0f;
    float pvol = ps.pcm_volume / 100.0f * (m->pcm86.vol / 15.0f) * 0.6f / 32768.0f;
    float bvol = ps.beep_volume / 100.0f * 0.15f;
    while (true) {
        double ts = (double)t0 + sample_acc;
        if (ts >= (double)t1) break;
        while (wi < m->regw.size() && (double)m->regw[wi].tick <= ts) {
            opna->write(m->regw[wi].part, m->regw[wi].addr, m->regw[wi].val);
            wi++;
        }
        while (bi < m->beepw.size() && (double)m->beepw[bi].first <= ts) {
            int v = m->beepw[bi].second;
            if (v == 0) beep_state = false;
            else if (v == 1) beep_state = true;
            bi++;
        }
        float l = 0, r = 0;
        opna->render_one(&l, &r);
        if (pi < m->pcm_out.size() || pcm_l || pcm_r) {
            // PCM86: この標本の時刻までに出た PCM 標本の平均（無ければ直前の値を保つ）
            int n = 0; int32_t sl = 0, sr = 0;
            while (pi < m->pcm_out.size() && (double)m->pcm_out[pi].tick <= ts) { sl += m->pcm_out[pi].l; sr += m->pcm_out[pi].r; n++; pi++; }
            if (n) { pcm_l = sl / n; pcm_r = sr / n; }
            l += pcm_l * pvol; r += pcm_r * pvol;
        }
        if (beep_state) {
            uint32_t rel = m->pit[1].reload ? m->pit[1].reload : 0x10000u;
            double f = (MASTER_CLOCK / 4.0) / rel;
            beep_phase += f / ps.sample_rate;
            if (beep_phase >= 1.0) beep_phase -= floor(beep_phase);
            float b = beep_phase < 0.5 ? bvol : -bvol;
            if (f > 20000) b = 0;
            l += b; r += b;
        }
        l *= vol; r *= vol;
        int li = (int)(l * 32767.0f), ri = (int)(r * 32767.0f);
        li = std::max(-32768, std::min(32767, li));
        ri = std::max(-32768, std::min(32767, ri));
        audio.push_back((int16_t)li); audio.push_back((int16_t)ri);
        sample_acc += step;
    }
    for (; wi < m->regw.size(); wi++) opna->write(m->regw[wi].part, m->regw[wi].addr, m->regw[wi].val);
    for (; bi < m->beepw.size(); bi++) { int v = m->beepw[bi].second; if (v == 0) beep_state = false; else if (v == 1) beep_state = true; }
    if (pi < m->pcm_out.size()) { pcm_l = m->pcm_out.back().l; pcm_r = m->pcm_out.back().r; }
    if (!(m->pcm86.ctrl & 0x80)) pcm_l = pcm_r = 0;
    m->pcm_out.clear();
    m->regw.clear();
    m->beepw.clear();
    sample_acc -= (double)(t1 - t0);
    if (render_video) video_render(m, fb);
}

void Player::shutdown() {
    delete opna; opna = nullptr;
    machine_destroy(m); m = nullptr;
}

// ---- ステートセーブ ----------------------------------------------------------
//  形式: "P98S" 版(4) 時刻文字列 縮小画像(160x100 RGB) 本体の長さ(4) 本体
static const uint32_t STATE_VERSION = 1;

std::string state_slot_path(const std::string& root, int slot) {
    char n[32]; snprintf(n, sizeof(n), "SLOT%d.P98S", slot + 1);
    return hostfs::join(hostfs::join(root, "PC98PLAYER.SAV"), n);
}

void Player::snapshot(std::vector<uint8_t>& out) {
    StateW w;
    machine_state_save(m, w);
    bios_state_save(w);
    dos_state_save(m, w);
    xmsems_state_save(m, w);
    w.tag("PLAY");
    w.pod(sample_acc); w.pod(beep_phase); w.pod(beep_state);
    std::vector<uint8_t> chip;
    opna->save_chip(chip);
    w.u32((uint32_t)chip.size()); w.bytes(chip.data(), chip.size());
    w.tag("END ");
    out.swap(w.b);
}

bool Player::restore(const uint8_t* data, size_t len) {
    StateR r(data, len);
    machine_state_load(m, r);
    bios_state_load(r);
    dos_state_load(m, r);
    xmsems_state_load(m, r);
    r.tag("PLAY");
    r.pod(sample_acc); r.pod(beep_phase); r.pod(beep_state);
    uint32_t clen = r.u32();
    std::vector<uint8_t> chip;
    if (r.ok && clen <= 16 * 1024 * 1024) { chip.resize(clen); r.bytes(chip.data(), clen); } else r.ok = false;
    r.tag("END ");
    if (!r.ok) return false;
    // 音源チップ: 内部状態があれば丸ごと戻す（鳴りかけの音の余韻まで一致する）。
    // 無ければレジスタの控えから作り直し、鳴っていた FM の音を鳴らし直す。
    if (!opna->load_chip(chip)) {
        opna->reset();
        opna->reload_from_registers(m->opna.reg[0], m->opna.reg[1]);
        for (int ch = 0; ch < 8; ch++) if ((ch & 3) != 3 && m->opna.keyon[ch]) opna->write(0, 0x28, (uint8_t)(m->opna.keyon[ch] | ch));
    }
    video_render(m, fb);
    return true;
}

static bool read_all(const std::string& path, std::vector<uint8_t>& d) {
    void* h = hostfs::open(path, 0, false, false);
    if (!h) return false;
    int64_t sz = hostfs::seek(h, 0, 2); hostfs::seek(h, 0, 0);
    if (sz <= 0 || sz > 64 * 1024 * 1024) { hostfs::close(h); return false; }
    d.resize((size_t)sz);
    int r = hostfs::read(h, d.data(), (int)sz);
    hostfs::close(h);
    return r == (int)sz;
}

bool Player::save_state(int slot, std::string* err) {
    std::vector<uint8_t> body;
    snapshot(body);
    StateW w;
    w.tag("P98S"); w.u32(STATE_VERSION);
    time_t t = time(nullptr); struct tm lt;
#ifdef _WIN32
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    char when[32];
    snprintf(when, sizeof(when), "%04d/%02d/%02d %02d:%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min);
    w.str(when);
    // 縮小画像（4x4 平均）
    for (int y = 0; y < STATE_THUMB_H; y++) for (int x = 0; x < STATE_THUMB_W; x++) {
        unsigned rr = 0, gg = 0, bb = 0;
        for (int dy = 0; dy < 4; dy++) for (int dx = 0; dx < 4; dx++) {
            uint32_t c = fb[(y * 4 + dy) * 640 + x * 4 + dx];
            rr += (c >> 16) & 255; gg += (c >> 8) & 255; bb += c & 255;
        }
        uint8_t px[3] = {(uint8_t)(rr / 16), (uint8_t)(gg / 16), (uint8_t)(bb / 16)};
        w.bytes(px, 3);
    }
    w.u32((uint32_t)body.size());
    w.bytes(body.data(), body.size());
    hostfs::mkdir(hostfs::join(ps.cfg.root, "PC98PLAYER.SAV"));
    std::string path = state_slot_path(ps.cfg.root, slot);
    std::string tmp = path + ".tmp";
    void* h = hostfs::open(tmp, 1, true, true);
    if (!h) { if (err) *err = "保存先を作れません: " + path; return false; }
    int wr = hostfs::write(h, w.b.data(), (int)w.b.size());
    hostfs::close(h);
    if (wr != (int)w.b.size()) { hostfs::remove(tmp); if (err) *err = "書き込みに失敗しました"; return false; }
    hostfs::remove(path);
    if (!hostfs::rename(tmp, path)) { if (err) *err = "書き込みに失敗しました"; return false; }
    return true;
}

bool state_slot_info(const std::string& root, int slot, SlotInfo* info) {
    *info = SlotInfo();
    std::vector<uint8_t> d;
    if (!read_all(state_slot_path(root, slot), d)) return false;
    StateR r(d.data(), d.size());
    if (!r.tag("P98S") || r.u32() != STATE_VERSION) return false;
    info->when = r.str();
    info->thumb.resize(STATE_THUMB_W * STATE_THUMB_H * 3);
    r.bytes(info->thumb.data(), info->thumb.size());
    info->used = r.ok;
    return r.ok;
}

bool Player::load_state(int slot, std::string* err) {
    std::vector<uint8_t> d;
    if (!read_all(state_slot_path(ps.cfg.root, slot), d)) { if (err) *err = "このスロットは空です"; return false; }
    StateR r(d.data(), d.size());
    if (!r.tag("P98S") || r.u32() != STATE_VERSION) { if (err) *err = "形式が違います"; return false; }
    r.str();
    std::vector<uint8_t> th(STATE_THUMB_W * STATE_THUMB_H * 3);
    r.bytes(th.data(), th.size());
    uint32_t len = r.u32();
    if (!r.ok || r.pos + len > d.size()) { if (err) *err = "ファイルが壊れています"; return false; }
    std::vector<uint8_t> backup;
    snapshot(backup);
    if (!restore(d.data() + r.pos, len)) {
        restore(backup.data(), backup.size());
        if (err) *err = "ファイルが壊れています";
        return false;
    }
    return true;
}
