# KeePassXC GNOME v0.5.0

エントリーごとにTOTPを設定し、認証コードと残り時間を表示できるようになりました。

- プレビューの「TOTP」を開き、サービスの手動設定用Base32キーか `otpauth://totp/…` のURIを入力します。本家KeePassXCで設定済みのTOTPも表示できます。
- 認証コードは大きな等幅文字で表示され、有効期限ごとに自動で更新されます。画面はGNOMEのライト・ダーク設定に追従します。
- 設定を登録・変更・削除したら、メイン画面の「保存」でDBに書き込みます。[TOTPの利用手順](https://github.com/NBE03xxx/keepassxc-gnome/blob/v0.5.0/docs/user-guide.md#totp)

## インストール・更新

Ubuntu 26.04（amd64）向けのdeb、完全対応ソース、ビルドmanifest、SHA256SUMSを添付します。配布物は未署名です。SHA-256を照合し、アプリを終了してからインストールまたは更新してください。

```bash
sudo apt install ./keepassxc-gnome_0.5.0-3_amd64.deb
```

長時間の反復操作でメモリ使用量が増える既知の問題があります。ブラウザー連携とAuto-Typeが入力する項目は利用ガイドで確認できます。[利用ガイド](https://github.com/NBE03xxx/keepassxc-gnome/blob/v0.5.0/docs/user-guide.md)

## 文書の更新

READMEにライト・ダークの表示イメージを追加しました。生体認証によるデータベースの解除とSecret Service（秘密サービス）統合の制限は、更新後のREADMEと利用ガイドで確認できます。[README](https://github.com/NBE03xxx/keepassxc-gnome/blob/main/README.md) · [利用ガイド](https://github.com/NBE03xxx/keepassxc-gnome/blob/main/docs/user-guide.md#対応する入力)
