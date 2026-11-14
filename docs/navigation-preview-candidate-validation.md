# 新UIのdev6候補・VM検証・実機反映

2026-11-14。[UI変更](navigation-preview-ui.md)の配布候補。**VM検証・実機の更新と日本語ロック画面での起動を確認した。新UIの一通りの実機操作は利用者自身の確認を待つ。GitHub公開は行わない。**

## 固定した成果物

保存先は `build/dist/navigation-preview-candidate/`。既存のdev5・RC1を上書きしていない。

| 成果物 | SHA-256 |
| --- | --- |
| `keepassxc-gnome-development_0.1.0~dev6-1_amd64.deb` | `6f3e2d9a271b6eb1346935521dfa6d76a800e8d77d180a1f80de00bb1770398a` |
| `keepassxc-gnome-development_0.1.0~dev6-1.source.tar.xz` | `e7ac8503a836f41168b087114a252865143b7ce8d1745f7524906afeed59e964` |
| `keepassxc-gnome-development_0.1.0~dev6-1_amd64.manifest.json` | `57f716dbf924a5cfa55771c2b84780a5727a609ce6535c9a2c9a99325cde56f9` |

更新試験用dev6-2は `upgrade/` に保持し、3実行ファイルはdev6-1と全件同一hash。

| 実行ファイル | strip後のSHA-256 |
| --- | --- |
| `keepassxc-gnome` | `a108af02e4c688d09df6404458f3cc462435e5e962ddf41abf0a9dfb5487fa01` |
| `keepassxc-gnome-worker` | `c9810b7f9b9076825f3b488e17b925669992d47609cba4f02d7db75a5799744f` |
| `keepassxc-gnome-picker` | `cc9b74490cbc44e521a61dd74edf2f920ac5c6f6dfffb80548088052b58b83fd` |

基点commitは `a8e44b219156a54836661f6e3d620b26ebdb6af4`、候補にはその後の未commit差分を含む。対応ソース全体のhashで版を特定する。固定した上流は2.8.0 / `1c2f23ef5914c0f442a00533def1689857afb5a7`。上流参照checkoutは変更していない。本書等のVM・実機結果の追記は候補固定後の文書差分として保持し、アーカイブへ後から書き込まない。

## 梱包検査

`tools/build-deb.py --version 0.1.0~dev6-1 --output build/dist/navigation-preview-candidate` と `tools/test-package.sh` を使用した。配置先workerで標準・カスタムアイコンを含む合成DB操作、picker189チェック、locale、ハードニング、出典・上流改変拒否、Git情報なしの対応ソースのconfigureが成功。通常22/22、日英Orca各31・polish各48、保存・寿命・選択したASanの結果は [UI記録](navigation-preview-ui.md)。本候補では完全ソースからのフル再コンパイル・同一入力の梱包再現を再実施しておらず、改修前RC1の成功をその代用としない。

最初の制限内実行はpickerの私有D-Bus socketを作れず待機した。該当する旧試験プロセスだけを終了し、socketを利用できる環境で同じ検査を終了0まで再実行した。ホストへのパッケージ導入を行う検査ではない。ログは `.cache/ui-navigation-package{,-test}.log`、`.cache/ui-navigation-upgrade-package.log`。

## Ubuntu 26.04 VM

停止中の `ubuntu26.04` に `keepass-gnome-before-dev6-20261114` を作成して起動した。OSはUbuntu 26.04.1 LTS / x86_64。転送先 `/tmp/keepass-gnome-dev6.2TNrnB/` でhash一致を確認した。

1. APT計画は開発アプリ1件だけ。新規依存・他パッケージの更新・削除は無い。
2. dev6-1導入、配置先workerで標準・カスタムアイコンを含む合成DB操作、dev6-2へ更新、purge、合成設定・状態sentinelの保持が成功。
3. dev6-1を再導入し、実行ファイル3件をmanifestと照合。一般ユーザーの私有GNOME / Waylandで、配置先 `/usr/bin/keepassxc-gnome` の日本語UI・アイコン選択・グループ開閉・選択プレビュー・編集・検索・ロックが成功。日英の監視バス不在guardも成功した。
4. 配置先workerと無変更の本家CLIを使う寿命検査が103チェック成功。
5. 検証アプリだけをpurgeし、全パッケージ名・版・状態が開始前とバイト一致。hashは `59e45b2d14e4c0511b423105c87adae1d57e8cf650aaca213194564c27a73333`。配置先アプリの残存プロセスなし。
6. guest agent経由で通常shutdownし、`virsh domstate` でshut offを確認。snapshot復元・依存のautoremoveは行っていない。

私有GUIの成功を、通常ログイン・物理GPU・実スリープ・音声の新しい実機合格とは扱わない。VMログは `.cache/vm-dev6-{transfer,plan,package-result,gui-result,cleanup}.log`、`.cache/vm-dev6-state.json`、`.cache/vm-dev6-logs/`。

## 実機更新と起動

利用者が「今すぐ更新できる」と回答した後、シミュレーションで `1 upgraded, 0 newly installed, 0 to remove`、対象が開発アプリ1件だけと確認し、OS認証経由でdev5-1からdev6-1へ更新した。追加依存はない。導入後の3実行ファイルは上表と一致し、それ以外の全APTパッケージ名・版・状態は不変。本家Flatpak `org.keepassxc.KeePassXC` のcommitも更新前と同じ `eddd9c1e1d5b574bfb550ac50c69d55a9ec219e6492e4ae2a5a4e00009329f0f`。

`/usr/bin/keepassxc-gnome` を日本語環境で起動した。既存DB・復旧記録に触れないよう、state / cache / dataは新しい `build/host-ui-dev6/` に分離した。対象PIDだけのAT-SPI読み取りで「データベースはロックされています」の日本語表示を確認。DBは開いていない。通常ログインの `/run/user/1000/doc fuse.portal` も維持されている。起動したアプリは利用者の確認用に開いたままとした。

記録は `.cache/dev6-host/{install.log,result.json,launch.json,app.log,packages.before,packages.after}`、導入前計画は `.cache/ui-navigation-host-plan.log`。実機での新UI操作は [ご自身での確認](manual-acceptance.md) を待つ。既存カスタム画像の描画未実装、長時間メモリ基準未達、外部clipboard残留等は維持する。GitHub公開、追加commit / pushは行っていない。
