# 非公開リリース候補RC1の固定・検証

2026-11-14。**共通リリース検査とUbuntu 26.04 VM検証を完了した非公開候補。GitHubへの公開は行わず、利用者自身の起動・操作確認後の改めての明示指示を待つ。** メール・セキュリティ報告先・署名方針等の未定事項は [公開用メタデータ](publication-settings.md) に残す。開発版の表示・識別子を維持し、実データ利用の最終受入済みとは表示しない。

## 成果物と出典

保存先は `build/dist/release-candidate-rc1/`。既存dev4・dev5は上書きしていない。

| ファイル | SHA-256 |
| --- | --- |
| `keepassxc-gnome-development_0.1.0~rc1-1_amd64.deb` | `ababa3df8a8222dff03fb1bebd4e3792cc84ecf5528de0193bb8e39869c7b095` |
| `keepassxc-gnome-development_0.1.0~rc1-1.source.tar.xz` | `e2336d462053dc61af45bb0eae13f91f793409c2aa52b9e1efc0fa6a71d09501` |
| `keepassxc-gnome-development_0.1.0~rc1-1_amd64.manifest.json` | `271f08c7f43168e5d2c7cb5c6892225d501bea0027c9c1005a8859671aa6b70d` |
| `upgrade/keepassxc-gnome-development_0.1.0~rc1-2_amd64.deb` | `801c56f454ec259bd2953e50fdd6bcba093e77bf2638f92add52855fe1f22631` |

rc1-2は版更新の試験用で、rc1-1と3実行ファイルのhashが一致する。署名は行っていない。完全ソース・manifest・SHA256SUMSを各版と対にして保持する。

基点のGitコミットは `a8e44b219156a54836661f6e3d620b26ebdb6af4`。その後の試験・文書・検査入口の作業差分を含むため、候補は完全ソースのhashで特定する。上流は2.8.0 / `1c2f23ef5914c0f442a00533def1689857afb5a7` のまま。製品コード・上流への追加変更はない。

この固定ソースは作成時点のスナップショット。後から受領したGitHub公開先・保守者、公開保留の指示、本書等の検証結果の追記は現在の作業ツリーで保持し、固定済みアーカイブへ書き込んでhashを変更しない。正式公開版は確定したmetadataと記録で改めて固定する。

## 共通リリース検査

実行：`KPXC_RELEASE_VERSION=0.1.0~rc1-1 KPXC_RELEASE_OUTPUT="$PWD/build/dist/release-candidate-rc1" bash tools/check-release.sh`。保存先は新規の空ディレクトリを使った。既存候補のある保存先を指定すると、処理前に拒否しファイルを変えない負の対照も成功した。

| 検査 | 結果 |
| --- | --- |
| 固定ソース・ビルド・CTest | 上流照合成功、通常22/22成功 |
| 通常GUI | 日本語CRUD、検索、キャンセル・クラッシュ、保存・復旧、寿命、guard警告の実サイズ、日英のpolishが成功 |
| 実Orcaの条件比較 | 日英それぞれ28チェック成功。通常欄・伏せ字・明示表示・再伏せ字を比較 |
| ASan | picker、GUIの寿命・polishの選択した検査が成功。GUI全体の生LSanゼロを意味しない |
| 梱包・配置 | 固定した対応ソース、出典、配置後worker / picker、locale、ライセンス、ハードニングが成功 |
| 完全な対応ソース | Git情報なしで `build/source-rebuild.jCxe5T/` に再構築し、22/22成功 |
| 梱包再現 | 同じ版・入力でsource / deb / manifest / checksumsが一致。`build/repro-check.DXgZ4P/`。異なる環境でのELF一致とは区別する |

全体の終了は0。ログは `.cache/rc1-check-release.log`。日英Orcaの結果は成果物フォルダーの `orca-ja/`・`orca-en/` にも保持する。独立した事前比較は [Orca条件比較](orca-echo-modes.md)、条件別の結果とhashは `docs/qualification/orca-echo-modes-2026-11-14.json`。

## VMの実導入

停止していた `ubuntu26.04` に `keepass-gnome-before-rc1-20261114` スナップショットを追加して起動し、Ubuntu 26.04.1 LTS / x86_64を確認した。転送した候補と試験ツールのhashも一致した。

1. APT計画は開発アプリ1件だけ。新規依存・他パッケージの更新や削除はない。
2. rc1-1導入、配置先workerによる合成DB操作、rc1-2への版更新、purge、合成config / state sentinel保持が成功。
3. rc1-1を再導入し、3実行ファイルがmanifestと一致。一般ユーザーの私有GNOME / Waylandで、配置先 `/usr/bin/keepassxc-gnome` の日本語CRUD・検索・ロックと、日英のguard検査が成功。
4. 配置先workerと無変更の本家CLIを使う寿命検査が103チェック成功。このVM検査は通常ビルドで、Orca比較はホストの私有セッションで別に行った。
5. 検証アプリだけを再度purgeし、全パッケージ名・版・状態が開始前とバイト一致。配置先アプリのプロセスも無い。照合SHA-256は `59e45b2d14e4c0511b423105c87adae1d57e8cf650aaca213194564c27a73333`。
6. guest agent経由で通常shutdownし、`virsh domstate` でshut offを確認。依存のautoremove・snapshot復元は行わず、既存・今回のsnapshotと合成記録を保持した。

VMはGTK 4.22.4+ds-0ubuntu0.1、libadwaita 1.9.1-0ubuntu0.1、Pango / PangoFT2 1.57.0-1、Cairo 1.18.4-3、Fontconfig 2.17.1-3ubuntu1。私有セッションのportal・GPU初期化・AT-SPI cache等の警告はログに残す。通常ログイン・物理GPU・実スリープをこのVM試験で確認したとは扱わない。

主なログは `.cache/vm-rc1-{transfer,plan,package-result,gui-result,cleanup}.log`、`.cache/vm-rc1-state.json`、`.cache/vm-rc1-logs/`。VM内の今回の記録は `/tmp/keepass-gnome-rc1.3gtPxN/`。

## 既存実機検証の引き継ぎ

RC1の3実行ファイルは実機確認済みdev5-1と全件同一。さらにdebの**実行用payload全体**を比較し、実行ファイル・翻訳・desktop・アイコン・symlink・権限の一致を確認した。比較から除いたものは `/usr/share/doc/` とDebian control / 版等のmetadata。記録は `.cache/rc1-runtime-payload.json`。依存要求も同一。

実機にはdev5-1を維持し、RC1パッケージへの更新は行っていない。同一実行内容の [dev5実機検証](dev5-physical-orca.md) を引き継ぎ、動作変更を伴わないため追加のOS認証・音声試験は求めなかった。利用者自身は現在のアプリで [起動・操作確認](manual-acceptance.md) を進められる。将来の正式metadataや識別子・動作を変更した版は差分を評価する。

## 公開まで

利用者自身の確認と、その後の改めての公開指示を待つ。公開予定は `NBE03xxx/keepassxc-gnome`、保守者は `NBE03xxx`。連絡用メールとセキュリティ報告先は未定。署名方針・公開用表示等も [メタデータ草案](publication-settings.md) で管理し、内部の `noreply@localhost` を公開連絡先にしない。

初回qzv読み上げの入力条件不明、長時間メモリ基準未達、依存の既知問題、clipboardの外部残留等は [残件判定](release-readiness.md) のまま。今回の試験成功だけで、全画面・全支援技術、任意のKDBX、長期常駐、実データ運用の安全性を無条件に保証しない。GitHub push・一般公開は行っていない。
