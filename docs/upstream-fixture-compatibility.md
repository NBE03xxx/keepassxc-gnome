# 公開された本家テストデータによる互換性検証

検証日：2026-11-13。上流：KeePassXC 2.8.0、commit `1c2f23ef5914c0f442a00533def1689857afb5a7`。

[互換性方針](compatibility-policy.md)の優先順位1・2を検証する。任意の未知構造を再保存する実装は行わない。

## 方法と結果

**確認済み。** 本家リポジトリの公開テスト用 KDBX 5件を私有一時ディレクトリにコピーし、変更していない本家 CLI の読込、GTK アダプターの解除・別名保存、同 CLI での再読込を試験した。参照 CLI は `build/reference/` に独立して構築する。元の公開ファイル・読込用コピーの SHA-256 が変わらないことも確認した（`prototype/core_probe.cpp:167`、`CMakeLists.txt:15`）。

| 入力 | 本家 CLI の読込 | アダプターでの往復 | 判定 |
| --- | --- | --- | --- |
| `NonAscii.kdbx` | 成功 | 成功 | 非 ASCII パスワード・非圧縮の対照 |
| `Compressed.kdbx` | 成功 | 成功 | 空パスワード・圧縮の対照 |
| `ProtectedStrings.kdbx` | 成功 | 成功 | 保護文字列・任意キー属性の対照 |
| `Format300.kdbx` 原本のコピー | 成功、無効なグループ参照1件の警告 | 解除を拒否 | 未解決モデル参照を保持できない。互換性上の制約 |
| `Format400.kdbx` 原本のコピー | 成功 | 解除を拒否 | 添付の保護フラグを保持できない。互換性上の制約 |
| `Format300.kdbx` の別コピーを本家 CLI で再保存 | 成功 | 成功 | 現在の本家 writer 出力との相互運用。原本の対応成功とは数えない |
| `Format400.kdbx` の別コピーを本家 CLI で再保存 | 成功 | 成功 | 同上 |

公開ファイル5件と、それに由来する別入力2件の計7入力である。**往復成功5件、原本の拒否2件**を区別する。拒否2件は合格した互換性ケースに数えず、理由、モデルを公開しないこと、保存先を作らないこと、元バイト列の維持を回帰試験する（`prototype/core_probe.cpp:190`）。CTest の成功は、この明示した期待動作が成立するという意味であり、拒否された入力を対応済みにするものではない。

本家での再保存は、さらに別の一時コピーだけに `db-edit -p` を実行し、同じ公開テスト用パスワードを再設定して行った。本家自身の正規化後の出力を検査する独立した実験である。アプリに変換経路を追加したり、利用者のファイルを自動修復したりしていない（`prototype/core_probe.cpp:300`、`upstream/keepassxc/src/cli/DatabaseEdit.cpp:86`）。

公開テストのパスワードの根拠は `upstream/keepassxc/tests/TestKdbx3.cpp:91` 以降の `testFormat300` / `testNonAscii` / `testCompressed` / `testProtectedStrings` と `upstream/keepassxc/tests/TestKdbx4.cpp:133`。実際の利用者の秘密・DB は使用していない。

## 保持を比較した範囲

**確認済み。** 成功した入力では暗号方式、KDF UUID、圧縮方式、DB 名・説明、グループとエントリー件数、グループ UUID・名前・ノートを比較した。各グループ内のエントリー UUID と `Entry::equals` の既定比較で、属性・保護指定、添付、CustomData の値、Auto-Type 関連、履歴等を比較した（`prototype/core_probe.cpp:220`、`upstream/keepassxc/src/core/Entry.cpp:938`）。

**限界。** 本試験は全メタデータ、全グループ設定、KDF の全パラメーター、形式の全バイトを比較していない。上流の `CustomDataItem::operator==` は項目の変更日時を比較しない（`upstream/keepassxc/src/core/CustomData.h:37`）。XML のバイト同一性や未知構造の保持を証明しない。編集後の TOTP・添付・履歴・任意属性・CustomData 等の保持は、既存の別の合成往復試験と併用する（`tests/xml_model_policy.cpp:141`、`prototype/core_probe.cpp:69`）。

## 拒否の根拠と未解決事項

- **確認済み：Format300。** 本家の reader は解決できないグループ参照を警告しつつ読込を続ける（`upstream/keepassxc/src/format/KdbxXmlReader.cpp:120`）。アプリ側の保持検査は同じ状態を `UnsupportedData` として拒否する（`patches/0004-xml-model-preservation.patch:179`）。参照の自動削除・置換は行っていない。
- **確認済み：Format400。** ガード付き reader の診断は `Binary protection flags cannot be preserved`。本家の reader は添付ヘッダー先頭のフラグをモデルに保持せず、本家 writer は `0x01` を出力する（`upstream/keepassxc/src/format/Kdbx4Reader.cpp:292`、`upstream/keepassxc/src/format/Kdbx4Writer.cpp:224`）。アプリは元のフラグを黙って変更しないよう拒否する（`patches/0004-xml-model-preservation.patch:87`）。今回フラグ保持モデルは追加していない。
- **技術的判断。** 現在の本家が読める全てのファイルと、本家が通常書き出すデータの範囲は一致しない。今回の正規化後2件の成功だけで前者の互換性を主張しない。
- **未検証。** 多様な Browser / SSH Agent / Secret Service 設定、カスタムアイコンの全属性、参照・親グループ情報等を含む、現在の本家 writer 出力の検証拡大が必要。正当な既知データを拒否する場合も互換性の制約として残す。Argon2 1.0、キー・ファイルや YubiKey による解除など、既存の未対応範囲も解決していない。

## 実行記録

`source tools/dev-env.sh` 後、`cmake --build build` と `ctest --test-dir build --output-on-failure` を実行し、全12項目が成功した。公開 fixture の試験は既存 `gnome_compatibility_tests` に追加した。ログは `.cache/upstream-fixture-compatibility.log`、`.cache/ctest.log`。キャッシュを削除しても、期待する結果は試験ソースと本書に残る。

今回、製品コード・上流パッチの追加変更、依存パッケージの導入、既存 KeePassXC の変更、実 DB の使用、GitHub への push、実インストールは行っていない。
