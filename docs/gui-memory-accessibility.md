# GUI のメモリ検査と Orca の確認

2026-11-13、Phase 3 手順7の残件。通常環境へ追加パッケージは導入していない。本家チェックアウトは変更していない。DB・資格情報は合成のみ。

## LSan：判定を細分化する

**確認済み。** 修正済みのダイアログ model 参照に加え、Window の実寿命と終了時の割り当てを調べた。`tests/lifetime_ui_probe.cpp` で、破棄前から登録した GObject 弱参照を使い、各巡のウィンドウ・AdwApplication が実際に finalize されることを確認した。同じプロセスで作成・確定編集・フォーム・コピー・自動ロック・復旧の再認証・無操作ロック・終了を1巡／10巡実行した。

| 抑制なしの終了時検査 | 報告 bytes | allocations | 操作・実破棄 |
| --- | ---: | ---: | --- |
| 1巡 | 2,604,535 | 66,316 | 完了 |
| 10巡 | 2,907,651 | 73,758 | 全10巡で完了 |

ログは `build/gui-memory-audit/cycle1-final/app.log`、`cycle10-complete/app.log`。LSan の生の終了コードは134で、合格・ゼロとは扱わない。`tools/summarize-lsan.py` は全グループの集計と summary の一致を検査し、今回の全報告グループの allocator 直後の caller が `libfontconfig.so.1` にあることを記録した。報告を消したり、ライブラリ名を理由に終了コードを成功へ変換したりしない。

**限定した判断。** 今回の経路では、独自の model / provider / watcher / Window を起点にする新たな割り当て報告は検出していない。GUI 自体の実破棄も確認した。ただし、割り当て元ライブラリだけで所有権や原因を完全に証明することはできない。10巡では1巡より303,116 bytes／7,442 allocations多く、キャッシュ量の変動・累積のどちらかを断定できない。GUI 全体の LSan 判定は **外部フォント層の報告が残るため未合格** とする。「全体でリークなし」とはしない。

### 独立対照と終了処理の実験

アプリを含まない既存 GTK 対照 `tests/gtk_memory_control.cpp` でも報告が出る。`tests/font_memory_control.cpp` の対照では、Fontconfig のリスト作成・破棄・FcFini は報告なし、Pango で合成文字列を描画して font map を shutdown しても320 bytes／3件が残った。

診断用の `tests/gtk_font_shutdown_control.cpp` では GTK 単独で display を閉じ、Pango の既定 map を解放し、FcFini を呼ぶ実験では報告が6,181 bytes／144件まで減った。GTK の実使用 map を shutdown し cairo renderer を使う対照では3,419 bytes／76件で、Fontconfig と Wayland 等の経路が残った。この実験は renderer・終了順が通常経路と異なる。製品へライブラリ全体の強制終了を加える根拠にはせず、製品に新しい Fontconfig 依存や終了時ハックを追加しない。

[Pango の公式 API](https://docs.gtk.org/PangoCairo/method.FontMap.set_default.html) は既定 map がスレッド単位であり、NULLで当該スレッドの参照を解放すると説明する。[Fontconfig の公式 API](https://fontconfig.pages.freedesktop.org/fontconfig/fontconfig-devel/#FcFini) は FcFini 後は再初期化が必要とする。GTK・描画スレッドの全寿命を無視して一律に呼ぶ処理は採用しない。

### 試験側で修正したもの

- compositor がコピー後すぐに所有権を引き取る場合がある。次の tick まで provider を所有し続けることを前提にせず、changed 通知内で実際の provider 公開と消費を観測する。所有権喪失の取消も確認する。ロックによる失効は別の私有 lease で確実に live → revoked を検査する。本体の10秒期限は緩めない。
- GObject 弱参照は破棄前から登録する。診断の途中で破棄済み raw pointer に登録してしまった試験側の UAF は修正し、その実行を合格証拠から除いた。
- 10巡の試験では最初の完了表示で監視を終了せず、10/10完了まで待つ。途中で終了した実行は反復の証拠としない。
- 制限環境で LSan 自身が実行不能だった対照、深い stack 取得でコピー期限を超えた未完走は成功に数えない。

## 実 Orca の検査

**確認済み。** ホストには Orca 50.3、GTK 4.22.4、libadwaita 1.9.1、Fontconfig 2.17.1 が既に導入されていた。`tools/test-ui.sh --orca` は私有 session/system D-Bus、私有 XDG directories、headless GNOME、実 Orca、製品 GUI を使用する。speech-dispatcher backend で実際に生成された `SPEECH OUTPUT` と AT-SPI の役割・フォーカスを検査する。通常のデスクトップや他アプリは記録しない。

Orca 専用設定 `tests/orca_settings.py` は私有データディレクトリにだけ置く。接続しない点字機器の再接続待ちを無効にし、検証ログを行ごとに書き出す。通常の Orca 設定は変更しない。ログのバッファ待ちを音声生成の失敗と誤認しない。

`tests/private_keyboard.py` は私有 XDG/runtime、memory GSettings、同一の session/system bus、専用 Wayland 名を検査した上で、私有 Mutter に入力する。初期 overview から合成アプリの正確な PID を前面へ出すため、headless shell のみに `--unsafe-mode` を指定する。これは製品設定でも実デスクトップの設定でもない。アプリのダンプ抑止は維持する。`tests/ui_orca.py` は Tab 移動、Enter でのボタン操作、Alt+F4 での終了を使う。

### 見つかった製品修正

DB 作成・解除の GtkEntry / GtkPasswordEntry の placeholder だけでは accessible name が空だった。`src/main.cpp` の credentials に、翻訳済みの Database name / Master password / Repeat master password を `GTK_ACCESSIBLE_PROPERTY_LABEL` として追加した。日本語と英語の双方で、意味の異なる入力欄を区別して読み上げる。暗号・KDBX・保存・ロックの処理はこの修正では変更していない。

### 再実行

```bash
source tools/dev-env.sh
cmake --build build --parallel 4
bash tools/test-ui.sh --orca
KPXC_UI_LANGUAGE=en bash tools/test-ui.sh --orca

bash tools/test-font-memory.sh --fontconfig
bash tools/test-font-memory.sh --pango
bash tools/test-font-memory.sh --gtk-shutdown

bash tools/test-reader-memory.sh --lifetime-ui
KPXC_GUI_DETECT_LEAKS=1 bash tools/test-ui.sh --lifetime-asan
# 生ログを保存してから反復する。LSan の非ゼロ終了が残る。
KPXC_GUI_CYCLES=10 KPXC_GUI_DETECT_LEAKS=1 \
  KPXC_UI_PASS='PASS lifetime GUI cycle 10/10:' \
  bash tools/test-ui.sh --lifetime-asan
python3 tools/summarize-lsan.py build/ui-lifetime-asan/app.log
```

私有 D-Bus・GUI・LSan を利用できる実行環境で実行する。検証ログは `build/ui-orca-{ja,en}/`。ここにある資格情報やキー入力は合成のみ。実デスクトップで利用する際に Orca の詳細 debug logging を有効化する手順ではない。

日本語・英語で各13チェックに成功した。その後、下記の日本語の実機聴取を利用者と実施した。分離試験の英語音声生成を、人が実機で英語を聴取した確認とは数えない。

## 回帰試験で発見した別の残件

通常 CTest は一度18/19となり、`gnome_save_tests` の別名保存・その直後の保存に間欠失敗を観測した。単独の再実行は成功したが、30回の反復では再発した。診断では CoreSession 直結経路で rename は成功、保存は SerializationError、元ファイル不変・dirty 維持だった。上流ストリーム暗号の空最終ブロック処理で原因を再現し、理由を利用者へ先に報告して独立パッチを追加した。今回のラベル修正とは独立した問題だった。経緯・適用範囲・決定的な回帰試験は [保存の境界処理](stream-finalization.md) に記録する。


## 保存修正の受入結果

空最終ブロック修正後、通常 CTest 21/21、保存試験50回連続、境界値842チェックの ASan / LSan が成功した。無変更の参照 core でも842チェックを通じて修正前の失敗を期待どおり再現した。過去の間欠失敗の実行は消さず、`.cache/orca-final-ctest.log`、`.cache/orca-save-diagnostic-repeat.log`、`.cache/save-child-diagnostic.log` に記録した。修正後のログは `.cache/stream-fix-ctest.log`、`.cache/stream-fix-save-repeat.log`、`.cache/stream-fix-asan.log`。一時診断ソースは復元し、通常バイナリの診断文字列・stderr 転送が残っていないことを確認した。


## 最新候補と通常環境との区別

入力欄ラベルと保存境界の修正後、GUI の ASan アクセス検査と finalize 確認は成功した。抑制なし LSan では再び2,604,535 bytes／66,316 allocationsを報告し、集計は全件 Fontconfig 起点だった（`build/gui-memory-audit/updated-gui/{access.log,lsan.log,summary.json}`）。これは0件の合格ではない。

`build/dist/accessibility-candidate/` に0.1.0~dev3-1と更新用dev3-2を作成した。パッケージの展開先でのworker・picker試験とソース改変拒否、Ubuntu 26.04 VMでの導入・更新・purge、設定・復旧 sentinel 保持が成功した。VMのログは `.cache/vm-accessibility-result.log`。パッケージ検証の最初の制限環境では私有DBusを作れなかったため終了し、利用可能な環境で全体を再実行した。これを製品のpickerの失敗とは数えない。VM受入時点の通常環境はdev2-1だった。その後、利用者が実機での聴取・操作可能と回答したため、承認済みの範囲でdev3-1へ更新した。追加依存・他パッケージの更新はなく、実行ファイル3件がmanifestと一致した。


## 実機での聴取（日本語・今回の操作範囲で確認済み）

開始時、既存の Orca プロセスはなく、screen-reader-enabled は false だった。通常環境へ0.1.0~dev3-1を導入し、`build/host-final.l6nr7R/installed-dev3.json` に版・実行ファイル hash・合成 fixture hash を記録した。データ保存先を従来の合成専用 state / cache / data に分け、通常の実行時ライブラリで起動した。DB作成画面だけを表示し、まだ新しいDBは作成していない。

実デスクトップでは debug logging を有効化せず、利用者が Super+Alt+S（既存の設定値を読み取り確認）で Orca を開始し、Tab移動と合成入力の聞こえ方を確認する。実機の仮想入力・unsafe-mode・全アプリの詳細ログ取得は使用しない。

実機のDB作成画面で、利用者は Tab／Shift+Tab によりデータベース名・マスターパスワード・確認欄の名前が読まれ、パスワード欄に入力した合成文字 qzv 自体は読まれないと確認した。作成は行わずキャンセルし、入力を消去して保存済み合成DBの確認へ進んだ。

利用者の開き直しの依頼によりアプリを再起動し、同じ保存済み合成DBを選び直した。既存エントリーの編集画面はこちらで準備し、値の変更・保存は行わなかった。利用者は Tab／Shift+Tab でタイトル・ユーザー名・パスワードの3欄を区別して聞けたと確認した。最初に依頼した検索→一覧→編集→ロックの一括手順を完走したと推定せず、項目を分けて確認する。


利用者は編集画面を Esc で閉じ、Ctrl+F の検索欄で Host final を入力して検索でき、Ctrl+L で DB をロックできたと確認した。`tests/host_ui.py verify-lock` も、エントリー・編集 dialog・password field の非表示、worker なし、保存済み合成 DB の hash 不変を確認した。終了時の screen-reader-enabled は false、Orca プロセスもなく、開始前と同じ無効状態だった。こちらから通常環境の読み上げ設定は書き換えていない。

実機は日本語で、DB作成の3入力欄、既存エントリーの3入力欄、パスワードの合成入力、検索、キーボードでの編集終了・ロックの範囲を確認した。すべての画面・すべての支援機器・英語の実機聴取を網羅したとはしない。`build/host-final.l6nr7R/orca-physical-result.json` に聴取と自動確認を区別して記録した。通常環境には検証済みdev3-1と従来の実行時依存3件を維持する。新規依存・本家チェックアウトの変更・本番DBの操作・公開・pushは行っていない。

**この段階の判定：Orca は今回の分離／実機検証範囲で確認済み。生の GUI 全体 LSan は非ゼロ。** 後述の追加調査で報告ごとの所有関係を照合した。実機の聴取成功をメモリ検査の合格へ読み替えない。

検証用アプリの正常終了（exit 0）を確認した。実機の聴取で通常のOrcaを一時使用した以外、実機のグローバル設定はプログラムから変更していない。


## 追加調査後の現在の判定

Fontconfigの相対ポインターと、Mesaの空contextが返す末尾ポインターについて、対応する固定版ソース・最小対照・GUIの実際のownerを照合した。Cairo 1巡／10巡およびVulkan 7巡／10巡で、報告を割り当てごとに説明し、未説明0 bytesだった。Vulkan最終10巡は2,987,115 bytes / 75,698件、うちMesaの49件も生存DBから参照されていた。意図的な380 bytesのリークはCairo・Vulkanの双方で説明済みにせず検出した。通常CTestは21/21成功。

**固定版・検証済み操作の補助診断は合格。生のLSanは134で終了し、0件合格ではない。** library全体のsuppressionや製品の強制終了処理は使っていない。通常環境のパッケージ・描画・Orca設定にも変更はない。詳しい根拠・実行結果・再現手順・未検証範囲は [相対参照・末尾参照の調査](font-offset-lsan.md)。新しい依存版ではハッシュ不一致で診断を拒否し、再調査を必要とする。


## 長時間検証による残件の追加

同一GUIで60分・718巡＋15分ロック待機を完走した。終了時の所有関係照合は未説明0だったが、生存割り当ての評価窓中央値が22.579→26.046 MiBへ増え、Pangoの拡縮フォント保持を主要因と確認した。**長時間メモリ安定性の合格とはしない。** 詳細・対照・限界は [長時間検証の記録](gui-soak-results.md)。製品への回避策や再インストールは今回行っていない。

後続の [キャッシュ制御](font-cache-control.md) では5分間隔の公開APIによる整理を実装し、75分試験で窓中央値の増加を3.468→2.126 MiBへ抑えた。所有関係照合は再び未説明0、生LSan終了134。増加の事前基準が1項目残るため、長時間安定性は未合格。新しい整理後の実機Orca音声は未確認であり、先の音声確認と区別する。
