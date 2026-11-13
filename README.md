# KeePassXC GNOME Native Edition

公開時の対象は **Ubuntu 26.04での日常的な短時間の個人利用** とする。GNOME 50 / Waylandで検証し、残るメモリ保持の性質の調査と最新版のVM→実機の画面確認を実施した。[対象範囲と受入条件](docs/release-scope.md)を参照。

[保持量の追加調査](docs/gui-retention-characterization.md)では描画キャッシュの回収と、検証版libadwaitaのbreakpoint解放漏れを確認した。この依存問題は既知の制限として残す。[最新版候補の確認](docs/short-session-validation.md)はVMと実機の画面・入力・ロックを完了した。整理後の実機Orca音声は未検証。

Ubuntu 26.04 / GNOME 50 / Wayland 向けの GTK4 + libadwaita アプリの開発版。暗号、KDBX、検索、パスワード生成には KeePassXC の既存コアを使用する。

現在は合成データによる開発・検証用。終端検証と[KDF・読み込み量の暫定上限](docs/read-resource-policy.md)に加え、[XML・モデル件数の上限と保持可否の検査](docs/xml-model-preservation.md)を追加した。保持できない未知データは解除を拒否する。読み込みには専用子プロセスの[資源制限と中断](docs/reader-process-limits.md)を追加した。[安全な保存と復旧](docs/safe-save-recovery.md)は自動検証した範囲で実装した。[自動ロック・編集の復旧・秘密の寿命](docs/auto-lock-secrets.md)も自動検証した範囲で実装した。GNOME や他アプリが保持するコピーの消去は保証しない。[GNOME UI の仕上げ](docs/gnome-ui-polish.md)ではネイティブファイル選択の補助プロセス、一覧の仮想化、狭幅表示、明暗・言語の追従を整えた。VM・実機での最終確認には製品化の合格条件が残る。実際のパスワード DB を使う段階に達していない。実装と検証の状況は [Phase 3 の記録](docs/phase3-results.md) に記載する。任意の未知構造の再保存は [検討記録のみ](docs/compatibility-policy.md) とし、実装対象・完成条件には含めない。

## ビルド

公式タグ 2.8.0 のコミット `1c2f23ef5914c0f442a00533def1689857afb5a7` を使う。ソース取得後は CMake がコミットを照合する。
アプリには [終端検証のパッチ](patches/README.md) をビルド用コピーへ適用する。相互運用試験には、別ビルドの無変更の上流 CLI を使う。
本家コアだけにリンクする参照ツールによる[既知データの保持試験](docs/known-data-compatibility.md)も実行する。

```bash
bash tools/fetch-upstream.sh
python3 tools/bootstrap-dev.py
source tools/dev-env.sh
cmake -S . -B build -G Ninja
CMAKE_BUILD_PARALLEL_LEVEL=4 cmake --build build
ctest --test-dir build --output-on-failure
bash tools/run-development.sh
```

`bootstrap-dev.py` は APT が解決した開発依存の deb をダウンロードし、SHA-256 を照合して `.cache/development/sysroot` へ展開する。取得したパッケージとハッシュは `.cache/development/packages.json` に記録する。この手順は Ubuntu 26.04 の既存のコンパイラー、Ninja、Git、pkg-config、gettext を使う。

日本語と英語は環境のロケールから選択する。日本語のカタログが選ばれない環境では英語を表示する。明暗は libadwaita のシステム設定追従を使い、通知できない場合には GNOME の配色設定を読み取って追従する。

## GUI の検証

```bash
bash tools/test-ui.sh
bash tools/test-ui.sh --cancel
bash tools/test-ui.sh --save
bash tools/test-ui.sh --lifetime
bash tools/test-ui.sh --polish
KPXC_UI_LANGUAGE=en bash tools/test-ui.sh --polish
bash tools/test-lifetime.sh --all
bash tools/test-ui.sh --orca
KPXC_UI_LANGUAGE=en bash tools/test-ui.sh --orca
```

GNOME Shell と AT-SPI のある環境で、独立した D-Bus、設定、Wayland セッションを作って日本語 UI を操作する。結果は `build/ui-test/`、`build/ui-cancel-test/`、`build/ui-save-test/`、`build/ui-lifetime-test/`、`build/ui-polish-ja/`、`build/ui-polish-en/` に記録する。保存・復旧の専用 GUI 試験は同じ画面コードを使い、ネイティブファイル選択の経路は通常ログインでの最終確認に残す。実インストールと deb の確認には、先に Ubuntu 26.04 の VM を使う。


[実機確認](docs/physical-final-check.md)ではロック・スリープ・配色・保存を確認した。[GUI メモリと Orca](docs/gui-memory-accessibility.md)には分離環境の実 Orca とキーボード試験、残る LSan 報告を記録した。回帰中に見つかった[保存のストリーム境界](docs/stream-finalization.md)も独立パッチで修正している。日本語の実機での音声聴取は確認済み。[GUIメモリの追加照合](docs/font-offset-lsan.md)では固定版の相対参照・末尾参照により10巡の報告を全件説明した。補助診断は合格、生のLSanは非ゼロであり0件合格とは区別する。[長時間の反復検証](docs/gui-soak-results.md)後に[5分間隔のキャッシュ整理](docs/font-cache-control.md)を実装し、75分試験で保持量増加を約39%抑えたが、増加の事前基準が1項目残り、長時間安定性は未合格としている。

## 開発版 deb と保守

[配布・保守手順](docs/distribution-maintenance.md)と[VM 試験手順](docs/vm-package-test.md)を用意した。開発版は本家と別の名前・アプリ ID を使い、本体と2つの補助プログラム、翻訳、ライセンスを含む。対応ソースとチェックサムを同時に作成する。

```bash
source tools/dev-env.sh
python3 tools/build-deb.py
bash tools/test-package.sh
bash tools/test-source-build.sh
bash tools/test-reproducibility.sh
```

成果物は `build/dist/`。通常環境にインストールせず、先に Ubuntu 26.04 VM で導入・更新・削除と合成データの操作を確認した。VM での結果と未確認事項は [手順6の記録](docs/distribution-maintenance.md#今回の確認結果)に記載する。実ログインの GUI と実機の最終確認、GUI 全体の LSan 判定は残る。`bash tools/check-release.sh` はローカル / CI 用の共通入口であり、公開やインストールを行わない。

## 構成

- `src/main.cpp` に GTK4 / libadwaita の開発用 UI と Qt ワーカーを置く。
- `prototype/CoreSession.*` は既存試作から継承した KeePassXC コアの境界。
- `cmake/` で上流のターゲットに独立したフロントエンドを接続する。
- `upstream/keepassxc/` は固定した上流の参照用チェックアウト。
- `docs/` に調査、実装、検証と未完了事項を記録する。

## ライセンス

本プロジェクトのソースコードは GNU GPL version 2 または version 3 の条件で提供する。
本文は [GPL-2](LICENSE.GPL-2) と [GPL-3](LICENSE.GPL-3) を参照。
再利用する KeePassXC と依存ライブラリの著作権表示・ライセンスは各ソースに従う。
