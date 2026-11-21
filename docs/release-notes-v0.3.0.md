# KeePassXC GNOME v0.3.0

Chrome安定版（deb）と公式のKeePassXC-Browserで、許可したHTTPSサイトのログインフォームにユーザー名とパスワードを入力できます。

- データベースメニューの「ブラウザー連携」からChromeとの連携を有効にします。拡張から接続を開始し、アプリで接続先のDBを確認して許可します。接続を保存するには、DBを保存してください。
- 入力を求められたときは、アプリに表示されたHTTPSの宛先とエントリーを確認し、「入力を許可」を選びます。フォームの送信はChromeで行います。
- DBをロックすると接続と承認待ちが閉じます。Chromeとの連携は、同じメニューから無効にできます。
- データベースメニューの「このアプリについて」で、アプリのバージョンを確認できます。

Ubuntu 26.04（amd64）用のdeb、完全対応ソース、ビルドmanifest、SHA256SUMSを添付しています。配布物は未署名です。debのSHA256を照合し、アプリを終了してからインストールまたは更新してください。

```bash
sudo apt install ./keepassxc-gnome_0.3.0-1_amd64.deb
```

ブラウザー連携はUbuntu 26.04.1 / GNOME 50 / Wayland上で、Chrome 158.0.8127.36とKeePassXC-Browser 1.10.4.1を使って確認しました。

ソースからビルドする場合は、固定した上流ソースを含む添付の `keepassxc-gnome_0.3.0-1.source.tar.xz` を使ってください。

長時間の利用や反復操作でメモリ使用量が増える既知の問題があります。ブラウザー連携の手順と対応範囲は[利用ガイド](https://github.com/NBE03xxx/keepassxc-gnome/blob/v0.3.0/docs/user-guide.md)を参照してください。
