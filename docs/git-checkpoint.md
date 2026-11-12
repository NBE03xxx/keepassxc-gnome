# Git 保存時点の記録

2026-11-13、利用者の指示に基づき、以下のリポジトリへ現在の開発成果を保存した。

- リモート：`git@192.168.1.152:yoshimi/keepass-gnome.git`
- ブランチ：`main`
- 初回ソースコミット：`52f3706` — `Initial GTK frontend with guarded KeePassXC core and compatibility tests`
- 保存対象：ソース、テスト、翻訳、CMake、上流パッチ、調査・設計記録、ライセンス等の51ファイル。本書は後続の記録コミットで追加する。
- 除外対象：`upstream/`、`build/`、`.cache/`、Python キャッシュ。パスワード DB や鍵ファイルは含めていない。
- 直前の検証結果：CTest 12項目成功。既知の互換性制約は [公開データ検証](upstream-fixture-compatibility.md) に記載。

指定先に既存の参照がないことを確認して初回コミットを作成した。既存ブランチの強制更新は行っていない。

このエージェント実行環境では、作業ルートの `.git` は空の読み取り専用マウントとして見えるため、通常の Git 管理領域として使用できなかった。今回の管理領域は `.cache/publication.git/` に作成し、作業ツリーを明示して操作した。ソースの別コピーを作成したり、`upstream/keepassxc/.git` を流用したりしていない。

この実行環境での状態確認は、プロジェクトのルートから次を実行する。

```sh
git --git-dir=.cache/publication.git --work-tree=. status
```

`.cache/` を削除すると、この実行環境のローカル Git 管理情報も失われる。リモートのコミットは残る。通常環境で新しく clone する場合は、通常の `.git` と Git コマンドを使用でき、この特殊な指定は必要ない。
