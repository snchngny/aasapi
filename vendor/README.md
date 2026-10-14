# 使用するOSS

確認日: 2026-10-09。ソースはcommit固定、個別ファイルのSHA256は `sources.json` に保持

| 依存 | 用途 | ライセンス・取得元 |
| --- | --- | --- |
| Steinberg ASIO SDKのヘッダー3点 | Windows x64 ASIO ABI | GPLv3を選択。`asio/LICENSE.txt`。[取得ミラー](https://github.com/audiosdk/asio)、[開発元](https://www.steinberg.net/developers/) |
| miniaudio | WASAPIの列挙・音声入出力・形式変換 | 同梱MIT-0を選択。`miniaudio/LICENSE`。[開発元](https://github.com/mackron/miniaudio) |
| LLVM/MinGW 20261006 UCRT x64 | ビルド用。コンパイラ自体はOS一時領域へ配置 | 静的リンクするランタイム等の通知は `toolchain-LICENSE.txt`。[配布元](https://github.com/mstorsjo/llvm-mingw) |

AASAPIのコードは `../LICENSE` のGPL-3.0-onlyを適用。ASIO商標・メーカー製ドライバーの権利はそれぞれの権利者に帰属する

JACK・DAMC・FlexASIOのソースは組み込んでいない。今回必要な仮想ASIOと複数機器の同期処理を小さく検証するため、SDK・音声入出力ライブラリを再利用した
