# 本家が通常保存する既知データの往復保持

検証日：2026-11-13。対象：KeePassXC 2.8.0、commit `1c2f23ef5914c0f442a00533def1689857afb5a7`。

[互換性方針](compatibility-policy.md)の優先順位1の試験を拡充し、コピー時の参照解決を修正した。任意の未知構造の再保存は実装していない。

## 検証方法と確認済みの結果

**確認済み。** 無変更の本家コアにだけリンクする `gnome_reference_fixture` を `build/reference/` で構築する。CMake 接続と試験コードはプロジェクト側に置き、本家の参照チェックアウト、既存の上流パッチには変更を加えない。参照ツールが CoreOverrides、gnome_session、GTK にリンクしないことを実際のコンパイル・リンクコマンドでも確認した（`cmake/AttachReference.cmake:1`、`cmake/ReferenceFrontend.cmake:1`、`CMakeLists.txt:15`）。

参照ツールは、合成モデルを本家 writer で KDBX に保存し、本家 reader で読み戻した値を JSON として返す。アプリの `CoreSession` が解除・別名保存したファイルを、参照ツールの別プロセスで読み戻して比較する。暗号、KDF、KDBX の再実装は行っていない。同じ KeePassXC 由来の処理との相互運用であり、別製品・別の暗号実装との相互運用を証明する試験ではない（`tests/known_data.cpp:298`）。

| 形式 | 暗号方式 | KDF | 圧縮 | 組合せ数 |
| --- | --- | --- | --- | ---: |
| KDBX 3.1 | AES-256、Twofish | AES-KDF | なし／gzip | 4 |
| KDBX 4.0 | AES-256、Twofish、ChaCha20 | AES-KDF、Argon2d 1.3、Argon2id 1.3 | なし／gzip | 18 |
| KDBX 4.1 | 同上 | 同上 | なし／gzip | 18 |

40組合せについて、編集なし・エントリーのタイトル編集ありの2通り、**計80回の往復が成功**した。読み込み元と先に保存したコピーのハッシュが、後の編集・保存で変わらないことも確認した。

## 比較する既知データ

比較対象は明示的なモデルの断面であり、上流の `operator==` だけに依存しない。

- DB：形式、暗号方式、圧縮方式、KDF パラメーター、公開 CustomData。型付き値は QVariant の型を含めて比較し、64ビット整数を JSON の浮動小数点に変換して比較しない。
- メタデータ：名前・説明・既定ユーザーとそれぞれの変更日時、鍵変更日時・期限設定、保護設定、履歴上限、色、ごみ箱・テンプレート・選択／表示グループの参照。
- カスタムアイコン：UUID、順序、画像バイト列、KDBX 4.1 の名前と変更日時。アイコンの上流 `operator==` は画像だけを比較するため、名前・日時は直接比較する（`upstream/keepassxc/src/core/Metadata.h:68`）。
- グループ：UUID、親、順序、名前・ノート、アイコン、各日時、展開状態、Auto-Type・検索の設定、表示エントリー参照、CustomData、4.1 のタグ・以前の親。
- エントリー：属性と保護指定、バイナリ・空の添付、CustomData、TOTP 設定、Auto-Type、色・タグ・URL 上書き、日時・使用回数、4.1 の品質検査指定・以前の親、履歴。
- UUID 参照を使ったパスワードと、文字列に埋め込んだユーザー名参照について、コピー結果が本家の解決結果と一致すること。

比較コードは `tests/known_data.cpp:53`、`:79`、`:122`。編集後はタイトル、当該エントリーの更新・アクセス日時、追加された履歴1件だけを許可し、それ以外の選択項目が変わらないことを比較する。更新日時は元の合成日時より新しく、実行時刻を超えないことも検査する（同ファイル `:328`）。

## 比較対象を限定する理由

以下は、保持失敗を成功に見せるための包括的な除外ではなく、元の形式で実際に保存される値と実行時の値を区別するための明示的な条件である。

- 保存時に更新する KDF salt `S` と、本家が自動管理する `_LAST_MODIFIED` / `KPXC_RANDOM_SLUG` は、同値を要求しない。KDF の方式、反復、メモリ、並列度、版等は比較する。
- KDBX 3.1 の `SettingsChanged` は本家 writer が出力しない。reader が初期化した実行時の日時は比較しない（`upstream/keepassxc/src/format/KdbxXmlWriter.cpp:163`）。
- CustomData の項目日時は、元のファイルに保存される KDBX 4.1 の Meta 項目を比較する。Group / Entry の項目日時と旧形式の Meta 項目日時は本家 writer が出力せず、未指定なら reader 経由の `CustomData::set` が実行時刻を補う（`upstream/keepassxc/src/format/KdbxXmlWriter.cpp:279`、`upstream/keepassxc/src/core/CustomData.cpp:88`）。ファイルに存在するのに保存できない日時を含む入力の拒否は、既存の XML 保持検査で別途試験する。
- TOTP の表示用 URI はタイトル等から動的に組み立てられるため、元属性と解析した設定値を比較する。タイトル編集による表示用 URI の変化を秘密の消失と扱わない（`upstream/keepassxc/src/core/Entry.cpp:654`）。

## 発見した不具合と修正

**再現済み。** コピー操作が編集用の生の属性を使っていたため、参照式を持つユーザー名・パスワードで、本家がコピーする解決後の値と異なっていた。回帰試験を先に追加し、`password copy resolves references as upstream does` の失敗を確認した（`.cache/known-data-reference-regression.log`）。

**修正・確認済み。** `CoreSession::copyUsername` / `copyPassword` は、本家 GUI と同じ `Entry::resolveMultiplePlaceholders` を呼ぶ。GTK のコピー操作もこの API を使用する。編集用の生の属性や参照式を書き換えず、保存後の参照保持も上記80往復で比較する。ユーザー名のコピー時に、不要なパスワードを含む `EntryDetails` を取り出す経路もなくした（`prototype/CoreSession.cpp:435`、`src/main.cpp:666`、`upstream/keepassxc/src/gui/DatabaseWidget.cpp:709`）。

通常文字列、存在しない ID、ごみ箱内のエントリー、ロック後のコピーも試験した（`tests/domain_tests.cpp:45`）。この修正は、Wayland のクリップボード失効保証を完成させたという意味ではない。

## 実行した確認と残る課題

**確認済み。** ビルドと CTest 全13項目が成功した。独立した GNOME Wayland セッションの日本語 UI 操作も成功した。別ビルドの AddressSanitizer / LeakSanitizer では、既存 XML・モデル検査323ケースと今回の80往復に指摘はなかった。参照ツールは無変更コアの通常ビルド、アプリ側コア・セッション・試験は sanitizer 付きビルドを用いる。

再現には通常のビルド後、`ctest --test-dir build --output-on-failure` を使う。メモリ検査は `bash tools/test-reader-memory.sh`。ログは `.cache/known-data.log`、`.cache/ctest.log`、`.cache/reader-asan.log`、`build/ui-test/result.log`。

**未解決。** 公開 fixture 原本2件の拒否制約、Argon2 1.0、キーファイル・YubiKey による解除等の未対応範囲は残る。Browser / SSH Agent / Secret Service / Passkey の合成値は格納されたデータの保持試験であり、それらの機能や実際の認証・鍵データを包括的に試験したものではない。全てのメタデータ・KDF パラメーターの可能な値、全編集操作、全ての KDBX の安全性を保証しない。

次は、読み込み処理のプロセス単位の資源制限と中断を進める。保存・復旧、画面ロック・スリープ、秘密の寿命、VM での deb 導入と実機の最終確認も未完了。新しい依存パッケージの導入、実 DB の使用、既存 KeePassXC の変更、push、実インストールは行っていない。


## その後の手順2での拡張

[読み込み子プロセスの資源制限と中断](reader-process-limits.md)を追加した際に、直接経路と子プロセス経路の両方で本試験を再利用した。Botan の実行用プール数を抑えても Argon2 の保存レーン数を変えないことを検査する4ケースを加え、現在の比較はそれぞれ44パラメーター組合せ・88往復となる。上記40組合せ・80往復は手順1完了時点の結果である。


## 手順3での通常保存の追加

コピー往復88件に加え、同じ44組合せについて通常の上書き保存後のスナップショット比較を追加した。各経路で計132回の保存後比較を行い、参照ソースと以前のコピーのハッシュ維持も確認する（`tests/known_data.cpp:363`）。保存と復旧の方式・故障試験は [専用記録](safe-save-recovery.md) を参照する。


## 手順4での自動復旧コピーの比較

IPC 経路で初期・編集後の自動復旧コピーを各44組合せ、計88件、同じ無変更コアの明示スナップショットで比較した。従来の88コピー往復と44通常保存も維持する。自動復旧処理によって未表示データや履歴・KDF・日時を失わないことを検証する（`tests/known_data.cpp:324`、`:368`）。
