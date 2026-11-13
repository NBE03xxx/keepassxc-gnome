# Ubuntu 26.04・短時間個人利用向け候補の確認

2026-11-14。[公開対象](release-scope.md)に基づく最新版の確認。**VMと実機の画面・入力・ロック確認は完了。** 実機はdev4-1へ更新済み。利用者は画面操作のみ対応可能と回答し、今回Orcaの音声確認は行わず未検証のまま残す。GitHubへの公開・pushは行っていない。

## 固定した候補

- 候補：`build/dist/short-session-candidate/keepassxc-gnome-development_0.1.0~dev4-1_amd64.deb`
- deb SHA-256：`c0ea47f0fe1d228bfd58d22a29254dd9df8903bba84df4824576643b8e569b34`
- 対応ソース：同フォルダーの `keepassxc-gnome-development_0.1.0~dev4-1.source.tar.xz`
- ソースSHA-256：`c44b361a41cbb6e7c45f5a62ef7dd346f2a4a7bf218a7acfed4755c47652880b`
- 上流KeePassXC：2.8.0 / `1c2f23ef5914c0f442a00533def1689857afb5a7`。本家参照チェックアウトは変更していない。
- 更新経路検査用の `0.1.0~dev4-2` は同フォルダーの `upgrade/`。3実行ファイルのhashは候補と全件同一。

今回の製品動作の変更は、既に検証した `src/gnome/FontCache.h:21` と `src/main.cpp:298` の5分整理。今回の追加調査では製品の暗号・KDBX・保存処理を変更していない。対応ソースには調査用の独立対照も含む。`tools/build-deb.py` のmanifestにはPango / PangoFT2 / Cairo / Fontconfigの版も加えた。

## 保持量の説明と既知の制限

[保持の性質の調査](gui-retention-characterization.md)で、描画キャッシュの保持と回収、libadwaitaのbreakpoint解放漏れを区別した。後者は上流修正と一致するが、依存ライブラリへ独自パッチを適用していない。60分反復の増加基準の未合格も維持する。短時間の個人利用向けの候補として検証し、この既知問題を公開時の最終判断に含める。

## VMで確認したもの

既存VM `ubuntu26.04` は開始時に停止していた。復元用スナップショット `keepass-gnome-before-dev4-20261114` を追加して起動した。Ubuntu 26.04の判定と、転送後のSHA-256を確認した。新規依存の導入・他パッケージの更新は不要だった。

1. `tools/vm-package-test.sh` による候補の導入、同実装の版更新、purgeが成功。配置・locale・実行時依存、合成DB操作、合成の設定・復旧sentinel保持、元パッケージ状態の不変を確認した。
2. 同じ候補を再導入し、3実行ファイルのhashがmanifestと一致した。一般ユーザー `yoshimi` の私有GNOME / Waylandで、**実際の導入先 `/usr/bin/keepassxc-gnome`** の日本語DB作成・エントリー作成／編集・検索・グループ作成・破棄確認・ロックが成功した。
3. 同じWindowコードの通常ビルドprobeと、導入先からコピーした同一hashのworkerで、330秒指定の66巡＋30秒待機を完走した。実時間の5分整理1回、同期APIの最大所要時間 **4.941 ms** を記録した。反復の機能・秘密入力消去・元ファイル不変と終了を確認した。これはASanの生存割り当て計測ではなく、整理後の全フレーム時間の測定でもない。
4. 開発パッケージだけを再度purgeし、VMを通常シャットダウンした。agentの切断によりshutdown要求自体は非0を返したが、`virsh domstate` で `shut off` を確認した。強制停止・スナップショット復元・依存のautoremoveは行っていない。合成プロファイルとスナップショットは保持する。

VMの実行版は GTK 4.22.4+ds-0ubuntu0.1、libadwaita 1.9.1-0ubuntu0.1、Pango / PangoFT2 1.57.0-1、Cairo 1.18.4-3、Fontconfig 2.17.1-3ubuntu1。libadwaitaは実機・私有開発prefixの0ubuntu0.2とDebian revisionが異なるため、同一バイナリ版と扱わない。

GUIはVM内の私有sessionであり、通常ログインのportal・実機のGPU・音声聴取を検証済みとはしない。EGL / Zinkの初期化と私有portal等の警告はログに残る。最終実機確認では通常のデスクトップを使う。

本家 `keepassxc` はこのVMでは未導入で、実機のdpkg照会でも対象パッケージは見つからなかった。前後状態の不変を本家GUIとの併存操作の検証へ読み替えない。他形式の既存KeePassXCの場所・設定・DBを探索・操作せず、開発パッケージの独立した名前空間を確認する。

ログ：`.cache/vm-short-session-{result.log,result.json,installed-ui.log,timer-ui.log,installed-metadata.json,gui-result.json,gui-purge.log}`、`.cache/vm-short-session-logs/`。展開先の構成・対応ソース・worker・pickerの追加検査も成功（`.cache/short-session-package-test.log`）。

## 実機の準備時点

実機の導入済み開発版は `0.1.0~dev3-1`。導入計画では本候補への更新だけで、新規依存・他パッケージの更新・削除はない。過去に利用者が保存した合成DB `build/host-final.l6nr7R/fixtures/host-native.kdbx` のhashが元記録と一致した。実データは使用しない。

この状態から、OS認証による候補への更新、同じ合成DBでの5分経過後の表示・入力、ロックと終了、合成ファイル・元パッケージ状態の再照合を実施した。音声確認は利用者の画面操作のみ対応可能という回答により実施しなかった。

## 実機への更新と画面確認

利用者の画面操作可能の回答後、OSの認証を通じて `0.1.0~dev3-1` からVM確認済みの `0.1.0~dev4-1` へ更新した。追加依存・他パッケージの更新・削除はない。3実行ファイルが候補manifestと全件一致し、保存済み合成DBのhashも不変だった。`/usr/bin/keepassxc-gnome` を通常のデスクトップで起動し、状態・cache・dataは従来の合成試験専用ディレクトリを使用する。

最初の起動では、検証コマンドの `LANG=C.UTF-8` と `LC_ALL=C.UTF-8` を継承し、利用者から英語表示の報告があった。日本語ボタンを探す自動「開く」も取得に失敗していた。`/etc/default/locale` は `LANG=ja_JP.UTF-8` であり、プロセスの起動方法の問題と確認した。解除前に英語の画面を通常の閉じる操作で終了し、検証コマンドのLC_*上書きを外してOSのLANGを使った。`--language-check` が「準備完了」となることを起動前に確認して再起動し、利用者も日本語の解除画面を確認した。アプリの言語判定コード・システム設定は変更していない。

既知の合成パスワードで保存済みDBを解除し、既知の合成エントリーの編集画面を開いた。編集欄の自動取得は不安定で、今回も値の自動取得・設定を完了できなかったため、その試行を成功には含めない。利用者がタイトルを「キャッシュ整理の確認（未保存）」へ変更したことを記録した。

再起動後243秒時点で未保存の入力を記録し、346秒を超えた時点で利用者が次を確認した。

- タイトルの入力内容が残っている。
- 日本語の欠け・欄の見切れ・画面の崩れがない。
- タイトル末尾への a の入力とBackspaceによる削除ができる。

これは通常アプリの5分経過後の画面・入力の確認である。実機の製品プロセスへ計測用フックは追加しておらず、整理APIの実機呼出し回数・全フレーム時間を直接測定したとはしない。VMでの実時間タイマー・同一binary hashの結果と合わせて評価する。

確認後の読み取りでは配色は `prefer-dark`、文字倍率は1.2だった。設定値の書き換えは行っていない（`.cache/host-dev4-desktop-settings.json`）。

利用者がEscで未保存編集を閉じ、Ctrl+LでDBをロックした。自動照合でもエントリー・編集画面・password欄・workerが消え、元の合成ファイルhashが不変だった。その後こちらの閉じる操作でアプリを終了し、利用者も閉じたことを確認した。開発アプリのプロセスがないこと、導入先3実行ファイルのhash一致、dev4-1の導入状態、合成ファイルと元のパッケージ照会状態の不変を最終照合した。開発版は実機に維持する。

Orcaは起動せず、整理後の音声確認は未検証のまま残す。portal settings / monitorのAccessDenied警告と試験ドライバーのAT-SPI非推奨API警告も生ログに保持する。ログは `.cache/host-dev4-*.log`、`build/host-final.l6nr7R/app-installed-dev4{,-ja}.log`、`installed-dev4.json`、`dev4-manual-form-check.json`、`dev4-screen-result.json`。

## 今回の判定

短時間の個人利用向け候補について、今回のVM→実機の画面確認は完了した。新規依存・上流修正・本番DB使用・デスクトップ設定変更・commit / pushは行っていない。libadwaitaの既知の解放漏れ、長時間試験の未合格、整理後の実機Orca音声と全フレーム時間の未検証は維持する。今回の成功を日単位の上限保証や製品化の全項目の合格へ読み替えない。
