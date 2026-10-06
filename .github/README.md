# PC98PLAYER（非公式ミラー）

[PC98PLAYER](https://i486.mods.jp/ichild/pc98player) の**非公式**のミラーです。本家の配布ソースをそのまま取り込み、次の 2 つを足しています。

- **Windows ARM64 版**：本家の Win32 版を ARM64 向けにビルドしたもの（x64 版は配布しません。本家の配布物を使ってください）
- **macOS／Linux 版**：SDL3 で組み直したフロントエンド（`sdl/`）

**このリポジトリのビルドについての問い合わせは、このリポジトリの Issues へお願いします。**ただし手を入れているのは SDL3 版（`sdl/`）と ARM64 版のビルドだけで、エミュレーションの本体（`core/`）は本家のままです。本体に原因がある問題は、こちらでは直せないことがあります。

使い方・INI の書き方・マニュアルは本家と同じです（同梱の `manual/index.html`、または本家のページを見てください）。

## ダウンロード

GitHub Actions のビルド（[Actions](../../actions) の各実行の Artifacts）から取れます。

| ファイル | 中身 |
|---|---|
| `PC98PLAYER-windows-ARM64` | 本家の Win32 版（ARM64） |
| `PC98PLAYER-macos-universal` | SDL3 版（Apple Silicon／Intel）。Apple の公証を受けていないので、初回は右クリック →「開く」 |
| `PC98PLAYER-linux-x86_64` | SDL3 版 |

## SDL3 版（macOS／Linux）

本家の Win32 版と同じ INI・同じ使い方で動くように、窓・音・入力の部分を SDL3 で作り直したものです。エミュレーションの本体（`core/`）は本家のまま共有しています。

本家の機能のうち、SDL3 版に入っているもの：

- ゲームのフォルダを選んで起動（起動時にフォルダを選ぶ画面。macOS は `.app` へのドロップでも）
- ハードディスクイメージの展開（イメージの横に同じ名前のフォルダを作って展開し、そのまま起動）
- ブートモード（`Boot=FD`）と 2 台目のドライブ
- F11 の画面（ステートセーブ／ロード、フロッピーの入れ替え・書き込み禁止、プログラム再起動、画面＆サウンド設定）
- メモリエディタ・コードエディタ（別の窓に PC-98 の字で描く）
- ゲームパッド、早送り、`MouseLockDisable=2`

本家と違うところ：

- フロッピーからのインストール支援（インストールアシスタントの画面）はありません
- メモリエディタ・コードエディタ・画面＆サウンド設定の入力欄は IME を通しません。コードエディタの日本語のコメントは Ctrl+V の貼り付けで、設定のフォント・SoundFont の欄はファイルを窓に落として入れてください
- MIDI は、ホストの MIDI 出力（Windows だけ）か、`MidiSoundFont=` に書いた SoundFont（.sf2。どの OS でも）で鳴らします
- `Font=` は、Windows では書体名、macOS／Linux ではフォントファイルのパスです（空なら既定のフォント）

**状態：プレビュー**です。普段の作業で動作を確かめているのは Windows（ARM64）上の SDL3 版だけです。macOS／Linux は GitHub Actions でビルドが通ることまでで、動かしての確認はしていません。ゲームパッドは未確認です。

## ブランチ

- `upstream`：本家の配布ソース（`PC98PLAYER_source_YYMMDD.zip`）をそのまま取り込んだもの。取り込むたびに `src-YYMMDD` のタグを打ちます
- `main`：`upstream` に、SDL3 版（`sdl/`）・ビルドの仕組み（`Makefile`・GitHub Actions）・この README を足したもの

## ビルド

- Windows：`cmake -S . -B build -A ARM64`（または `x64`）→ `cmake --build build --config Release`
- macOS／Linux：`make`（SDL3・FreeType はシステムのもの）、配布用は `make dist`（SDL3・FreeType をソースから静的に埋め込む。macOS は `ARCH=universal` も可）

ymfm はサブモジュールです（`git clone --recursive`）。

## ライセンス

本家と同じ MIT License（`LICENSE`）。同梱しているもの：ymfm（`THIRD-PARTY-ymfm-LICENSE.txt`）。SDL3 版の配布物には、追加で SDL3（zlib License）・FreeType（FreeType License）・TinySoundFont（MIT License）が入ります（それぞれ `THIRD-PARTY-*-LICENSE.txt`）。
