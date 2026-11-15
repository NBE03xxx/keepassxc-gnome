# v0.1.1の公開（2026-11-15）

T16へ導入したフォント調整版0.1.0-2を利用者が確認した後、v0.1.1としてリリースする明示指示を受けた。GitHub `NBE03xxx/keepassxc-gnome` へのcommit・タグ `v0.1.1` ・push・Release公開・配布物添付をこの指示に基づき実施する。

## 範囲と成果物

文字サイズの変更は `src/gnome/Appearance.h` / `src/gnome/SummaryList.h`。基本サイズ20%拡大、一覧ユーザー名のcaption縮小解除、ヘッダー・編集画面のsubtitleを同じ文字サイズにする。[実装・T16の確認記録](font-readability.md)に先行の検証を記録している。

今回の公開準備では、`CMakeLists.txt`、`src/main.cpp`、`po/ja.po`、`tools/build-deb.py` / `tools/check-release.sh`、`tests/package_tests.py`、README・利用ガイド・SECURITY.mdをv0.1.1へ揃える。暗号・KDBX・worker・picker・上流参照・新規依存・機能範囲への変更はない。

配布物は `build/dist/v0.1.1-publication/` にdeb `keepassxc-gnome_0.1.1-1_amd64.deb`、完全対応ソース、manifest、SHA256SUMSとして新規固定する。先行の `build/dist/v0.1.1/` は文書同期前の候補として保持する。既存のv0.1.0タグ・公開アセットとT16導入済み0.1.0-2は変更しない。リリース本文は [release-notes-v0.1.1.md](release-notes-v0.1.1.md)。既知のメモリ増加・未対応・未検証は解決扱いにしない。

## 手順

1. v0.1.1をビルドし、梱包・版表記・既存試験・日英GUI・対応ソース再ビルドと梱包再現を確認する。
2. Ubuntu26.04 VMで最終debの導入・0.1.0-2からの更新・GUI・削除・元のパッケージ状態への復旧を確認する。
3. 公開対象のファイルと資格情報パターンを確認し、ローカルcommit・注釈タグを作成する。GitHub mainが確認済みのHEADから進んでいないことを確認して通常pushする。
4. 下書きReleaseへ4件を添付して照合し、公開する。全アセットを再取得しSHA256を照合する。latest・タグ・非公開脆弱性報告の有効状態も確認する。

GitHub連携でリポジトリを確認し、Release添付・タグとpushには既存CLIを利用する。GitHub以外のremoteへpushしない。T16でのv0.1.1への再更新は今回行わない。確認済みのフォント変更そのものは導入済みであり、今回の依頼は公開である。

## 固定成果物と検証結果

公開物は以下のとおり。アプリ表示0.1.1、Debian版0.1.1-1、amd64。

| ファイル | SHA256 |
| --- | --- |
| `keepassxc-gnome_0.1.1-1_amd64.deb` | `49e5cb0f09787e032296a524ccf80d97ec8857c0f73ecf0fea628213c58f9ee5` |
| `keepassxc-gnome_0.1.1-1.source.tar.xz` | `700d00ebc193a05f5c966178c9c7311ed170771a477c9ac3fad8bd4b49a8428a` |
| `keepassxc-gnome_0.1.1-1_amd64.manifest.json` | `e35bc6ad9a6ed4b34cd2e49659ea3cf76a24d1474e69d51bc769d3f7d6937b6d` |
| `SHA256SUMS` | `0bf71d0ab21bed86d95730fa24966414f27d20ca503e6e3a9218a4a870a395c9` |

- 通常CTest 23/23、梱包検査、版・翻訳・hardening・配置先worker/picker、Gitなしの対応ソースconfigureが成功。
- 公開する完全対応ソースを `build/source-rebuild.NCjdoJ/` でクリーン再ビルドし、その23/23も成功した。
- 同一入力から2回作ったdeb・対応ソース・manifest・SHA256SUMSがバイト一致し、公開用の固定物とも一致した。独立ELFビルドのバイト再現を主張するものではない。
- プロジェクトの全213ファイルについて、タグ対象commitと完全対応ソースのパス・内容が一致した。worker/pickerはT16で確認済みの0.1.0-2と同じhash。
- 公開ファイルの棚卸しと秘密鍵・代表的なtokenパターン検査は該当なし。キャッシュ・build・ローカルGit・実DBを含めていない。秘密情報の不存在を網羅的に証明する検査ではない。

Ubuntu26.04.1 VMで、T16に導入したのと同じ0.1.0-2を導入し、最終0.1.1-1へ更新した。変更は本アプリ1件だけで、依存追加・他アプリの削除なし。配置先の版・3実行ファイルとmanifest・dpkg整合が一致し、製品GUIの基本操作、日英の明暗・狭幅・アクセント（polish各362 / 363）、日本語の実Return・メモ改行・終了時保存、公開歓迎102チェックが成功した。

初回のVM GUI検査は、青からtealへ切り替えた直後の保存ボタンのピクセル不一致で停止した。元のログを保存し、ホストのソース再ビルド完了後に製品・probe・合格基準を変更せず全GUIシナリオを再実行して成功した。描画／アニメーションのタイミングとの関係は推測であり、初回失敗を成功扱いに変更していない。

最後に検証アプリをpurgeし、全パッケージ状態が開始前のSHA256 `59e45b2d14e4c0511b423105c87adae1d57e8cf650aaca213194564c27a73333`と一致し、対象プロセスがないことを確認してVMを通常停止した。`domstate`で停止済みも確認した。T16のインストール済みアプリ・通常profile・実DBは今回操作していない。

ログ：`build/v011-{configure,final-package,package-test,ctest,source-rebuild,reproducibility}.log`、`.cache/v011-vm-state.json`、`.cache/v011-vm-logs/`、`.cache/v011-diagnostic-*`、`.cache/v011-{source-audit,source-comparison,tag-source-comparison}.json`。最終QA記録のこの節は、凍結済み対応ソースへ書き戻さず、タグ後の文書commitとして保持する。

## 公開完了

**2026-11-15 15:51:33 JSTに [KeePassXC GNOME v0.1.1](https://github.com/NBE03xxx/keepassxc-gnome/releases/tag/v0.1.1) を公開した。**

- リリースcommit：`64b7a03f684629f83f54bf10fbcab735b481421c`
- 注釈タグ `v0.1.1` のobject：`69e5131538afd5281e6802908ed4394be605edc9`
- Release ID：`445867559`。draft=false、prerelease=false、latest=true。
- 全4アセットを下書きで添付し、本文・名前・サイズ・GitHub側digestを照合してから公開した。
- 公開後に全4アセットを認証なしで再ダウンロードし、固定物とSHA256が一致した。認証なしのRelease参照と最新リリースも照合した。
- タグが上記commitを指し、Private vulnerability reportingが有効のままであることを確認した。

記録：`.cache/v011-release-{draft,published}-verification.json`、`.cache/v011-release-download/`。GitHubのmainと新しいv0.1.1タグのみをpushし、既存v0.1.0タグ・アセットやGiteaのoriginは変更していない。既知のメモリ・未対応・未検証はREADME等に維持している。
