# D-Bus接続失敗の修正版deb・VM検証

2026-11-14。[公開前レビュー](pre-release-review.md)の修正を含む **0.1.0~dev5-1の作成とUbuntu 26.04 VM検証は完了**。その後、[同じ候補の実機更新・今回のOrca確認](dev5-physical-orca.md)も完了した。実機の導入版はdev5-1。初回文字読み上げの入力条件不明という観測は残り、実データ利用の合格判定・一般公開は行っていない。

## 固定した成果物

保存先：`build/dist/session-guard-candidate/`。以前のdev4の成果物は維持した。

| ファイル | SHA-256 |
| --- | --- |
| `keepassxc-gnome-development_0.1.0~dev5-1_amd64.deb` | `5f9cefee29a1bcb8c84df062f0c319b7ff065374f7dc82cba82110590a7cf3fc` |
| `keepassxc-gnome-development_0.1.0~dev5-1.source.tar.xz` | `2b3eb463548dc75cd0194dff963a2752288c25977e047027bf3f7b8097d04d72` |
| `keepassxc-gnome-development_0.1.0~dev5-1_amd64.manifest.json` | `b9515be36e206b7acdb2d2813d8fd1b4049c7270741924519f6a37a85a088007` |
| `upgrade/keepassxc-gnome-development_0.1.0~dev5-2_amd64.deb` | `db17a583ff31d3aa4f6c7723b6511a03cd7c861afd7ac345966f360f2df3ce8b` |

dev5-2は更新経路の試験用で、3実行ファイルのhashはdev5-1と全て同一。両フォルダーの `SHA256SUMS` はそれぞれdeb・対応ソース・manifestを含む。本書は候補を固定して検証した後の記録であり、固定済みの対応ソースやmanifestを書き換えていない。

| 配置済み実行ファイル | SHA-256 |
| --- | --- |
| `keepassxc-gnome` | `bdf47cea7154ce0e2b58c794a53aa7f9427ea0dd9dbf7b0767bec43d5229239d` |
| `keepassxc-gnome-worker` | `4eb7fc9a6aaff543f968486ef1d9851f17344c5da57904aaf784f486f40c8762` |
| `keepassxc-gnome-picker` | `cc9b74490cbc44e521a61dd74edf2f920ac5c6f6dfffb80548088052b58b83fd` |

上流は従来どおりKeePassXC 2.8.0 / `1c2f23ef5914c0f442a00533def1689857afb5a7`。開始時のGit HEADは `58313987d07edbed78f9e5420524115acc5decf1` で、前回のD-Bus修正と今回のGUI試験等は作業差分を含む。完全な対応ソースのhashで候補の内容を特定する。workerとpickerのhash、依存要求はdev4と同じ。本家の参照checkout・暗号・KDBX・保存処理へ追加変更はない。

## 変更と回帰試験

製品の変更は、初回D-Bus接続でproxyを作れなかった場合にDB作成・解除を禁止する処理（`src/gnome/SessionGuard.cpp:121`）。3通りの接続失敗の単体検査（`tests/lifetime_tests.cpp:49`）に加え、今回は実際の製品GUIでの受付も検査した。

`tools/test-ui.sh:21` の `--guard [配置先の実行ファイル]` は、GNOME・AT-SPI・portalを動かす私有バスを維持しつつ、**試験対象アプリだけ**のsystem busを存在しないsocketへ向ける（同ファイル `:131`）。製品に試験用の認証回避は追加しない。

`tests/ui_guard.py:35` は対象PIDの画面だけを読み取り、警告表示、作成・開くの両ボタンの無効、認証入力欄・DB workerの不在を確認する。アクセシビリティ経由で操作を試みた後も3秒間確認する（同ファイル `:64`）。日本語・英語で成功した。`tools/check-release.sh:11` の共通入口にもguard検査を加えた。

## 梱包・対応ソース

| 検査 | 結果 |
| --- | --- |
| deb内容と配置後の動作 | 独立名・配置・相対リンク・locale・ライセンス・依存・補助プログラムを確認。PIE / RELRO / NOW / 非実行stack、RPATH・ASan・set-IDの不在が成功。配置後のworkerで合成DB操作、picker検査が成功 |
| 対応ソースの照合 | 固定した上流ツリーとパッチ、Git情報のないソースを検証。アーカイブ内の修正・寿命検査・guard検査・GUI harnessが候補作成時のファイルと一致 |
| 独立した再ビルド | 対応ソースから `build/source-rebuild.i83Dg4/` に再構築し、CTest **22/22成功（27.29秒）** |
| 同じ入力の梱包一致 | `repro/` に再度梱包し、source・deb・manifest・checksumsが全てバイト一致。異なるパスや環境でのELF一致の主張ではない |

ログ：`.cache/dev5-{package-build,upgrade-build,package-test,source-rebuild,repro,guard-ui,guard-en-ui}.log`。梱包には既存のusr-mergeに伴う `dpkg-shlibdeps` 警告があり、依存算出の失敗を無視する設定は使っていない。

## Ubuntu 26.04 VM

対象VMは `ubuntu26.04`。停止中に `keepass-gnome-before-dev5-20261114` スナップショットを追加して起動し、guest agentからUbuntu 26.04.1 LTS / x86_64、kernel 7.0.0-38を確認した。

1. 転送した2つのdebと試験ツールのSHA-256が一致した。APT計画は開発アプリ1件のみで、**追加依存・他パッケージの更新・削除はない**。
2. `tools/vm-package-test.sh:1` によりdev5-1導入、配置後のworkerを使った合成DBの作成・編集・検索・履歴・保存・ロック、dev5-2への更新、purgeが成功。合成config / state sentinel、本家パッケージの照会状態が不変。
3. dev5-1を再導入して3実行ファイルをmanifestと照合。一般ユーザー `yoshimi` の私有GNOME / Waylandから **`/usr/bin/keepassxc-gnome`** を操作し、日本語のDB・エントリー・グループ作成、編集、検索、破棄確認、ロックが成功。
4. 同じ配置先の製品で、日本語・英語のguard検査が成功。警告が表示され、作成・開くは無効で、アクセシビリティ操作でも認証フォーム・workerを開始しなかった。
5. VMの実行時ライブラリと配置先worker、無変更の本家CLIを使う寿命検査も **103チェック成功**。このVM実行は通常ビルドであり、ASan / LSanの実行とは別である。
6. 開発アプリだけを再度purge。導入前後の**全パッケージ名・版・状態**の一覧がバイト一致し、配置先アプリのプロセスが無いことを確認。照合SHA-256は `59e45b2d14e4c0511b423105c87adae1d57e8cf650aaca213194564c27a73333`。
7. guest agent経由で通常シャットダウンし、`virsh domstate` で **shut off** を確認。既存・今回のスナップショット、合成ログは保持。スナップショット復元や依存のautoremoveは行っていない。

実行版はGTK 4.22.4+ds-0ubuntu0.1、libadwaita 1.9.1-0ubuntu0.1、Pango / PangoFT2 1.57.0-1、Cairo 1.18.4-3、Fontconfig 2.17.1-3ubuntu1。libadwaitaは実機・開発prefixの0ubuntu0.2と異なる。

GUIは私有セッションであり、通常ログインのportal・物理GPU・実際の画面ロック／スリープ・音声聴取を確認した結果ではない。通常GUI試験は実system busへスリープ要求等を送らない。GNOME Shellの私有logind不在、portalのAccessDenied、EGL / Zink、AT-SPI初期cache等の警告をログに保持した。表示したGUIと操作の判定は成功しているが、警告なしとは報告しない。本家 `keepassxc` のパッケージはVMでは元から未導入で、前後状態の不変を本家GUIとの併存試験済みとは扱わない。

## 試験基盤で発生した停止

製品を変更せず、以下の試験側の問題を調整して再実行した。初回停止を成功へ読み替えない。

- GUIの役割名を最初は `push button` に限定していたため、`button` と報告するAT-SPIで待機が失敗した。既存のGUI試験と同様にbutton役割名を扱い、日本語・英語で再確認した。
- VMのroot作成ディレクトリが0700で、一般ユーザーの試験起動がexit126で停止した。今回の専用親ディレクトリだけをroot所有・グループyoshimi・0750に調整した。
- 寿命試験のCLI引数へfixture生成ツールを誤指定し、復旧コピーの本家CLI検査がexit2で失敗した。無変更の本家CLIを別に転送・hash照合し、正しい引数で103チェックを通した。
- purge計画の検査がAPTの `Purg` 行を扱わず、削除前に停止した。対象が開発アプリ1件だけである検査を維持して書式を調整し、purgeと全パッケージ状態の照合を完了した。

主な記録：`.cache/vm-dev5-{transfer,plan,package-result,gui-result,cleanup}.log`、`.cache/vm-dev5-state.json`、`.cache/vm-dev5-logs/`。初回エラーは同フォルダーの `lifetime-initial-*.log`、`.cache/vm-dev5-gui-initial.log`、`.cache/vm-dev5-cleanup-initial-plan-parser-error.log`、`.cache/dev5-guard-en-initial-failure.log` に保持する。VM側の試験ディレクトリは `/tmp/keepass-gnome-dev5.LOX8uN/`。

## 後続の実機確認

VMを通過した**同じdev5-1**へ実機を更新し、通常の監視開始・sleep猶予取得、日本語の解除・ロックと、5分経過後のOrca聴取を今回の操作範囲で確認した。導入直前に計画とhashを再照合し、追加依存なしで開発アプリだけを更新した。実機の入力条件と初回報告の切り分け、終了状態は [実機記録](dev5-physical-orca.md) を参照する。

libadwaitaの既知の解放漏れ、長時間反復の増加基準未達、外部クリップボードの保持等は [公開前レビュー](pre-release-review.md#残件の整理) の範囲を維持する。今回、長時間試験を再実行したり、未合格を合格へ変更したりしていない。本番DB使用・依存パッケージの追加・commit / push・公開は行っていない。
