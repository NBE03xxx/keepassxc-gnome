# v0.1.0のT16導入とGitHub公開

2026-11-14。利用者の明示指示に基づき、T16へ公開版を導入した後、GitHub `NBE03xxx/keepassxc-gnome`へv0.1.0を公開する。保守者はNBE03xxx、Maintainerは `NBE03xxx <NBE03247@nifty.com>`。公開先はpublic・空のリポジトリで、既存リリースはないことを読み取り確認した。

## 範囲

READMEのロードマップを含む文書を最新にしたうえで、既定OFFの開発用表示フラグ、公開パッケージ名 `keepassxc-gnome`、版 `0.1.0-1` を維持する。アプリ内の開発版バナーは表示しない。内部IDと設定・履歴・復旧の保存場所は維持し、本家KeePassXCや実DBは変更しない。新規依存パッケージは不要。配布物は未署名。

実装・実行ファイルは既に公開用候補で検証済みのものを維持する。文書更新後の完全対応ソース・deb・manifest・SHA256SUMSを `build/dist/v0.1.0-publication/` へ新しく固定し、以前の候補を上書きしない。資料の前提と既知のメモリ等の制限は[公開準備記録](release-v0.1.0-preparation.md)とREADMEに維持する。

## 実施順序

1. 最終配布物の内容・ハッシュ・実装同一性を確認する。
2. Ubuntu26.04 VMで最終debの導入と日本語GUI基本操作を再確認し、検証アプリを削除して導入前の状態へ戻す。既存候補の全23試験・再ビルド・梱包再現・移行等の結果は、全実装と3実行ファイルの同一性を確認したうえで引き継ぐ。
3. 利用者が保存・終了したT16の旧開発版だけを公開パッケージへ移行し、本家・他のパッケージ・MIME既定設定の不変、3実行ファイル、版、公開名称・日本語catalogを照合する。DBを引数で渡さず起動する。
4. 公開するソースの構成を確認してcommitとv0.1.0タグを作成し、GitHubへpushする。
5. GitHub Private vulnerability reportingを有効化し、deb・完全対応ソース・manifest・SHA256SUMSを同じReleaseへ添付して公開する。
6. 公開先のタグ・アセット一覧・アセットのダウンロードSHA256を照合する。

GitHub連携ツールはリポジトリの参照に使用する。Release添付・非公開報告設定は連携のツールにないため、既存のGitHub CLIを利用する。認証トークンを表示・保存・添付しない。検証・実行の詳細ログは `.cache/` 内に保持し、同ディレクトリやbuild、upstream参照チェックアウト、実データはGitへ追加しない。


## 最終配布物とVM

次を `build/dist/v0.1.0-publication/` に固定した。READMEのロードマップ、TOTPの現状と予定、YubiKeyの扱いを含む。

| ファイル | SHA256 |
| --- | --- |
| `keepassxc-gnome_0.1.0-1_amd64.deb` | `b2d23a44db4ccd8066b9bc34294c96d7d8951961fd3b0dd7b13d29ae9356b661` |
| `keepassxc-gnome_0.1.0-1.source.tar.xz` | `0906f478ced24df983094d80db43c350a74279eef3aa30c001c34ae646d52e92` |
| `keepassxc-gnome_0.1.0-1_amd64.manifest.json` | `a138b56d30b5a6d7291f57907a16010b985139b29ca0c0aee4565d2339219331` |

3実行ファイルは以前の公開表示候補と同一で、ソース全ファイルの差分はREADME・docsだけであることを確認した。最終debの梱包検査（ライセンス、翻訳、独立した名称・関連付け、完全対応ソース、hardening、配置先worker/picker）が成功。先行候補の全23試験、クリーン再ビルド23/23、梱包再現、VM移行・日英GUI等の合格結果は実装同一性を根拠に維持する。

Ubuntu26.04 VMでこの最終deb自体の導入、3実行ファイル・表示版・同梱READMEの一致、配置先製品による合成DBの作成・編集・検索・グループ・ロックを再確認した。VM再起動で以前の一時スクリプトが消えていたため、スクリプトだけを再転送してGUI試験を実行した。製品修正は不要だった。試験後に検証アプリだけを削除し、全パッケージ状態が導入前と一致して対象プロセスが残らないことを確認した。VMには通常停止を要求した。

ログ：`.cache/v010-publication-source-comparison.json`、`build/v010-publication-package-tests.log`、`.cache/v010-publication-vm-logs/`、`.cache/vm-v010-publication-state.json`。公開するGitツリーと対応ソースの実装・ビルド・テスト入力は一致させる。凍結後のQA結果だけはGitのdocsに追記し、配布済みソースを自己参照するhashの循環を作らない。


## T16への公開版導入

利用者が保存・終了・OS認証の準備完了と回答し、その後OSの認証画面で認証して、T16を旧 `keepassxc-gnome-development` 0.1.0~dev11-1から `keepassxc-gnome` 0.1.0-1へ移行した。APTの変更は旧自作パッケージ1件の削除と公開版1件の導入だけで、新規依存なし。3実行ファイルと配布manifest、公開用の表示名、実行版0.1.0、日本語catalog、ロードマップを含むREADME、dpkgのファイル整合が一致した。

他の全APTパッケージ、本家KeePassXCのFlatpak commit、MIME既定設定、本家のユーザー用launcherと候補cacheは不変。document portalのfuse.portalマウントも維持している。DB引数を渡さず通常profileで日本語起動し、プロセスの継続動作を確認した。既存の本番DBを開いたり、利用者の画面や秘密を取得したりしていない。配置・起動の今回の確認と、前段の利用者によるdev11操作確認は区別する。

記録：`.cache/v010-publication-host/{install.plan,install.log,result.json,launch.json}`。ホストの実行ファイルSHA256は上記の公開候補と一致する。

## GitHub公開前の確認

対象リポジトリは `NBE03xxx/keepassxc-gnome`、public、初回のmain/ref・Releaseは空。GitHub CLIの認証アカウントはNBE03xxx、管理・push権限を確認した。GitHubのPrivate vulnerability reportingを有効化し、APIでenabled=trueを照合した。SECURITY.mdの非公開報告先はこのリポジトリを指す。

公開候補のパス208件と履歴のblob278件を対象に、キャッシュ・build・上流チェックアウト等の混入と、秘密鍵・代表的なトークンパターンを確認し該当はなかった。この検査は秘密情報の不存在を網羅的に証明するものではない。実行ログや認証情報はGitに追加しない。
