# ストリーム暗号の空最終ブロックと保存の間欠失敗

2026-11-13、GUI / Orca の回帰検証中に発見。対象 KeePassXC 2.8.0、commit `1c2f23ef5914c0f442a00533def1689857afb5a7`。

## 確認した問題

通常の保存試験でコピー保存・別名保存・その直後の保存が間欠的に SerializationError になった。元ファイルは不変で、モデルの dirty は維持された。再実行で成功するだけでは未解決として調べた結果、上流 writer は `Cannot process 0 length data.` を返していた。

`src/streams/SymmetricCipherStream.cpp` の上流実装では、ストリーム暗号に1024バイトのバッファを使い、満杯になれば `writeData()` で処理して空にする。一方 `reset()` はデータを書いたことがあれば `writeBlock(true)` を呼ぶ。ChaCha20 / Salsa20 は最終 padding がなく、この時点で空なら処理すべき内容はないが、従来の `writeBlock()` は `SymmetricCipher::process()` に空のバッファを渡していた。`src/crypto/SymmetricCipher.cpp` は長さ0を明示的に拒否する。KDBX の圧縮出力の長さは RandomSlug 等で変わるため、通常の DB 保存では間欠的に見えた。

これは参照した上流コードの境界処理であり、GTK の表示や今回の accessible label 修正を原因とはしない。

## 修正の範囲

本家修正が必要な理由を利用者へ先に報告し、`patches/0005-stream-cipher-empty-final-block.patch` を追加した。参照用の `upstream/keepassxc/` は変更せず、従来どおり manifest 検証・fuzz=0 を経てビルド用コピーへ適用する。

`m_streamCipher && lastBlock && m_buffer.isEmpty()` に限り、以前の error がなければ正常終了とする。AES の最終ブロックはこの条件に入らず、既存の `finish()` が padding を行う。非空の暗号処理、暗号方式、KDF、KDBX の形式・認証・reader 検査は変更していない。長さ0を暗号ライブラリ全体で無条件に許可する修正ではない。

## 決定的な回帰試験

`tests/stream_finalization.cpp` を、無変更 core の参照ターゲット `gnome_reference_stream_tests` と、パッチ適用 core の `gnome_stream_finalization_tests` にそれぞれリンクする。前者には `CoreOverrides` を適用しない。

ChaCha20 / Salsa20 / AES-256-CBC、長さ0・1・15・16・17・1023・1024・1025・2047・2048・2049・4096、まとめ書込みと137バイトごとの分割書込みを比較する。各出力が本家の直接 cipher API の結果と一致し、AES padding と終了後の重複出力がないことを検査する。参照側のストリーム暗号の1024倍数だけは従来の失敗を期待して再現し、修正側はすべて正常に終了する。

通常 CTest は新しい2ターゲットを含む21件で成功した。1回の成功だけで間欠失敗が消えたとは判断せず、保存試験の50回反復と ASan / LSan 境界検査も実施する。最新結果は [GUI・Orca の確認](gui-memory-accessibility.md) に記録する。

診断の一時的な writer logging と子プロセス stderr 転送は独自ソースから取り除いた。通常の worker は stderr を `/dev/null` に向ける既存の保護を維持する。診断後に再ビルドし、診断用文字列が製品バイナリに残っていないことも検査する。

```bash
source tools/dev-env.sh
cmake -S . -B build -G Ninja
cmake --build build --parallel 4
build/reference/gnome_reference_stream_tests --reference
build/bin/gnome_stream_finalization_tests
ctest --test-dir build --output-on-failure
ctest --test-dir build -R '^gnome_save_tests$' --repeat until-fail:50 --output-on-failure
bash tools/test-reader-memory.sh --stream-only
```

本件を修正するために、保存の検証・暗号化復旧・外部更新の検知を緩める変更はしない。未導入の候補ソースに対する修正であり、既存の実機導入版の修正済みを意味しない。
