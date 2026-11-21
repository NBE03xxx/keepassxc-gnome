# 配布物と継続保守

## 公開版v0.3.0の配布

Ubuntu 26.04（amd64）用の公開パッケージ名は `keepassxc-gnome`。本体とworker、picker、Chrome連携用のNative Messagingホストを `/usr/libexec/keepassxc-gnome/` に配置する。Chromeへの登録は利用者がアプリで連携を有効にしたときに作られる。[利用ガイド](user-guide.md#ブラウザー連携)。

同じビルドのdeb、完全対応ソース、ビルドmanifest、SHA256SUMSをGitHub Releaseで配布する。v0.3.0の公開用debは未署名。公開前にUbuntu 26.04.1 VMでの新規導入・更新・削除と、T16での導入・操作を確認した。[v0.3.0の検証記録](phase3-results.md#v030のバージョン表示とt16導入2026-11-21)。

## 手順6の開発記録

2026-11-13時点の開発版記録。対象はUbuntu 26.04 / GNOME 50 / Waylandで、合成データを使用した。

**2026-11-14の現在地。** この文書の試験件数・dev1の結果は手順6時点の記録。後のdev4の画面確認に続き、[dev5の梱包とVM検証](session-guard-candidate-validation.md)、[実機更新・5分経過後のOrca確認](dev5-physical-orca.md)を今回の操作範囲で完了した。初回のqzv読み上げの入力条件は不明で、未解決の観測として残す。GUIの生LSanと長時間の増加基準も[公開前レビュー](pre-release-review.md)で区別している。

## 配布構成

パッケージ名は `keepassxc-gnome-development`。本家 `keepassxc` を置き換える Provides / Conflicts / Replaces、MIME の既定変更、サービス登録、ユーザー設定を変更する maintainer script は持たせない。

| 配置 | 内容 |
| --- | --- |
| `/usr/bin/keepassxc-gnome` | 下記の本体への相対シンボリックリンク |
| `/usr/libexec/keepassxc-gnome/` | 本体、`keepassxc-gnome-worker`、`keepassxc-gnome-picker` |
| `/usr/share/locale/ja/LC_MESSAGES/keepassxc-gnome.mo` | 日本語翻訳。英語はソースの表示文字列 |
| `/usr/share/applications/org.keepassxc.Gnome.Development.desktop` | 本家と異なるアプリ ID・開発版の表示名 |
| `/usr/share/icons/hicolor/scalable/apps/` | 独立したオリジナルアイコン |
| `/usr/share/doc/keepassxc-gnome-development/` | ライセンス、本家の著作権表示、出典・パッチ・依存・バイナリーのハッシュ、検証手順 |

`packaging/Install.cmake` は GTK アプリのファイルのみをステージする。上流の install ターゲットは呼ばない。`src/gnome/RuntimePaths.h` は開発ビルドとインストール済みの配置を区別して翻訳を探す。ワーカーは本体と同じ私有ディレクトリから起動する。共有ライブラリは同梱せず、Ubuntu パッケージへ依存する。

## 作成

新しい依存の導入は AGENTS.md に従って事前に相談する。通常環境で `apt install` してビルドすることは必須ではなく、既存のプロジェクト内 prefix を使える。

```bash
source tools/dev-env.sh
cmake -S . -B build -G Ninja
CMAKE_BUILD_PARALLEL_LEVEL=4 cmake --build build
bash tools/test-lifetime.sh --all
python3 tools/build-deb.py
```

成果物は `build/dist/` の deb、完全な対応ソース `*.source.tar.xz`、manifest、`SHA256SUMS`。通常環境へのインストールは行わない。インストール・更新・削除の最初の検証先は Ubuntu 26.04 VM とする（[VM 手順](vm-package-test.md)）。

ビルドがこのプロジェクトと固定した上流のものか CMake cache で照合し、梱包前に再ビルドする。異なる候補 checkout のバイナリーや変更前の実行ファイルを、現在のソースとして梱包しない（`tools/build-deb.py:28`）。

依存は `dpkg-shlibdeps` で実際の3実行ファイルから求める。ホストへ導入していない展開済みライブラリは、SHA-256 を照合した元の Ubuntu deb の `shlibs` を使う。不明な依存を無視するオプションは使わない。配色の GNOME schema と FileChooser の portal / GNOME backend も実行時依存に含める。

manifest は固定した上流・完全なツリーのハッシュ・独自パッチ・対応ソース・strip 後の実行ファイル・開発用 deb・ツールの版を記録する。設定・キャッシュ・実 DB・`.git` は配布物に含めない。出力先の既存成果物を同じ版で上書きできるため、受入済み成果物は別の保管先へ移し、再発行時は版を上げる。

## ソースと再現性

`packaging/upstream.json` に本家のリポジトリ、タグ、commit、全ファイルを順序・実行属性・SHA-256 で集約したハッシュを固定する。Git checkout では dirty / untracked も拒否する。対応ソースには `.source-commit`・本家が認識する `.gitrev` / `.version` を添え、Git の履歴がなくても完全なツリーを検証してビルドできる。これら3マーカー以外の追加・変更ファイルもハッシュを変える。既存の reader パッチの個別ファイル検証は継続する（`tools/lib/provenance.py`、`tools/prepare-core-overrides.py`）。

`SOURCE_DATE_EPOCH`、tar のファイル順・owner・時刻・mode、gzip の時刻、deb の owner を固定する。同じソースと同じビルド出力からの梱包を再現する。別の Ubuntu 更新状態・コンパイラー・絶対パスから生成する ELF がビット単位で一致することまでは保証しない。ソースからの再ビルド可能性と、梱包のバイト一致は分けて検証する。

対応ソースに外部依存の deb は含めない。開発 prefix の manifest はその環境で追加展開した集合であり、クリーンな Ubuntu の全 build dependency を置き換えるものではない。クリーン VM では [ビルド依存](../tools/bootstrap-dev.py) の要求に加え、コンパイラー・Git・Ninja・pkg-config・gettext・Python・dpkg-dev と標準開発環境が必要。導入前に APT のシミュレーションを確認する。

## ライセンスと公開前の条件

独立コードの既存方針は GPL version 2 または version 3。元のソースヘッダーと本家 `COPYING` / `LICENSE.*` を保持し、リンクされる第三者の表記も配布する。QtIOCompressor の LGPL-2.1 / GPL-3 の選択肢と例外、埋め込まれる資源の表記も対象とする。動的リンクの Ubuntu ライブラリは同梱しない。

deb と**同じ内容の対応ソースを同時に配布**する。本家へのリンクだけでは、この GTK フロントエンドとパッチの対応ソースにならない。パッケージ中の `build-manifest.json` と外部 `SHA256SUMS` で照合する。公開する場合は実際の保守連絡先、独立した公開 URL、著作権表示の確認、署名鍵の管理者を確定してから、版付きタグ・チェックサムへの署名を行う。現在の `noreply@localhost` は内部検証用であり公開窓口ではない。アップロードや署名鍵の生成は自動で行わない。

## 継続保守

1. 本家の [Releases](https://github.com/keepassxreboot/keepassxc/releases)、[Security Advisories / 報告窓口](https://github.com/keepassxreboot/keepassxc/security)、Ubuntu の依存更新を、担当者が通常週1回以上・緊急通知時は随時確認する。これは運用上の目安であり自動監視を設定したという意味ではない。
2. セキュリティ修正は UI の追加より優先し、影響範囲・悪用条件・固定版・このアプリに到達する経路を記録する。未評価は「影響なし」にしない。修正前のビルドを新規配布しない判断も含める。
3. 本家の参照 checkout を編集せず、候補を独立した checkout / 検証ブランチで確認する。タグ署名・公開された出典を確認し、対象 commit を記録する。Git の hash 照合だけを発行者の署名確認と扱わない。
4. `packaging/upstream.json`、取得タグ、パッチの個別 hash、対象 API と `cmake/` 接続を一組で更新する。パッチの fuzz 適用は禁止。不要になった独自修正は削除し、上流と同じ挙動に戻す理由を記録する。暗号・KDBX を独自実装で迂回しない。
5. 上流テスト、既知データの raw / IPC 往復、資源上限・終端・保持拒否、保存中断・復旧・自動ロック・picker を回す。GUI と ASan を確認し、候補の無変更の CLI と相互運用する。任意未知構造の再保存は対象に戻さない。
6. 依存更新では公開 soname・ABI と Ubuntu のセキュリティ情報を確認し、再リンク・パッケージ依存の再算出・VM の更新試験を行う。固定した古いキャッシュだけで「安全」と判断しない。
7. 検証結果と残件を記録し、版を上げて成果物を作り直す。VM の導入・更新・削除と手順7を通過するまで、製品として検証完了とは扱わない。

コミットは、上流 baseline 更新、独自パッチの調整、GTK / adapter、試験と文書をレビュー可能な単位に分ける。既存作業を勝手に破棄しない。タグや lock の更新だけで自動採用せず、取り込みの最終判断を人が行う。remote への push と公開は明示的な指示時のみ。

本家の内部 API は安定した SDK ではない。モデル所有権、シグナル、reader / writer、鍵、stream、CMake ターゲットが変われば adapter と試験の影響を評価する。対応不能時は最後に検証した安全な版を維持し、理由を示して新しい形式や設定を拒否する。

## 残る受入条件

VM への実インストール、実機での最終確認、GUI 全体の LSan 判定は別の合格条件。AppStream の公開 URL・実際の保守窓口・公開署名・APT repository のホスティングは、現在の非公開検証用 deb の外側に残す。Flatpak / Snap は対象外。

## 本家の確認記録（2026-11-13）

`python3 tools/check-upstream.py --output .cache/upstream-review.json` で、公式 GitHub API の最新リリースと公開 advisory を読み取り確認できる。失敗時は unknown として非0で終了する。コードや lock を自動更新せず、担当者が評価する。定期実行の設定や外部通知は行っていない。

[今回の記録](security/upstream-review-2026-11-13.json)では、最新リリースの表示は固定版と同じ2.8.0。公開 advisory は6件で、本家記載の修正版は2.8.0または2.7.12。この版比較だけで脆弱性全般がないとは判断しない。依存・GTK adapter・未公開の問題・署名の発行者確認は別の評価が必要。参照先は [公式のリリース](https://github.com/keepassxreboot/keepassxc/releases/tag/2.8.0)と[公式の公開 advisory](https://github.com/keepassxreboot/keepassxc/security/advisories)。

## ローカル / CI の共通入口

`bash tools/check-release.sh` で source 検証、ビルド、CTest、GUI、ASan、梱包、展開試験、対応ソースの再ビルド、同一入力の梱包一致を順に実行する。個別の対応ソースの再ビルドは `bash tools/test-source-build.sh`、梱包一致は `bash tools/test-reproducibility.sh`。いずれもパッケージのインストール・公開・署名・push を行わない。GUI と私有 D-Bus が利用できる、合成データ専用の Ubuntu 26.04 runner を使う。個人のパスワード環境を持つ runner で、外部 PR の未審査コードを自動実行しない。

未公開リポジトリのため、外部サービス上の CI job は登録していない。将来の GitHub / Gitea 接続ではこの入口を再利用し、使用する runner・依存・ログ保存期間を明示する。成功ログに加え、VM の受入記録と未完了事項をリリースごとに保持する。

## 今回の確認結果

**手順6の非公開開発版の作成・自動検証と VM のパッケージ試験の範囲で完了。**

| 検査 | 結果と範囲 |
| --- | --- |
| 通常の回帰 | CTest 19/19成功。既知データ・資源上限・保存／復旧・秘密の寿命・picker を含む |
| 対応ソース | Git 情報を除いたソース一式から本体・補助プログラム・参照 CLI・全試験を独立して再ビルド。再ビルド側も CTest 19/19成功 |
| 改変検出 | 本家のファイル変更・追加、異なる source / upstream の CMake build を拒否 |
| 配置の検査 | 本体と2補助プログラム、翻訳、相対リンク、desktop、icon、ライセンス、source digest を確認。RPATH・ASan・set-ID はなく、PIE / RELRO / NOW / 非実行 stack を確認 |
| 配置後の動作 | 展開した私有 worker で合成 DB 操作。picker 189チェック。実行ファイルの日本語・英語 fallback。展開済み deb の本体から独立 GNOME の日本語 GUI 操作も成功 |
| 梱包の一致 | 同じソース・既存ビルド出力で2回作成した source、deb、manifest、checksums が一致。異なるビルド環境での ELF 一致の試験とは区別する |
| VM | 利用者の承認後、Ubuntu 26.04.1 LTS / x86_64 に導入。開発 prefix を渡さず、配置済み worker で作成・編集・検索・履歴・保存・再解除・ロックを確認。0.1.0~dev1-1 → -2 の更新、purge、合成 config / state sentinel の保持が成功 |

ホストに開発・実行時パッケージを追加していない。VM に承認を受けて追加した依存は `libbotan-3-10`、`libqt6concurrent6`、`libtspi1`。既存パッケージの更新・削除はなく、依存の autoremove も行っていない。開発版は試験後に削除する。VM の本家 KeePassXC は元から未導入だったため、「未導入の状態を変更しなかった」という確認であり、本家と実インストールした併存試験ではない。

検証基盤では2点を修正した。対応ソースの `.gitrev` は本家が C++ の版文字列へそのまま埋め込むため、末尾改行を付けずに生成する。配置試験の domain runner は引数の worker を無視していたため明示的に使うよう修正し、存在しない指定先を渡すと失敗する対照も加えた。これにより開発ツリーの worker で誤って成功することを避けた。本家の参照ソース・既存パッチには変更なし（`tools/make-source.py:16`、`tests/domain_tests.cpp:40`、`tests/package_tests.py`）。

この Ubuntu では `dpkg-shlibdeps` が usr-merge に伴う dynamic loader の diversion を警告する。依存欠落を無視せず、生成した依存と実際の VM の動的ロード・モデル操作で確認した。警告なしと報告しない。`desktop-file-validate` は成功し、Security カテゴリーの分類に関する hint が出る。

主なログは `.cache/distribution-build.log`、`.cache/distribution-ctest.log`、`.cache/package-test.log`、`.cache/package-ui.log`、`.cache/source-rebuild.log`、`.cache/package-reproducibility.log`、`.cache/vm-package-result.log`。VM のハッシュ・終了状態は成果物側の受入記録にも保存する。公開・push・署名は行っていない。

**残件。** 通常ログインの GNOME の実 FileChooser、実機でのロック・スリープ・clipboard、拡大表示・支援ツール、GUI 全体の LSan、公開前の保守窓口・署名確認。次は手順7。既存の KeePassXC や本番 DB を変更せず、利用者が実操作できる時間に合成データで最終確認する。
