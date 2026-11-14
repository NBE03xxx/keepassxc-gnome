# v0.1.0の公開準備

2026-11-14。利用者のUI確認と公開準備指示に基づく。到達点は準備結果の報告であり、GitHubへのpush・リリース公開は別途指示を待つ。

## 変更

- READMEを概要・導入・機能・既知の制限・ビルド・ライセンスだけに整理した。開発履歴は既存の調査記録に保持する。
- `CMakeLists.txt`、`cmake/Frontend.cmake`、`src/main.cpp`で開発用UIを既定OFFとし、製品から開発版のタイトル・バナーを除く。開発用途でONを指定する機能は残す。
- `packaging/org.keepassxc.Gnome.Development.desktop`の表示名と説明を日英とも公開用にする。内部ID・アイコン名・データ保存先は互換性のため維持する。
- `tools/build-deb.py --release`で `keepassxc-gnome` / `0.1.0-1` を生成する。開発UIがONの場合やMaintainerを明示しない場合は拒否する。旧自作開発パッケージだけをConflicts/Replaces対象とし、本家を置き換えない。
- 利用者指定のMaintainerは `NBE03xxx <NBE03247@nifty.com>`。SECURITY.mdを追加し、公開時にGitHub Private vulnerability reportingを有効にする。公開前なので現時点でリンクの利用可能を保証しない。
- `docs/release-notes-v0.1.0.md`をGitHub Release本文の草案とする。配布物は未署名と明記する。既知のメモリ増加、未対応の解除・連携機能、カスタム画像の汎用表示は隠さない。

## 生成・検査の入口

```bash
source tools/dev-env.sh
cmake -S . -B build -G Ninja -DKPXC_GNOME_DEVELOPMENT_UI=OFF
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
python3 tools/build-deb.py --release --version 0.1.0-1 \
  --maintainer 'NBE03xxx <NBE03247@nifty.com>' --output build/dist/v0.1.0
bash tools/test-package.sh build/dist/v0.1.0/keepassxc-gnome_0.1.0-1_amd64.deb
bash tools/test-source-build.sh build/dist/v0.1.0/keepassxc-gnome_0.1.0-1.source.tar.xz
bash tools/test-reproducibility.sh 0.1.0-1 --release --maintainer 'NBE03xxx <NBE03247@nifty.com>'
```

共通の広範囲検査は `KPXC_RELEASE_PUBLIC=1 KPXC_RELEASE_MAINTAINER='NBE03xxx <NBE03247@nifty.com>' bash tools/check-release.sh`。いずれもホストへの導入・GitHubへの公開は行わない。VMの導入・旧開発版からの移行・更新・削除を先に検査する。対応ソースからの再ビルドと、同一入力での梱包バイト再現は別々の検査とする。

## 公開時の最終手順

1. 公開を指示された後、GitHubの対象リポジトリと公開範囲を確認する。
2. Private vulnerability reportingを有効化し、SECURITY.mdの報告導線を確認する。
3. 公開対象だけをcommitし、既存の資格情報・キャッシュ・実データを含まないことを確認する。タグは `v0.1.0`。
4. 固定したdeb、完全対応ソース、build manifest、SHA256SUMSを同時に添付し、用意した本文でReleaseを公開する。

署名用の鍵生成、GitHub変更、実機への公開版の移行は今回自動実行しない。GUI生LSan非ゼロ・長時間の増加基準未達、実機IME変換等の未検証は公開準備によって解決したとは扱わない。


## 凍結前の文書整理

READMEだけでなく同梱利用ガイドの冒頭・版案内も公開版の説明へ揃えた。先行の `build/dist/v0.1.0-preliminary/` は検査過程の成果物として維持し、配布対象は `build/dist/v0.1.0/` に固定する。実装・実行ファイルは先行候補と同一であることを照合する。


## 検証結果と固定成果物

公開用最終成果物は `build/dist/v0.1.0/`。版は0.1.0-1（アプリ表示0.1.0）、アーキテクチャはamd64。

| ファイル | SHA256 |
| --- | --- |
| `keepassxc-gnome_0.1.0-1_amd64.deb` | `b5192902c21f02002d69b6c690f1997341ec7d67cb45bc7c347e1443d7f220d6` |
| `keepassxc-gnome_0.1.0-1.source.tar.xz` | `83d33b74497f2f480a60448c253d49a1091f44e2610d87cfb8a96353ab4b4311` |
| `keepassxc-gnome_0.1.0-1_amd64.manifest.json` | `559c2b4f9b3b3a8be6a17c0e0136c35d891602c4df18331ebdf0ee351204a349` |

mainのSHA256は `8e65546fe7428391703a5548d33e2f2c2e527fa57c3514a5fe13f1b45b457050`。worker / pickerはdev11と同一であり、暗号・KDBX・既存保存形式は変更していない。成果物にはdevelopment_ui=falseを記録し、日英のdesktop表示名と実行版を梱包検査で照合した。

- 通常23/23が成功。通常サンドボックス内の最初の実行はD-Busソケット生成の拒否で寿命・pickerがタイムアウトしたため、同じ試験を専用バスが使える実行環境で再実行して全件成功した。製品コードを修正して隠していない。
- 公開表示の日英の歓迎画面、実Return / KP_Enterの確定、メモ改行、終了時保存、10色×明暗の20%選択混色が成功した。
- debのhardening、独立した名称・関連付け、ライセンス、翻訳、対応ソース、配置先worker/pickerが成功した。
- 先行候補の完全対応ソースを `build/source-rebuild.1EhpQl/` でクリーン再ビルドし、そのビルドでも23/23が成功した。最終対応ソースは先行版と全ファイルの集合・内容を比較し、差分が利用ガイドと本準備記録の2文書だけであり、実装・ビルド・テストの全入力が同一であることを照合した。最終文書差分によるコンパイルの再実施は不要と判断した。
- 最終成果物を同一入力から2回梱包し、deb・対応ソース・manifest・SHA256SUMSのバイト一致と、配布用に固定した最終成果物との一致を確認した。これは同一バイナリからの梱包再現であり、独立ELFビルドのバイト再現を主張しない。
- Ubuntu26.04.1 VMで導入→更新→削除、dev11から公開版への移行、設定・復旧sentinelの保持、配置先製品での基本操作、日英の既定キー・終了保存・歓迎画面・ファイル引数・既定関連付け保全・監視不能時の保護が成功した。寿命107・直近情報26チェックも成功した。VMの色検査は363チェック。移行時の削除は本プロジェクトの旧開発パッケージ1件だけであり、本家や他のパッケージは置き換えない。

ログ：`build/v010-*.log`、`.cache/v010-vm-logs/`、`.cache/vm-v010-state.json`。最終成果物を凍結した後のQA記録は対応ソースへ追記せず、別の作業結果として保持する。


## 最終判定

**v0.1.0のコード・README・配布物と公開手順の準備完了。** 最終debそのものもVMへ再導入し、SHA256、公開名称、版、同梱文書、3実行ファイル、dpkgファイル検証を照合して配置先アプリの基本操作を再確認した。初回の最終GUI試験はログ保存先がroot所有で開始できなかったため、既存の検証ユーザーが書けるログ保存先へ訂正して再実行し成功した。製品は変更していない。最終検証アプリをpurgeし、全パッケージ状態が開始前のSHA256 `59e45b2d14e4c0511b423105c87adae1d57e8cf650aaca213194564c27a73333`と一致し、アプリのプロセスが残らないことを確認してVMを通常停止した。

今回追加した依存パッケージはない。ホストのdocument portalマウントは維持している。通常環境のDBは開かず、本家のプログラム・設定へ変更していない。実機のインストール済みアプリはdev11のままとし、公開用パッケージへ移行済みとは扱わない。動作部分の既存実機確認は利用者の確認済み回答に基づき維持するが、新しい公開名称・パッケージ切替の実機操作は未実施。

Git commit・tag・push、GitHubへのリポジトリ変更・Release公開、Private vulnerability reportingの有効化は未実施。公開を指示された後に上記の手順を実行する。従来のGUI生LSan非ゼロ、長時間のメモリ基準、IME等の未検証範囲は、v0.1.0の範囲・既知の制限として残している。


## 凍結後のREADME追記（2026-11-14）

利用者の追加指示で `README.md` にキーファイル解除・ブラウザー連携・Auto-Type・TOTPの実装予定と、YubiKeyは現時点の予定に含めない旨を追記した。現在の作業ツリーのREADMEは、上記SHA256の凍結済み配布物内のREADMEより新しい。配布物は上書きしていない。公開前の最終梱包で文書を同期し、新しい対応ソース・deb・manifest・SHA256SUMSを一組で再固定する必要がある。今回の変更は文書だけで、実行ファイルや既存の動作検証結果は変更しない。
