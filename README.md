# PC98PLAYER

PC-9801 のゲームを、**インストール済みのフォルダごと** Windows 11 のアプリのように動かすプレイヤーです。
GMPV3 Studio の「必要なデバイスの振る舞いと DOS ファンクションだけを自前で持つ小さな仮想 PC-98」を、
音だけでなく **画面・キーボード・マウス・ファイル** まで広げたものです。

```
ゲームのフォルダ\
    KAKYU.BAT, AI5.EXE, PLAY5.COM ...   ← PC-98 のゲーム（HDD インストール済みのもの）
    PC98PLAYER.EXE                        ← これを置く
    PC98PLAYER.INI                        ← 設定（無ければ初回起動時にひな形を作ります）
```

`PC98PLAYER.EXE` をダブルクリックすると窓が開き、INI の `Start=` に書いたプログラムまたはバッチから動き出します。

## 仕組み

実機の ROM・フロッピー/HDD イメージ・MS-DOS 本体は **一切使いません**。

| 層 | 中身 |
|---|---|
| CPU | i386 リアルモードのインタプリタ（8086/V30/286 上位互換、32bit レジスタ・386 命令込み） |
| ハードウェア | GDC（テキスト/グラフィック、200 ライン表示）、GRCG、**EGC**、16 色アナログパレット、8259、8253、キーボード、バスマウス、漢字 ROM ポート・外字、YM2608(OPNA)、**86 ボードの PCM**、**MPU-PC98II（MIDI）** |
| BIOS | HLE: INT 18h（キー/CRT/グラフィック）、INT 1Ch（時計/インターバルタイマ）、IRQ1、INT 33h、INT DCh |
| DOS | HLE: INT 21h をゲームのフォルダへ直結（ファイル・FCB・MCB メモリ管理・EXEC・常駐終了・FindFirst・SFT/List of Lists など） |
| 拡張メモリ | HLE: **HIMEM.SYS（XMS 3.0）** と **EMM386（LIM EMS 4.0、ページフレーム D000h）** |
| シェル | COMMAND.COM の代わりにバッチを解釈（ECHO/IF ERRORLEVEL/IF EXIST/GOTO/CALL/SET/PAUSE/FOR/COPY/DEL/MD/RD/REN/TYPE/COMMAND /C…） |
| 音 | GMPV3 Studio Core の OPNA レンダラ（ymfm）＋自前合成のリズム ROM、ビープ |
| 文字 | 漢字 ROM の代わりに Windows の MS ゴシックを必要な字だけその場で描画 |

ゲーム付属のサウンドドライバ（PMD / PLAY5 / MMD 等）はバッチどおり本当に常駐させ、ゲームはそれを呼んで鳴らします。

## PC98PLAYER.INI

```ini
[PC98PLAYER]
Start=KAKYU.BAT      ; 最初に実行するプログラム/バッチ（必須）
Args=                ; 引数
Scale=2              ; 表示倍率（1 で 640x400）
FullScreen=0         ; 起動時に全画面
Smooth=0             ; 拡大をなめらかに
CpuMHz=16            ; 仮想 CPU の速さ
Drive=A              ; ゲームのフォルダを何ドライブに見せるか
SoundBoard=86        ; 86 / 26 / 0（なし）
SoundIRQ=12          ; 3 / 10 / 12 / 13
MIDI=0               ; 1 で MPU-PC98II（E0D0h）を載せ、Windows の MIDI 出力へ送る
MidiDevice=-1        ; MIDI の出力先（-1 = 既定）
EMS=1                ; EMS（EMM386 相当）。EMSKB=4096 で容量
XMS=1                ; XMS（HIMEM.SYS 相当）。XMSKB=8192 で容量
Volume=100           ; 全体音量 %（FMVolume / SSGVolume / BeepVolume / PCMVolume もあり）
MouseSpeed=100       ; マウスの速さ %
Title=               ; 窓のタイトル（空ならフォルダ名）
Font=ＭＳ ゴシック    ; 漢字に使うフォント
KanjiJIS=78          ; 78=PC-98 と同じ旧 JIS の並び / 83=新 JIS
ExitOnEnd=1          ; ゲーム（バッチ）が終わったら窓を閉じる
PauseInactive=0      ; 非アクティブ時に止める
RhythmROM=           ; 本物のリズム ROM（任意。無くても鳴ります）
```

INI は Shift_JIS でも UTF-8 でも構いません。`samples\` に 4 本ぶんの例があります。

## 操作

| Windows | PC-98 |
|---|---|
| 英数字・記号キー | 物理位置どおり（JIS 配列基準） |
| F1〜F10 | f･1〜f･10 |
| Alt | GRPH　／ カタカナひらがな = カナ ／ 変換 = XFER ／ 無変換 = NFER |
| Home / End | HOME CLR / HELP |
| PageDown / PageUp | ROLL UP / ROLL DOWN |
| Insert / Delete | INS / DEL |
| Pause または ScrollLock | STOP ／ PrintScreen = COPY |
| 窓をクリック | マウスを捕まえる（PC-98 のバスマウスとして動く） |
| F12 / 中クリック | マウスを放す |
| **Shift+F11** | **ステートセーブ**（スロット選択画面） |
| **F11** | **ステートロード**（スロット選択画面） |
| Shift+F12（押している間） | 早送り |
| Alt+Enter | 全画面の切替 |

## ステートセーブ／ロード

どの場面でも、その瞬間の PC-98 まるごとを保存・復元できます（ゲーム側にセーブ機能が無くても使えます）。

- **Shift+F11** で画面が暗くなり、8 個のスロットが縮小画像つきで並びます。**F11** なら読み込み用の同じ画面です
- カーソルキーまたはマウスで選び、**Enter / 左クリック** で決定、**Esc / 右クリック / F11** でやめます
- 保存済みのスロットは上書きします。空のスロットは読み込めません（その旨を表示）
- 選択画面を開いている間はゲームが止まります
- 保存先はゲームのフォルダの `PC98PLAYER.SAV\SLOT1.P98S`〜`SLOT8.P98S`（1 個約 1.4MB）。窓を閉じても残るので、次に起動したときタイトル画面から読み込めます

保存するもの: CPU・メモリ全体・VRAM・各デバイス、常駐ドライバを含む DOS の状態（開いているファイルは位置ごと開き直す）、
バッチの実行位置、そして **FM 音源チップの内部状態**（鳴っている音の余韻まで）。
検証では、保存した時点から続けた映像と、読み込み後に続けた映像が 1 ビットも違わず一致しました。

## 擬似漢字 ROM

PC-98 の漢字 ROM・ANK ROM を、**本物の ROM も第三者のビットマップフォントも使わずに**組み立てます（`core/fontrom.cpp`）。

| 範囲 | 作り方 |
|---|---|
| JIS 非漢字・第 1/第 2 水準、NEC 特殊文字（13 区）、NEC 選定 IBM 拡張 | MS ゴシックをそのまま描く |
| ANK 0x20-0x7E・半角カナ | MS ゴシック（0x5C は ¥） |
| ANK の 年月日時分秒円、10 区の ヰヱヮヵヶ | MS ゴシックを **幅 8 ドットに縦長で**描いて半角化 |
| 10 区の半角 ガ〜ポ・ヴ | 半角カナに **濁点・半濁点を合成** |
| 11 区の半角 〔〕〈〉《》『』【】 | 全角の字形から字のある位置で **8 ドット切り出し** |
| JIS 8 区・PC-98 の 11 区（半角）・12 区（全角）の罫線（U+2500〜254B の 76 字） | Unicode の字名から腕の太さ・点線を読み取り **幾何学的に描画** |
| ANK 0x80-0x9F・0xE0-0xFF（ブロック・罫線・三角・丸・斜線）、制御コード略号、矢印 | **幾何学的に描画** |
| ♠♥♦♣ | 自作ドット絵 |
| 外字（76・77 区） | RAM として保持。CG ウインドウ・ポート A9h・INT 18h AH=1Ah の書き込みを反映（ステートセーブにも含む） |

- PC-98 と同じく旧 JIS（1978）の並びで引きます（1983 年改正で入れ替わった 26 組を入れ替え）。`KanjiJIS=83` で無効
- テキスト画面の **簡易グラフ**（属性 bit4、モード F/F で切替: 2x4 ブロック）に対応
- テキスト VRAM の全角右半分は「直前のセルが全角の左半分だったか」で決める（実機どおり）
- 外字は 76・77 区の第 2 バイト 00h〜7Fh の 128 字ずつ（東方の得点・残機表示が使う 21h 未満も含む）
- 字形の確認: `PC98PLAYER.EXE --fontsheet` で `PC98FONT.BMP` に一覧を書き出します

## 動作確認

| ゲーム | Start= | 結果 |
|---|---|---|
| 下級生 | KAKYU.BAT | NMD/PLAY5 常駐 → ロゴ → 名前入力画面まで。キー・マウス操作可 |
| 同級生2 | NANPA2.BAT | FREE/AMD/PLAY5 → ロゴ（FM でジングル再生）→ メニュー → プロローグ確認まで |
| 瑠璃色の雪 | RURI.COM | MMD 失敗 → PMDB2 に切替・常駐 → AIL ロゴ → メニュー → 本編テキストまで |
| LEGAM | LEGAM.EXE | PMD/MMDR 常駐 → タイトル → 新規開始 → マップ・会話・BGM（PMD）再生 |
| FLEIA | FLEIA.BAT | NA 常駐 → タイトルメニュー（テキスト VRAM の全角文字） |
| 親父王 | OYAJIO.EXE | 簡易グラフで描くシューティングの画面 |
| 東方封魔録 体験版 | GAME.BAT | ZUN.COM で 86 ボードと判定 → PMD86 → タイトル → ゲーム本編（得点・残機の外字表示） |
| 東方夢時空 お試し版 | GAME.BAT | PMD86 → タイトル → キャラ選択 → 対戦（200 ライン表示・隠し VRAM のスプライト） |
| 東方幻想郷 体験版 | GAME.BAT | PMD86 → 音源選択 → キャラ選択 → 本編（EGC・GDC スクロール・右端のタイル置き場を黒テキストで隠す） |
| 東方怪綺談 体験版 | GAME.BAT | PMD86 → 本編 → コンティニュー画面 |

実行速度は検証機で実時間の約 10 倍（描画・音込み）なので、普通の PC で余裕があります。
ステートセーブは東方幻想郷のゲーム中でも、保存時点から続けた映像と読み込み後の映像が一致することを確かめています。

### 東方 Project（PC-98 版）で直したこと

- **EGC** を実装（公開資料の仕様から独自に実装）。ZUN のゲームは EGC で画面を描く
- **黒の文字は不透明**。ZUN のゲームはテキスト画面の黒・反転の空白で、グラフィックの余白（タイルの置き場）を隠す
- DOS ワークエリア **0000:0712h（テキスト行数-1）** を用意。master.lib の text_fillca などが参照する
  （以前は DOS のデータ領域が 0600h〜を上書きしていた）
- **ONGCHK / ZUN.COM の音源判定**: 86 ボードは A460h bit0 が 0 の間、拡張ポート 18Ch/18Eh が見えない（実機どおり）→ 86 と正しく判定
- バッチの **`IF ERRORLEVEL==5`** のように `=` でつながった書き方を受け付ける（東方の GAME.BAT がこう書く）
- **INT 18h AH=42h** の 200 ライン表示で、VRAM のどちらの半分を表示するかを正しく（東方夢時空は前半 200 ラインをスプライト置き場に使う）
- **INT 18h AH=0Ah** でテキスト属性 bit4 の意味（縦線/簡易グラフ）を切り替える（同級生2 の縦縞もこれで解消）
- 外字の第 2 バイトが 21h 未満の字（得点・残機の表示）
- **PC-9801-86 の PCM**（FIFO・割込み）。PMD86 は PCM の割込みフラグを見て待つので、無いと止まる
- PMD が List of Lists から **SFT** を辿るので、鎖を正しく終わらせた

## 手元に無いソフトについて（調べた範囲）

ゲーム本体は入手せず、Internet Archive のマニュアル・資料等から要件を調べ、足りない機能を足しました。
実物での確認はできていないので、動かない場合は PC98PLAYER.INI に `Trace=1` を書くと、ゲームのフォルダに `PC98PLAYER.LOG`（起動したプログラム・未対応の DOS ファンクションや I/O ポート）が残ります。

| ソフト | 分かったこと | 対応 |
|---|---|---|
| RPGツクール Dante98 / Dante98 II | II は 386 以上・1.6MB 以上・HIMEM.SYS を使う起動ディスク・FM 必須（26K/86/118） | **XMS**、マウス、OPNA |
| スタープラチナ（カスタム, 1996） | PC-9801VM 以降の 16 色・FM・右クリックで進む | FCB 系 DOS ファンクション、マウス |
| 雛鳥の囀（STUDiO B-ROOM, 1997） | FM 版と MIDI 版（SC-88 推奨）がある | **MPU-PC98II**（`MIDI=1`）、EMS/XMS |
| Steam-Heart's（GIGA, 1994） | 570KB 以上の空き・HDD では HDSH.BAT・26K FM ＋ MPU-PC98 の MIDI・ジョイスティックは FM 音源のポート | バッチの COPY/FOR/MD 等、MPU-PC98II |
| 銀河英雄伝説 IV EX | 286 以上・HDD 必須・FM（86/26K）と MIDI（MT-32/SC-55）・独自の起動ディスクを作る | MPU-PC98II、FCB、COMMAND /C |

※ 「銀河英雄伝説 VI EX」は見つからず（VI は Windows 専用）、PC-98 の **IV EX** のことと判断しました。

## まだ無いもの（今後）

- **GDC の描画コマンド**（LINE/ARC 等）、LIO（N88-BASIC 系）
- ADPCM（86 ボード＋ちびおと、SPB 等の ADPCM RAM）
- MPU-PC98II のインテリジェントモードの演奏機能（UART モードと、D0h 系の直接送信・問い合わせのみ対応）
- 保護モード（DOS エクステンダ、VCPI/DPMI）。EMS/XMS は実モードの範囲で使えます
- 256 色（PEGC）、31kHz 表示、フロッピーからの起動・INT 1Bh のディスク読み書き
- ジョイスティック

## セキュリティソフトの警告について

PC98PLAYER.EXE には電子署名がなく、mingw-w64 で静的リンクしているため、Windows Defender などが
機械学習ベースの判定（例: `Trojan:Script/Wacatac.H!ml` の「!ml」は機械学習による推定の印）で誤検知することがあります（ネット接続・他プロセスへの干渉・キー入力の記録などは一切しません）。
この版では誤検知を減らすため、バージョン情報・アプリケーションマニフェスト（asInvoker）・アイコンを埋め込み、
シンボルの削除（strip）をやめています。それでも警告が出る場合は、同梱のソースから Visual Studio でビルドした
EXE を使うか、Microsoft の誤検知報告（https://www.microsoft.com/wdsi/filesubmission）に提出してください。

## ビルド

- Visual Studio 2026：`tools\fetch_ymfm.cmd` を実行 → `PC98PLAYER.sln` を開いて `Release|x64`
- CMake：`cmake -S . -B build && cmake --build build --config Release`
- mingw-w64（Linux からのクロス）：`build_win.sh`
- `tools/harness.cpp` は窓を出さずに走らせて画面(PPM)と音(WAV)を書き出す検証用（Linux）

ソースは UTF-8。MSVC では `/utf-8` を指定済みです。

## 権利

本体は MIT。ゲーム・ROM・フォントは一切同梱しません。FM 音源は ymfm（BSD 3-Clause, Aaron Giles）を使います。
他のエミュレータのソースコードは使っていません（EGC・PCM86・XMS/EMS・MPU なども独自実装）。
`core/opna_renderer.*` と `core/rhythm_rom.*` は GMPV3 Studio Core から流用しています。
