# KeePassXC GNOME v0.4.0

GNOME Waylandで、選んだエントリーを別のアプリへ自動入力できるようになりました。

- DBを解除し、対象エントリーを選んでから「自動入力」を開きます。GNOMEでキーボード操作を許可し、入力先の最初の欄でGNOMEに登録されたショートカットを押してキーを離します。
- ユーザー名、パスワード、Tab、Enterなど、対応する既定の入力手順を使います。Enterを含む手順では、フォームが送信される場合があります。入力が終わるまで対象ウィンドウを切り替えないでください。
- DBをロックすると、準備中や実行中の自動入力が取り消されます。現在のキー配列で入力できない文字や未対応の命令がある場合は、入力前に案内します。

## インストール・更新

Ubuntu 26.04（amd64）用のdeb、完全対応ソース、ビルドmanifest、SHA256SUMSを添付します。配布物は未署名です。SHA-256を照合し、アプリを終了してからインストールまたは更新してください。

```bash
sudo apt install ./keepassxc-gnome_0.4.0-1_amd64.deb
```

## 日本語IMEの自動切り替え

同梱のGNOME Shell拡張を有効にすると、Auto-Typeの間だけ直接入力に切り替わり、入力後に元の入力ソースへ戻ります。debを導入した後、GNOMEから一度ログアウトしてログインし、端末で次を実行してください。

```bash
gnome-extensions enable keepassxc-auto-type-ime@keepassxc-gnome
```

GNOME 50 Waylandの「日本語（Mozc）」と「日本語」の組み合わせで確認済みです。「ウィンドウごとに異なる入力ソース」はオフにしてください。拡張を使わない場合は、Auto-Typeの前に入力先を直接入力にします。 [READMEの有効化手順](https://github.com/NBE03xxx/keepassxc-gnome/blob/v0.4.0/README.md)

自動入力の対応範囲と既知のメモリ制限は[利用ガイド](https://github.com/NBE03xxx/keepassxc-gnome/blob/v0.4.0/docs/user-guide.md)を参照してください。ソースからビルドする場合は、固定した上流を含む添付の `keepassxc-gnome_0.4.0-1.source.tar.xz` を使用してください。
