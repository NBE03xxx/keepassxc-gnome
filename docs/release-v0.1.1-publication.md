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
