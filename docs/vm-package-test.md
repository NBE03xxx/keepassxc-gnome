# Ubuntu 26.04 VM でのパッケージ確認

通常環境ではインストールしない。最初に、合成データのみを使う Ubuntu 26.04 VM で導入・更新・削除を確認する。

最新の実施記録は [D-Bus修正版dev5の作成とVM検証](session-guard-candidate-validation.md)。導入・更新・purge、配置先の実アプリによる日本語CRUD／ロックと日英の接続失敗時の受付禁止、寿命103チェック、前後の全パッケージ状態一致を確認した。VMは削除試験後に通常シャットダウン済み。以下のdev1の記録は過去の試験として保持する。

## 事前確認

- VM 名だけで OS を決めず、ゲストの `/etc/os-release` が Ubuntu 26.04 であることを確認する。
- 元へ戻せるスナップショットと、ゲスト内の管理者操作手段を確保する。
- ホストのパスワード DB を共有・転送しない。クリップボード共有も秘密の試験には使わない。
- `SHA256SUMS` と対応ソースを照合し、`apt-get --simulate --no-remove install ./…deb` の追加・更新依存を確認する。新たな依存の導入は事前に相談する。
- GNOME / Wayland の通常ログイン、portal の起動、必要な日本語ロケールを確認する。

## 導入・更新・削除

`tools/vm-package-test.sh` は VM 内で使う。Ubuntu 26.04 と仮想マシンの判定を行い、既存の本開発パッケージがある場合は拒否する。通常環境へ実行を流用しない。

```bash
# 先に、対応ソースから第1版・第2版を作成する（実装が同じでも版を変えて更新経路を確認）。
python3 tools/build-deb.py --version 0.1.0~dev1-1 --output build/dist/install
python3 tools/build-deb.py --version 0.1.0~dev1-2 --output build/dist/upgrade

# VM 内：まず変更のない計画確認。
bash tools/vm-package-test.sh --plan /path/to/first.deb

# 追加依存を確認・合意後、VM 内のみで実施。
sudo bash tools/vm-package-test.sh --install /path/to/first.deb /path/to/second.deb
```

導入前後で本家 `keepassxc` の版と `dpkg --verify` の結果を比較する。パッケージの version / locale チェック、3実行ファイルと翻訳・desktop・icon の配置、更新後の版、削除後のファイルと合成ユーザーデータの保持を確認する。試験の途中で失敗した場合は自動 purge / スナップショット復帰を行わず、状態とログを調べる。依存パッケージの autoremove もしない。

## 通常ログインでの画面確認

ゲストの一般ユーザーから、合成試験専用の XDG 設定・状態ディレクトリを指定して起動する。開発 prefix の `LD_LIBRARY_PATH` を持ち込まず、導入した Ubuntu ライブラリで動かす。

- アプリ一覧の日本語・英語の表示名とアイコン、起動、ライト／ダークへの自動追従。
- 実際の GNOME FileChooser による開く・新規名保存・コピー・復旧フォルダー選択。取消と再選択、親ウィンドウとの関係。
- 合成 KDBX の作成・解除・編集・検索・保存・ロック。別途のコア試験を通ったものだけを使う。
- 狭い画面、拡大表示、キーボード、アクセシビリティの操作名。
- 本家の起動と既定のファイル関連付けが変わっていないこと。

画面ロック・スリープの実機確認は手順7。VM の結果でホストのロックや Wayland clipboard の保持を確認済みとは扱わない。試験記録にはゲストの OS・GNOME・ライブラリ版、deb とソースの SHA-256、成否・残件を残す。

## 2026-11-13 の実施記録

既存 VM `ubuntu26.04` は開始時に停止しており、`keepass-gnome-before-packaging-20261113` の復元用スナップショットを作成してから起動した。ゲスト agent で Ubuntu 26.04.1 LTS / x86_64 と確認し、利用者が実行時依存3件の導入と更新・削除試験を承認した。ホストに追加パッケージは導入していない。

開発版の導入、配置後の CLI と私有 worker の合成 DB 操作、同じ実装で Debian revision を上げた更新、purge と合成 sentinel の保持が成功。追加した依存3件とスナップショットは保持し、自動のスナップショット復帰や依存削除は行わない。本家 KeePassXC はゲストでは未導入であり、併存試験済みとは扱わない。通常ログインの GNOME 画面操作と実機の確認も別に残す。

最初の試行は試験 runner の worker 指定の問題で中断した。状態を調べて開発版だけを削除し、runner を修正して全経路を再実行した。失敗を成功に読み替えたり、VM の安全機能を解除したりしていない。詳細は [配布・保守の結果](distribution-maintenance.md#今回の確認結果)。
