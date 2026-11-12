# KDBX の読み込み終端検証

更新日：2026-11-12。対象：KeePassXC `1c2f23ef5914c0f442a00533def1689857afb5a7`。

## 再現した問題

調査報告書 §22 の一時コードに依存せず、`tests/reader_finalization.cpp` に合成データ生成と故障注入を保持した。暗号、圧縮、ヘッダー解析、ブロック書き込みには上流の実装を使用する。テスト用の既知の鍵とエントリーだけを使い、製品にはこの生成コードをリンクしない。

**修正前に確認済み。** 最初の244ケースは KDBX 4.0 / 4.1、AES-256-CBC / Twofish-CBC / ChaCha20、圧縮あり・なし、ルート後の約256 KiB の正当な XML コメントあり・なしを組み合わせた。正常な対照24件は成功した。不正な入力130件も受け入れられた。

| 不正な入力 | 修正前に受け入れた件数 |
| --- | ---: |
| HMAC 終端の後の1バイト | 24 |
| 二つ目の XML ルート | 24 |
| 閉じない末尾 XML コメント | 24 |
| 正当な HMAC で包んだ不正な CBC パディング | 16 |
| 終端 HMAC の破損 | 6 |
| 終端ブロックの削除 | 6 |
| 終端の最後の1バイトの削除 | 6 |
| Gzip メンバー後のデータ | 12 |
| Gzip CRC の破損 | 6 |
| Gzip トレーラーの欠落 | 6 |

不正なパディング・CRC 等を含む入力は、既知の合成キーを用いて再暗号化・認証した試験である。鍵を知らない第三者が HMAC を偽造できることを示すものではない。記録は `.cache/reader-finalization-baseline.log`。

## 実装した境界

API の読み込み成功値だけでは文書とストリームの完了を確認できないため、理由を報告した後に上流に対する局所的な差分を作成した。参照チェックアウトは変更せず、[パッチの管理手順](../patches/README.md) に従ってビルド用コピーへ適用する。

- `KdbxXmlReader` は `KeePassFile` の閉じタグの後も XML の `EndDocument` まで読む。正当な末尾コメントや空白は許可し、複数ルートや未完了の字句を拒否する。
- `SymmetricCipherStream` は CBC の最後のブロックを先読みで識別し、上流の `SymmetricCipher::finish` に渡す。認証ブロックの読み取り失敗を EOF と扱わない。`finishReading` は残余平文や未完了の暗号処理を拒否する。
- `QtIOCompressor::isReadComplete` で圧縮終端を確認する。ローカルの完全なファイル・バッファを読む経路では、`Z_STREAM_END` の前に基底入力が尽きた場合をエラーとする。先読みデータを戻す際の範囲外の1バイトも除去した。
- KDBX 4 reader は上記に加えて HMAC 終端とファイル EOF を確認する。KDBX 3 reader はハッシュ終端と暗号処理完了を確認する。
- KDBX 3 のハッシュストリームでは、同期読み取りに失敗した後を「入力が来る可能性がある状態」と報告しないようにした。読み取りエラー自体は保持して完了検査で拒否する。これにより、末尾コメント後の破損終端で XML reader が再試行し続けるケースを防ぐ。

根拠は `patches/0001-reader-finalization.patch:1`。上流の接点は `upstream/keepassxc/src/format/Kdbx3Reader.cpp:114`、`upstream/keepassxc/src/format/Kdbx4Reader.cpp:153`、`upstream/keepassxc/src/format/KdbxXmlReader.cpp:111`、`upstream/keepassxc/src/streams/SymmetricCipherStream.cpp:124`、`upstream/keepassxc/src/streams/qtiocompressor.cpp:515`。適用は `cmake/CoreOverrides.cmake:1` と `tools/prepare-core-overrides.py:14` で行う。

**採用した扱い。** 終端後の未解釈データ、連結した別の Gzip メンバーを黙って捨てず、解除を拒否する。元ファイルは変更しない。必要なデータが末尾に追加されている可能性がある入力を、切り詰めて保存する機能は設けない。非同期のネットワーク入力を待つ reader としては使用しない。

CoreSession は候補 Database を非公開で構築し、reader が失敗すれば破棄する。ロック状態からの解除を拒否したケースでは一覧、グループ、既知 ID のパスワード、保存のいずれも利用できないことを回帰試験で確認する（`prototype/CoreSession.cpp:149`、`tests/ReaderFixtures.h:274`）。

## 上流テストと互換性の検証

`cmake/CoreTests.cmake:1` で上流の暗号、HMAC、ハッシュブロック、KDBX 3 / 4 のテスト本体を再利用し、Qt Test とコアへ接続する。上流の Qt Widgets GUI ターゲットをビルドする必要はない。

上流の鍵変更試験では `QBuffer::seek(0)` だけで再書き込みするため、新しい出力が短いと以前の末尾が残り、厳格な EOF 検査で拒否された。コピーした試験の再書き込み前にバッファを空にする `0002-tests-truncate-reused-buffer.patch` を加えた。元の上流テストをそのまま実行したという扱いにはしない。その他のテスト本体とアサーションは維持する。

相互運用にはパッチのない上流 CLI を `build/reference/` でビルドして使用する（`CMakeLists.txt:15`、`:51`）。KDBX 3.1 / 4.0 / 4.1 の選択した属性・履歴・添付・TOTP の保持を確認する既存試験も継続する。

## 最終結果

**確認済み。** KDBX 3.1 と CBC の不完全なブロックを加えた352ケースが全て期待どおりになった。正常対照32件を読み込み、不正な320件を拒否した。拒否後に一覧、秘密の取得、保存ができないことも検査した。KDBX 3.1 では AES-256-CBC / Twofish-CBC、KDBX 4.0 / 4.1 ではさらに ChaCha20 を使った。データブロックは4093バイトにして暗号のブロック境界ともずらした。テスト用 AES-KDF の反復は高速な合成データ用設定であり、製品の新規 DB の既定値には使っていない。

`ctest --test-dir build --output-on-failure` は10項目全て成功、約13秒。再利用した上流の5実行ファイルは、Qt Test の初期化・終了を含む集計で計300項目成功、失敗・スキップなし。上流試験の全てを実行したという意味ではない。

同じアプリで `bash tools/test-ui.sh` も成功した。独立した GNOME Wayland セッションで、日本語の DB 作成、エントリー作成・編集、検索、グループ作成、未保存の破棄確認と手動ロックを確認した。ファイル選択や明暗・画面ロック・スリープの実環境試験はこの確認に含まない。

適用ツールを再実行して、内容が同じコピーの更新時刻を変えないことを確認した。合成の参照コピーの対象ファイルを変更した対照では、ハッシュ不一致で処理が失敗し、出力を作らなかった。参照チェックアウトの追跡ファイルは変更なし。

Qt の生成コードについても、実際にコンパイルする `mocs_compilation.cpp` から3種類のストリームの生成ソースを辿り、全てがコピーした対応ヘッダーを参照していることを確認した。古い生成ディレクトリの残存ファイルだけを検査する方法は使わない。構成とパッチのハッシュを含む専用の生成先へ切り替えた最終ビルドで、CTest と日本語 UI 操作を再確認した。

ログは `.cache/reader-finalization.log`、`.cache/ctest.log`、`build/Testing/Temporary/LastTest.log`、`build/ui-test/result.log`。再実行は通常のビルド後に次を使う。

```bash
source tools/dev-env.sh
ctest --test-dir build --output-on-failure
bash tools/test-ui.sh
```

通常環境のパッケージ追加・既存 KeePassXC の変更・実 DB の使用・push・実インストールは行っていない。

## 残る制約

この変更だけで実データ利用を許可しない。KDF と読み取り量には、その後 [暫定上限](read-resource-policy.md) を実装した。XML 階層・モデル件数と保持不能データの拒否も、その後 [追加](xml-model-preservation.md) した。KDF の完全な相互運用は未解決である。任意の未知構造の再保存は、その後の [範囲確定](compatibility-policy.md) により検討記録のみとし、完成条件から外した。文書全体を読む変更は読み取り量を増やすため、特に展開サイズ等の上限との組み合わせが必要。

読み取り中のファイル差し替え・変更、並列解除と中断、鍵や文字列の残留、保存の同期と障害復旧、画面ロック・スリープ、Wayland のクリップボード失効も今回の終端検査の保証に含めない。より多数のブロック境界、すべての暗号・KDF の組み合わせ、ファザーと独立したレビューは未完了。
