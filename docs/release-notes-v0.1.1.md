# KeePassXC GNOME v0.1.1

文字を読みやすくするための更新です。

- 基本の文字サイズを約20%拡大しました。
- エントリー一覧のユーザー名を、タイトルと同じ文字サイズに揃えました。
- ヘッダーのファイル名・状態と、編集画面の項目名も読みやすいサイズに揃えました。
- システムの文字設定を基準とし、高コントラストでも拡大を維持します。

暗号処理・KDBXの保存形式は変更していません。フォント調整版はUbuntu 26.04のVMで検証し、T16で利用者が表示を確認済みです。

## インストール・更新

Ubuntu 26.04（amd64）向けです。起動中のアプリで変更を保存して終了し、debのSHA256を`SHA256SUMS`と照合してから実行してください。

```bash
sudo apt install ./keepassxc-gnome_0.1.1-1_amd64.deb
```

本家KeePassXCと共存でき、設定・履歴・復旧データは引き継ぎます。完全対応ソース、ビルドmanifest、SHA256SUMSも添付します。配布物は未署名です。

長時間・反復操作でメモリ使用量が増える既知の問題は継続しています。日常的な短時間の個人利用を対象とし、大切なデータベースはバックアップしてください。キーファイル・YubiKeyでの解除、ブラウザー連携、Auto-Type、TOTPのコード生成・表示は未対応です。既存のTOTP設定は保持します。

詳しくは[README](https://github.com/NBE03xxx/keepassxc-gnome#readme)と[利用ガイド](https://github.com/NBE03xxx/keepassxc-gnome/blob/v0.1.1/docs/user-guide.md)を参照してください。
