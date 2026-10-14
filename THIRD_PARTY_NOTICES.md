# AASAPIのライセンスと第三者表記

本体はGPL version 3（[LICENSE](LICENSE)）

| 対象 | 採用条件 | 同梱資料 |
| --- | --- | --- |
| ASIO SDKのヘッダー | デュアルライセンスのGPLv3側 | vendor/asio/LICENSE.txtとヘッダー内の原表記 |
| miniaudio 0.11.25 | MIT No Attribution（MIT-0）側 | vendor/miniaudio/LICENSEとヘッダー内の原表記 |
| LLVM-MinGWと静的リンクランタイム | コンポーネント別の条件確認は未完了 | vendor/sources.jsonに取得版・ハッシュを記録 |

依存ソースの固定版・ファイルハッシュはvendor/sources.jsonを参照。第三者の原ライセンスと著作権表記を保持する

ASIO is a registered trademark of Steinberg Media Technologies GmbH.

AASAPIはSteinbergやMicrosoftの公式製品・認定製品ではない。ソースコードの利用条件と商標の使用条件は別。名称の商標クリアランスが完了したとは主張しない

メーカー純正ドライバーは同梱せず、利用者がWindowsへ導入済みのものを使用する。配布ZIPには対応ソースとビルド入口を含める。実行ファイルを第三者へ配布する前に、実際にリンクしたランタイムを含むライセンス条件を確認する必要がある
