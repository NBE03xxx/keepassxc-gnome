# v0.2.0の導入・公開記録（2026-11-19）

利用者の指示に基づき、既存KDBXのキーファイル単独・パスワード併用での解除を含む候補を作成した。解除の鍵読み取りには、本家KeePassXCの `FileKey` と `CompositeKey` を利用する。根拠と検証方法は[キーファイル解除の記録](keyfile-unlock.md)を参照。

## 固定成果物

`build/dist/v0.2.0/` に保存した。Debianパッケージ版は0.2.0-1。対応ソースのプロジェクト内223ファイルが、リリース用コミット `b36da6ca11339a6ad2b57fcdda7f24f301695ff6` と一致することを確認した。上流参照の固定コミットは `1c2f23ef5914c0f442a00533def1689857afb5a7`。

| ファイル | SHA256 |
| --- | --- |
| `keepassxc-gnome_0.2.0-1_amd64.deb` | `97b5074fe9c7a6086af90773673e9de14c4ea14ade19ed0df3ecb5c0d610ce49` |
| `keepassxc-gnome_0.2.0-1.source.tar.xz` | `be68c3d37c18149b451bc62f63942dd96e9a8ae713303d424ea7e9347e249e17` |
| `keepassxc-gnome_0.2.0-1_amd64.manifest.json` | `9249a3c6dbcd6e79c81eb1c89633f23eb1282a7d137b1100f6283084a08ae1bc` |
| `SHA256SUMS` | `fa2d37bcb93beed1f2faa0789e74805dccf629ddbfdbdecac29c9c4209525dda` |

## VMとT16

通常CTestと公開予定の完全対応ソースからのクリーン再ビルドは、どちらも24件全件合格した。梱包検査では実行ファイル、翻訳、対応ソース、配置先workerとpickerを照合した。

Ubuntu 26.04.1 VMで、T16と同じ旧版0.1.0-2から0.2.0-1への更新、導入済みの本体の基本操作、導入済みworkerでの鍵形式・DB版・因子の45通りと本家コアとの保存・復旧往復、日英の実Return/KP_Enterによる解除を確認した。検証パッケージを削除し、全パッケージの状態が導入前と一致したことを確認してVMを停止した。

T16では、直接実行したpkexecが作業環境の権限制限で動かなかったため、UbuntuのPackageKitを使った。APTとPackageKitのシミュレーションで、本アプリ1件のみの更新であることを確認した。更新後は0.2.0-1、3実行ファイルのSHA256、dpkg整合、日本語のキーファイル文言を照合した。他のAPTパッケージ、本家KeePassXCと関連付け設定に差分はなかった。T16内の私有セッションで導入済み本体の基本操作と、導入済みworkerで45通りの鍵の読み書きも確認した。

本家コアで作成した確認専用の合成DBを、T16の通常Waylandデスクトップと確認専用プロファイルで開いた。利用者がキーファイル単独・パスワード併用の両方で解除できた。

初回の確認用起動では、作業環境の `LC_ALL=C.UTF-8` を引き継いだため英語で表示された。`src/gnome/Locale.h` の言語選択が参照するロケールを確認し、T16のGNOMEセッションと同じ `LANG=ja_JP.UTF-8` で再起動した。導入済み本体の `--language-check` が「準備完了」を返し、T16の私有セッションで日本語の画面文言を使った基本操作が通ることを確認した。debはこの時点でT16に導入済みのものを使った。

ログは `build/v020-*.log` と `.cache/v020-{vm-logs,host}/` に保持した。

## 公開結果

2026-11-19 20:46:19 JSTに [KeePassXC GNOME v0.2.0](https://github.com/NBE03xxx/keepassxc-gnome/releases/tag/v0.2.0) を公開した。Release IDは451269893。注釈タグ `v0.2.0` はコミット `b36da6ca11339a6ad2b57fcdda7f24f301695ff6` を指す。

T16とVMで検証したdeb、完全対応ソース、manifest、SHA256SUMSを公開した。4件すべてを認証なしで再ダウンロードし、SHA256が上記と一致することを確認した。最新の正式リリースと非公開脆弱性報告が有効であることも確認した。照合結果は `.cache/v020-release-published-verification.json` に保持した。
