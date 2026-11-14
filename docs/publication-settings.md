# 公開用メタデータの準備

2026-11-14。非公開Giteaへのチェックポイントpush後に作成した草案。公開操作・公開リポジトリの作成・鍵生成は行わない。値が未定でも、私有環境での検証と文書の整備は進める。

| 項目 | 現在値・方針 | 公開前に必要な指定 |
| --- | --- | --- |
| プロジェクト名 | KeePassXC GNOME Native Edition | 名称変更の希望があれば反映。本家公式版ではないことを明記する |
| 開発版の表示名 | KeePassXC GNOME — 開発版 / KeePassXC GNOME Development | リリースの位置付けと併せて表示を確定する |
| パッケージ | keepassxc-gnome-development | 現在の検証候補では維持。本家を置き換えるProvides / Conflicts / Replacesは追加しない |
| アプリID | org.keepassxc.Gnome.Development | 正式な公開識別子は公開先の保守者が管理できる名前空間で確定。変更時は配置・起動・更新試験を行う |
| 公開先 | 利用者指定：`git@github.com:NBE03xxx/keepassxc-gnome.git`、予定URLは `https://github.com/NBE03xxx/keepassxc-gnome` | 存在・公開状態を確認したとの意味ではない。新たな指示までGitHubへのpush・公開を行わない |
| Maintainer | 利用者指定の保守者：`NBE03xxx`。メールは未定 | 公開パッケージ用のメールは未確定。候補の既存 `noreply@localhost` は内部検証用で、連絡窓口と表示しない |
| 問題・セキュリティ報告先 | 利用者回答：未定 | 一般の問題と秘密を含み得る脆弱性の非公開報告先を区別する。実DBや資格情報の送付は求めない |
| 著作権・ライセンス | 既存の寄稿者表示、GPL version 2または3、上流・第三者の表示を保持 | 公開用の表示を確認する。対応ソースはバイナリと同時に提供する |
| 署名 | 検証候補は未署名。鍵を自動生成しない | 署名を採用するかを含め、公開時の方針を確定する。採用時は署名管理者・公開鍵配布先を指定する |
| 対象 | Ubuntu 26.04 / GNOME 50 / Wayland、日常的な短時間の個人利用 | 既存の対象方針を維持する |

実装上の参照先は `packaging/org.keepassxc.Gnome.Development.desktop`、`packaging/Install.cmake`、`packaging/copyright`、`tools/build-deb.py:32`、`src/main.cpp:1139`。現在のdebは開発用の識別子・表示・説明を持つ。値を決めただけで、実機へ新しいバイナリを導入した扱いにはしない。

公開用の連絡先や署名方針が未定の間に候補を作る場合は、非公開の開発候補として明示し、公開済み・署名済み・実データ利用承認済みとは扱わない。正式な識別子・実行動作を変更した場合はVMを先に確認し、実機確認は動作変更のある箇所へ絞る。

## 利用者による公開の条件

利用者は2026-11-14に、アプリの起動・操作を自分で一通り確認した後、改めて公開を指示すると指定した。**確認結果の回答や作業継続の指示だけをGitHub公開の許可と扱わない。** ご自身の確認手順は [起動・操作確認](manual-acceptance.md)。確認後の新しい明示指示を待ち、リポジトリ作成・GitHub push・リリース公開を自動実行しない。


## v0.1.0で確定した設定（2026-11-14）

上の草案に優先する。表示名は日英ともKeePassXC GNOME、公開deb名は `keepassxc-gnome`、版は `0.1.0-1`、Gitタグ予定は `v0.1.0`。既存設定・履歴・ファイル関連付け候補を引き継ぐため、内部ID `org.keepassxc.Gnome.Development` は維持する。IDの末尾はユーザー向けの名称へ表示しない。旧開発パッケージだけへのConflicts/Replacesを指定し、本家のパッケージやFlatpakを置き換えない。

利用者の回答により、Maintainerは `NBE03xxx <NBE03247@nifty.com>` とする。公開時にGitHub Private vulnerability reportingを有効化し、SECURITY.mdの報告リンクの利用可否を確認する。現時点では未有効化・未公開。配布物は未署名と明記し、鍵生成はしない。署名を追加する場合は別途鍵管理を決める。

今回の指示はREADME・公開表示・配布物の準備と完了報告であり、GitHubの変更・push・リリース作成は行わない。準備後に公開指示を受け、Gitタグとリリースへ同じソース・deb・manifest・SHA256SUMSを紐付ける。


## 公開指示と実機導入（2026-11-14）

利用者がT16への公開版導入後にGitHubへv0.1.0をリリースするよう明示指示したため、従来の公開保留は解除された。T16への公開版0.1.0-1導入と起動を確認し、GitHub Private vulnerability reportingを有効化してAPIのenabled=trueを照合した。以降のcommit・tag・push・Release公開はこの指示に基づいて実施する。[現在の結果](release-v0.1.0-publication.md)。


## 公開完了

[v0.1.0](https://github.com/NBE03xxx/keepassxc-gnome/releases/tag/v0.1.0)を公開した。T16への公開版導入後に実施し、Private vulnerability reportingの有効化、latest、認証なし参照、タグと配布物の一致を確認した。公開済みの正式な結果は[公開記録](release-v0.1.0-publication.md)を参照する。上の草案・保留の記述は過去時点の記録である。
