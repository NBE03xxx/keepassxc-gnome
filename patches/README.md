# 上流に対する局所的な差分

対象は KeePassXC `1c2f23ef5914c0f442a00533def1689857afb5a7`。
`upstream/keepassxc/` は参照専用とし、次のパッチを `build/native/gnome-core/` のコピーへ順に適用する。

1. `0001-reader-finalization.patch`：XML 文書全体の終了、暗号ストリームの完了・パディング、ブロック終端、Gzip 終端・CRC と残余データを検査する。暗号処理は既存の `SymmetricCipher` / Botan を使用する。
2. `0002-tests-truncate-reused-buffer.patch`：鍵変更試験で同じ QBuffer に再保存するとき、前の長い出力の残りを切り詰める。試験だけの変更で、製品の reader / writer の条件は緩めない。
3. `0003-read-resource-policy.patch`：アプリの解除スコープで KDF の元の型・値、ヘッダー・辞書・ブロック・復号と展開の量を検査する。ポリシーと診断はプロジェクトの `src/gnome/ReadBudget.h` へ局所化する。詳細は [資源制限の記録](../docs/read-resource-policy.md) を参照。
4. `0004-xml-model-preservation.patch`：XML の追跡とモデル生成・追加の予算、保持できない項目の拒否を接続する。未知データをモデルへ取り込んで書き戻す機能は未実装。対応範囲と試験は [XML・保持の記録](../docs/xml-model-preservation.md) を参照。

実測と制約は [終端検証の記録](../docs/reader-finalization.md) を参照。

`tools/prepare-core-overrides.py` がコミットと対象ファイルの SHA-256 を `reader-finalization.json` と照合する。パッチは fuzz を禁止して適用する。ビルドには差分のあるソースと対応するヘッダーを使い、上流更新で接点が変われば構成時に停止する。新しい上流への追従時は、パッチの内容と回帰試験を見直した上でコミットとハッシュを更新する。

Qt の自動生成コードも同じヘッダーを参照させる。`cmake/CoreOverrides.cmake` は移動したストリーム実装に対応する4ヘッダーを AUTOMOC の対象として明示的に登録し、元のヘッダーを対象から外す。生成先は `build/native/gnome-core-autogen/<fingerprint>/` とする。指紋には構成スクリプト・マニフェスト・製品パッチのハッシュを含め、旧構成の生成コードを再利用しない。

無変更の上流 CLI は別の `build/reference/` でビルドし、相互運用試験に使用する。アプリ側のパッチをこの参照ビルドに適用しない。
