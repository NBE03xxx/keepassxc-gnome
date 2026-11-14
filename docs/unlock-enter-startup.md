# Enterでの解除と初期画面の状態表示

2026-11-14。利用者の報告した「クリックなら解除できるがパスワード欄のEnterで解除できない」「DB未選択の起動直後にロック済みと表示する」を修正した。暗号・KDBX処理・上流参照checkout・新規依存の変更は無い。

## 修正

解除ダイアログの既定応答を既存のacceptへ設定し、解除用GtkPasswordEntryのactivates-defaultをtrueにした。GTKの標準処理により、パスワード欄のEnterが既存の解除ボタンと同じresponseへ届く。新しい認証経路や認証の省略は加えない。解除世代・デスクトップ監視・暗号化復旧コピーの保護と、応答後の入力消去は既存の処理を使う（`src/main.cpp` の `credentials()`）。

原因の根拠は、従来のダイアログに既定応答がなく、[GtkPasswordEntry.activates-default](https://docs.gtk.org/gtk4/property.PasswordEntry.activates-default.html)の既定値がfalseであること。固定開発環境のGtk-4.0.girでも確認した。

起動直後の中央表示とヘッダーを「データベースが開かれていません」/ “No database is open” とし、開くアイコンを表示する。正常に作成・解除した事実だけをWindow内のboolに保持し、その後に実際にロックした場合は従来の「データベースはロックされています」へ切り替える。ウィンドウを作り直した場合は未選択から始め、過去のDBを自動で開かない（`src/main.cpp` の `updateTitle()`、`credentials()`）。

## 実キーによる検証

`tools/test-ui.sh --unlock-keys` は専用のD-Bus・GNOME / Waylandで、既存のWindowを使う寿命probeが用意した上流暗号化済みの合成復旧DBを開く。製品コードに試験用の認証フックは設けない。probeは通常の解除ダイアログで待ち、`tests/ui_unlock_keys.py` が対象PIDのパスワード欄へ合成パスワードを設定し、実際のキー入力を送る。

- 日本語のReturnで解除し、保護された復旧・ロック・秘密消去を含む寿命シナリオの最後まで成功。
- 英語のテンキーEnterでも同じシナリオを成功。
- 通常CTest22/22成功。既存のクリック解除は別の保存・寿命GUIで維持し、実キーと両方を確認する。

GTK wrapperのactivate通知を手動でemitするだけの検査は実キーを再現せず、保存probeで失敗した。これは製品のキーボード不具合の再現とは扱わず、既存のクリックprobeと、実際のキーを送る独立した回帰検査に分けた。失敗ログを保持する。共通の `tools/check-release.sh` に日英の実キー解除を追加した。私有GNOMEのテスト用APIを通常ログインで有効にしない。

## 追加の検証と候補

- 日英polish各61チェックで、未選択の初期表示と、開いた後に実際にロックした表示を確認。
- クリック解除の保存・復旧GUI、日本語製品GUI、GUI寿命、ASanのGUIアクセス検査も成功。GUI全体の生LSanや長時間メモリの合格判定にはしない。
- 通常ログインの `/run/user/1000/doc fuse.portal` は私有GUI試験の前後で維持。

`build/dist/unlock-startup-candidate/` にdev8-1、`upgrade/` に更新試験用dev8-2を固定した。3実行ファイルは両版で同一hash。workerとpickerはdev7から不変で、修正はUI・翻訳・検査に局所化している。

| 成果物 | SHA-256 |
| --- | --- |
| `keepassxc-gnome-development_0.1.0~dev8-1_amd64.deb` | `d7fcfee0c2be76eb6c2699ea7961e7e63cec2dedd751cb10cb3b5ce0bf50cca7` |
| `keepassxc-gnome-development_0.1.0~dev8-1.source.tar.xz` | `c4f88c630be4ee4d8f878bfd652fc6c79de9b95b5758c896b8720e8265705dd0` |
| `keepassxc-gnome-development_0.1.0~dev8-1_amd64.manifest.json` | `fe0ca2e7faf27adf98e4bdbbeb07eff363836fda361ba06a7baefcc89c8a7c36` |

本体は `aeba5da1bf4222ac0bff44cdc1443f0156cf5113efbb098921d5c18766300ad7`、workerは `b6567b7d8f98c517fbb3dacf94c309c36abc96669c57365c3b89e21640f2e237`、pickerは `cc9b74490cbc44e521a61dd74edf2f920ac5c6f6dfffb80548088052b58b83fd`。完全ソースは基点a8e44b2後の未commit差分を含み、hashで特定する。候補固定後の検証結果・文書追記はアーカイブへ後から書き込まない。

梱包・配置先worker / picker・locale・ハードニング・出典・改変拒否・Git情報なしの対応ソースconfigureが成功。完全ソースからのフル再コンパイルと同一入力の梱包再現は本候補で再実施していない。

Ubuntu 26.04.1 VMに `keepass-gnome-before-dev8-20261114` を追加して起動し、アプリ1件だけの導入・更新・削除を確認した。dev8-1を再導入して3実行ファイルをmanifestと照合し、配置先製品GUIの初期未選択・作成・グループ編集・検索・実際のロック後の表示、日英guard、配置先workerの寿命107チェックが成功した。VMの製品GUIはクリックによる操作で、実Return / テンキーEnterはホスト内の私有セッションの同じWindowコードで別に確認した。

最後にアプリだけを削除し、全パッケージ名・版・状態が開始前と一致した。新規依存・他パッケージ更新・autoremove・snapshot復元は無い。guest agent経由で通常shutdownした。

主なログは `.cache/unlock-startup-{build,ctest,return-ja,keypad-en,save-verified,polish-ja,polish-en,smoke,lifetime,asan-lifetime-final,package-test}.log`、`.cache/vm-dev8-{transfer,plan,package-result,gui-result,cleanup}.log`、`.cache/vm-dev8-state.json`、`.cache/vm-dev8-logs/`。VM内の合成記録は `/tmp/keepass-gnome-dev8.ydOHWz/`。## 実機反映

利用者が保存・終了して更新できると回答した後、開発版のプロセスが無いことと、APT計画が開発アプリ1件だけの更新であることを確認した。OS認証経由で実機をdev8-1へ更新し、3実行ファイルのhashを候補と照合した。他の全APTパッケージ名・版・状態と本家Flatpak commitは不変。新規依存は無い。

配置された日本語カタログの「データベースが開かれていません」「グループを編集」を照合した。更新後に利用者自身が開発版を起動済みだったため、そのプロセスを操作せず、DBと画面内容は読み取らなかった。実機のEnter解除・起動表示は利用者自身の確認を待つ。記録は `.cache/dev8-host/{install.log,result.json,launch.json,packages.before,packages.after}` と `.cache/unlock-startup-host-plan.log`。

VMは通常シャットダウン後、shut offを確認した。既存のカスタム画像表示、長時間メモリ、生LSan、clipboard等の既知の制限は変更しない。追加commit / push・GitHub公開は行っていない。実際のDBは使わず、GitHub公開は利用者の確認後の改めての明示指示を待つ。
