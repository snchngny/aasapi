# AASAPI

Windowsで複数のオーディオ機器を接続する、C++製のオープンソース音声ルーター

主機器の純正ASIO入出力とクロックを使用し、追加のWASAPI機器を同期させる。入力×出力のマトリクスで、各チャンネルを独立して接続できる

## 機能

- 純正ASIO 1台を主機器にし、追加のWindows入力・出力と端子ごとのPC再生音を接続
- ルーティング、音量・ミュート、機器設定、診断、出力別遅延を1つのGUIで操作
- バッファ量とリサンプリングによる機器間クロック差の補正
- マイクで試験音を測り、出力ごとの遅延を自動補正。残差を最大5回追加補正し、確認中央値の差0.1ms以内を目標にする
- 仮想ASIO DLLで合計入出力をDAWへ公開する実験的機能
- GUIとCLIによる診断・校正、Windowsログイン時の常駐復元

Windows x64、C++20。合計各128チャンネル、追加端子は方向ごと最大8。複数の純正ASIOドライバーを同時に集約する構成は未対応

## ビルドと起動

PowerShellで、このリポジトリを作業フォルダにする

```powershell
# 固定版LLVM-MinGWを一時フォルダへ取得（初回のみ）
./scripts/setup-toolchain.ps1
./scripts/build.ps1

# 個人の端子情報を含まない初期設定を作成（初回のみ）
Copy-Item config/aggregate.example.ini config/aggregate.ini
./build/aasapi.exe
```

初期設定のコピーは既存設定のある環境では不要。GUIの「機器設定」で主ASIOと追加機器を選び、「ルーティング」で接続して「音声を開始」を押す。Windowsの既定端子やメーカー側ミキサーの設定は別に管理する

独自のLLVM-MinGWがある場合は ./scripts/build.ps1 -Compiler <clang++.exeのパス> を指定できる。依存版とハッシュはvendor/sources.json、ライセンスは[第三者表記](THIRD_PARTY_NOTICES.md)を参照

## CLIとテスト

```powershell
./scripts/test.ps1
./build/aggregate_host.exe list
./build/aggregate_host.exe probe config/simulation.ini 2 2

# GUIで選択・保存したマイクと出力を使う。試験音が鳴る
./build/aggregate_calibrate.exe --session config/aggregate.ini config/router.txt build/results/calibration-cli.json -12 60 --apply
```

テストは人工機器で行い、通常のテストでは実機を鳴らさない。機器列挙・実機診断・校正ログには端子ID等が含まれるため、そのまま公開しない

詳しいルーティング・常駐・DAW登録・校正・復旧方法は[利用手順](docs/portable-usage.md)を参照。既存のaggregate_gui.exe、aggregate_router.exeも起動入口として利用可能。設定・登録CLSID・内部識別子は互換性のため維持する

## 現在の制約

開発中のソフトウェア。機器やドライバーによる相性、長時間動作、抜き差し後の復帰は環境ごとの検証が必要。PC再生音ループバックの停止・再開時に診断警告が出る場合がある

自動校正は実際に試験音を拾えるマイクが必要。0.1msは確認測定の中央値同士の目標であり、常時の差や完全な0msを保証しない。測定不良・未達・中止時は保存済み補正を維持する。新しい0.1ms目標は人工信号で検証済み、実機での到達確認は未完了

遅延補正は本ソフトから送る音に適用する。Windowsや別アプリが直接機器へ送る音には適用されない。各機器への同じ音を本ソフト経由でルーティングする必要がある

## プライバシーと配布

実機設定はconfig、診断とパッケージはbuildへ保存する。Gitでは汎用の初期設定と人工機器設定だけを追跡し、個人の端子ID・ルーティング・常駐状態・音声・ログ・ビルドを除外する

このリポジトリはソースの公開用。署名済みインストーラーやバイナリReleaseは提供していない。scripts/package.ps1でローカルパッケージを作成できるが、静的リンクランタイムの配布条件確認は未完了

## ライセンス

GPLv3（[LICENSE](LICENSE)）。ASIO SDKはデュアルライセンスのGPLv3側、miniaudioはMIT-0側を使用する。第三者の著作権・ライセンス表記を保持する

ASIO is a registered trademark of Steinberg Media Technologies GmbH.

AASAPIはSteinbergやMicrosoftの公式製品・認定製品ではない。名称の商標登録やクリアランス完了を主張しない
