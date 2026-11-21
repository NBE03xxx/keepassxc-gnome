# KeePassXC GNOME v0.1.0

Ubuntu 26.04 / GNOME 50 / Wayland向けの初回リリースです。KeePassXCのコアを利用する独立したGTK4・libadwaitaアプリです。

- KDBX 3.1 / 4.0 / 4.1の読み書き、グループ・エントリー管理、検索・プレビュー
- パスワード生成・コピー、自動ロック、バックアップ・未保存変更の復旧
- 日本語・英語、システムの明暗・アクセントカラーへの追従

日常的な短時間の個人利用を対象とします。長時間・反復操作ではメモリ使用量が増加する既知の問題があります。キーファイル・YubiKeyによる解除、ブラウザー連携、Auto-Type、TOTPのコード生成・表示は未対応です。既存のTOTP設定は保持します。重要なデータベースはバックアップしてください。

## 導入

amd64用debのSHA256を`SHA256SUMS`と照合し、次を実行してください。

```bash
sudo apt install ./keepassxc-gnome_0.1.0-1_amd64.deb
```

本家KeePassXCを置き換えません。以前の本プロジェクトの開発用パッケージがある場合は、そのパッケージを置き換えます。設定・履歴・復旧データの保存場所は引き継ぎます。

対応ソースとビルドmanifestも添付します。配布物は未署名です。詳細はREADME、利用ガイド、SECURITY.mdを参照してください。


v0.1.0の時点では、キーファイルでの解除、ブラウザー連携、Auto-Type、TOTPを今後の実装予定としていました。[v0.1.0のREADME](https://github.com/NBE03xxx/keepassxc-gnome/blob/v0.1.0/README.md#ロードマップ)
